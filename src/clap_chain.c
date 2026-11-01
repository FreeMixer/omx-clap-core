/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * This file is part of omx-clap-host.
 *
 * omx-clap-host is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * omx-clap-host is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with omx-clap-host.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 */

/*
************************************************************************************************************************
*
* The chain (clap_chain.h, docs/chain.md): the members as the control thread keeps them, in order, and the walk a cycle
* reads, one of two plans. A change builds the spare plan, publishes it with one store and waits for the cycle that may
* still be reading the other to leave, so the cycle reads a plan no control thread writes and takes no lock.
*
************************************************************************************************************************
*/

/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "clap_chain.h"


/*
************************************************************************************************************************
*           LOCAL DATA TYPES
************************************************************************************************************************
*/

/* a member as the control thread keeps it */
struct chain_slot
{
    uint32_t id;
    struct omx_clap_instance *instance;
    omx_clap_chain_process_fn process;
    void *ctx;
    uint32_t in_channels, out_channels;
    uint32_t latency;
    uint32_t bypass;
    uint32_t mono;      // 1 channel in and out inside a stereo chain: the channels summed into it, its output to both
};

/* one member of the walk: an instance or a stage */
struct chain_step
{
    struct omx_clap_instance *instance;
    omx_clap_chain_process_fn process;
    void *ctx;
    uint32_t mono;      // the sum and copy around it; bypassed (no instance, no process) it is the sum and copy alone
};

/* the walk a cycle reads: the non-bypassed members in order, and a bypassed mono member as its sum and copy, since
 * Zynthian's routing sums both channels into its one input and gives its output, the sum when it is bypassed, to both */
struct chain_plan
{
    uint32_t steps;
    struct chain_step step[OMX_CLAP_CHAIN_MEMBERS_MAX];
};

struct omx_clap_chain
{
    uint32_t channels;
    omx_clap_chain_latency_fn latency_changed;
    void *ctx;

    pthread_mutex_t lock;                   // every control-thread call; never the cycle
    struct chain_slot slot[OMX_CLAP_CHAIN_MEMBERS_MAX];
    uint32_t count;
    uint32_t next_id;
    uint32_t reported;                      // the latency latency_changed last said

    /* the two plans, the one the cycles read published in `live`, and the cycle's flag while it reads one */
    struct chain_plan plan[2];
    struct chain_plan *_Atomic live;
    _Atomic uint32_t in_cycle;

    /* what omx_clap_chain_report reads without the lock */
    _Atomic uint32_t members, running, latency, revision;
    _Atomic uint64_t passed;

    /* the two blocks between members, `channels` channels of OMX_CLAP_CHAIN_FRAMES_MAX frames each, one allocation */
    float *block_mem;
    float *block[2][CLAP_HOST_PORT_CHANNELS];
    /* a mono member's input and output, OMX_CLAP_CHAIN_FRAMES_MAX frames each, in the same allocation (a stereo chain) */
    float *mono_in, *mono_out;
};


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS
************************************************************************************************************************
*/

/* a structure's `abi` and `size` are ones the library reads: an ABI it knows, at exactly the size that ABI gives the
 * structure (one ABI so far, its size this build's) */
static int abi_ok(uint32_t abi, uint32_t size, size_t size_abi1)
{
    return abi >= OMX_CLAP_CHAIN_ABI_OLDEST && abi <= OMX_CLAP_CHAIN_ABI && size == size_abi1;
}

/* the member with `id`, its index in slot[], or -1 */
static int slot_find(const struct omx_clap_chain *chain, uint32_t id)
{
    uint32_t i;

    for (i = 0; i < chain->count; i++)
        if (chain->slot[i].id == id)
            return (int)i;
    return -1;
}

/* the sum of the non-bypassed members' latencies */
static uint32_t latency_sum(const struct omx_clap_chain *chain)
{
    uint32_t i, sum = 0;

    for (i = 0; i < chain->count; i++)
        if (!chain->slot[i].bypass)
            sum += chain->slot[i].latency;
    return sum;
}

/* under the lock: the walk of the members as they now stand built into the spare plan, published, and the cycle that
 * may still read the old one waited for. The cycle's enter (store in_cycle, load live) and this (store live, load
 * in_cycle) are sequentially consistent: either that cycle loads the new plan, or this sees its flag and waits for its
 * leave. One audio thread per chain, so once the flag reads 0 no cycle reads the old plan, and the next change may
 * build into it. */
static void publish(struct omx_clap_chain *chain)
{
    struct chain_plan *old = atomic_load_explicit(&chain->live, memory_order_relaxed);
    struct chain_plan *next = old == &chain->plan[0] ? &chain->plan[1] : &chain->plan[0];
    uint32_t i, running;

    next->steps = 0;
    running = 0;
    for (i = 0; i < chain->count; i++)
        if (!chain->slot[i].bypass || chain->slot[i].mono)
        {
            const int runs = !chain->slot[i].bypass;

            next->step[next->steps].instance = runs ? chain->slot[i].instance : NULL;
            next->step[next->steps].process = runs ? chain->slot[i].process : NULL;
            next->step[next->steps].ctx = runs ? chain->slot[i].ctx : NULL;
            next->step[next->steps].mono = chain->slot[i].mono;
            next->steps++;
            running += (uint32_t)runs;
        }
    atomic_store(&chain->live, next);
    while (atomic_load(&chain->in_cycle) != 0u)
        usleep(CLAP_HOST_UNPUBLISH_POLL_US);
    atomic_store_explicit(&chain->members, chain->count, memory_order_relaxed);
    atomic_store_explicit(&chain->running, running, memory_order_relaxed);
    atomic_fetch_add_explicit(&chain->revision, 1u, memory_order_relaxed);
}

/* under the lock: the latency summed again; nonzero when latency_changed is to be told, `*frames` the new sum */
static int latency_update(struct omx_clap_chain *chain, uint32_t *frames)
{
    const uint32_t sum = latency_sum(chain);

    atomic_store_explicit(&chain->latency, sum, memory_order_relaxed);
    *frames = sum;
    if (sum == chain->reported)
        return 0;
    chain->reported = sum;
    return chain->latency_changed != NULL;
}

/* the lock released: the latency told when it changed */
static void unlock_and_tell(struct omx_clap_chain *chain, int tell, uint32_t frames)
{
    pthread_mutex_unlock(&chain->lock);
    if (tell)
        chain->latency_changed(chain->ctx, frames);
}

/* RT: `inputs` copied to `outputs`, the buffers that are not the same */
static void pass_dry(const struct omx_clap_chain *chain, const float *const *inputs, float *const *outputs, uint32_t frames)
{
    uint32_t c;

    for (c = 0; c < chain->channels; c++)
        if (outputs[c] != inputs[c])
            memcpy(outputs[c], inputs[c], (size_t)frames * sizeof(float));
}


/*
************************************************************************************************************************
*           GLOBAL FUNCTIONS
************************************************************************************************************************
*/

int omx_clap_chain_create(const struct omx_clap_chain_config *config, struct omx_clap_chain **out)
{
    struct omx_clap_chain *chain;
    void *mem = NULL;
    uint32_t b, c;

    if (!out)
        return OMX_CLAP_CHAIN_ERR_ARG;
    *out = NULL;
    if (!config)
        return OMX_CLAP_CHAIN_ERR_ARG;
    if (!abi_ok(config->abi, config->size, sizeof(*config)))
        return OMX_CLAP_CHAIN_ERR_ABI;
    if (config->channels == 0 || config->channels > CLAP_HOST_PORT_CHANNELS)
        return OMX_CLAP_CHAIN_ERR_WIDTH;
    chain = calloc(1, sizeof(*chain));
    if (!chain)
        return OMX_CLAP_CHAIN_ERR_MEMORY;
    // a stereo chain's two blocks are followed by a mono member's input and output
    if (posix_memalign(&mem, OMX_CLAP_CHAIN_ALIGN, (2u * config->channels + (config->channels == 2u ? 2u : 0u)) * OMX_CLAP_CHAIN_FRAMES_MAX * sizeof(float)) != 0)
    {
        free(chain);
        return OMX_CLAP_CHAIN_ERR_MEMORY;
    }
    memset(mem, 0, (2u * config->channels + (config->channels == 2u ? 2u : 0u)) * OMX_CLAP_CHAIN_FRAMES_MAX * sizeof(float));
    chain->block_mem = mem;
    for (b = 0; b < 2; b++)
        for (c = 0; c < config->channels; c++)
            chain->block[b][c] = chain->block_mem + ((size_t)b * config->channels + c) * OMX_CLAP_CHAIN_FRAMES_MAX;
    if (config->channels == 2u)
    {
        chain->mono_in = chain->block_mem + (size_t)4u * OMX_CLAP_CHAIN_FRAMES_MAX;
        chain->mono_out = chain->mono_in + OMX_CLAP_CHAIN_FRAMES_MAX;
    }
    chain->channels = config->channels;
    chain->latency_changed = config->latency_changed;
    chain->ctx = config->ctx;
    chain->next_id = 1;
    pthread_mutex_init(&chain->lock, NULL);
    atomic_init(&chain->live, &chain->plan[0]);
    *out = chain;
    return OMX_CLAP_CHAIN_OK;
}

void omx_clap_chain_destroy(struct omx_clap_chain *chain)
{
    if (!chain)
        return;
    pthread_mutex_destroy(&chain->lock);
    free(chain->block_mem);
    free(chain);
}

int omx_clap_chain_add(struct omx_clap_chain *chain, const struct omx_clap_chain_member *member, uint32_t position, uint32_t *id)
{
    struct chain_slot slot;
    uint32_t frames;
    int tell;

    if (!chain || !member)
        return OMX_CLAP_CHAIN_ERR_ARG;
    if (!abi_ok(member->abi, member->size, sizeof(*member)))
        return OMX_CLAP_CHAIN_ERR_ABI;
    if ((member->instance == NULL) == (member->process == NULL))
        return OMX_CLAP_CHAIN_ERR_ARG;
    memset(&slot, 0, sizeof(slot));
    slot.instance = member->instance;
    slot.process = member->process;
    slot.ctx = member->instance ? NULL : member->ctx;
    slot.in_channels = member->instance ? member->instance->main_in.channels : member->in_channels;
    slot.out_channels = member->instance ? member->instance->main_out.channels : member->out_channels;
    slot.latency = member->instance ? omx_clap_host_latency(member->instance) : member->latency;
    slot.bypass = member->bypass != 0;
    // one channel in and out inside a stereo chain runs as Zynthian routes it alone: the channels summed into it, its
    // output given to both. Any other width than the chain's is refused, and the run ends there
    slot.mono = chain->channels == 2u && slot.in_channels == 1u && slot.out_channels == 1u;
    if (!slot.mono && (slot.in_channels != chain->channels || slot.out_channels != chain->channels))
        return OMX_CLAP_CHAIN_ERR_WIDTH;

    pthread_mutex_lock(&chain->lock);
    if (position == OMX_CLAP_CHAIN_END)
        position = chain->count;
    if (position > chain->count)
    {
        pthread_mutex_unlock(&chain->lock);
        return OMX_CLAP_CHAIN_ERR_ARG;
    }
    if (chain->count == OMX_CLAP_CHAIN_MEMBERS_MAX)
    {
        pthread_mutex_unlock(&chain->lock);
        return OMX_CLAP_CHAIN_ERR_FULL;
    }
    slot.id = chain->next_id++;
    memmove(&chain->slot[position + 1], &chain->slot[position], (chain->count - position) * sizeof(chain->slot[0]));
    chain->slot[position] = slot;
    chain->count++;
    publish(chain);
    if (id)
        *id = slot.id;
    tell = latency_update(chain, &frames);
    unlock_and_tell(chain, tell, frames);
    return OMX_CLAP_CHAIN_OK;
}

int omx_clap_chain_remove(struct omx_clap_chain *chain, uint32_t id)
{
    uint32_t frames;
    int i, tell;

    if (!chain)
        return OMX_CLAP_CHAIN_ERR_ARG;
    pthread_mutex_lock(&chain->lock);
    i = slot_find(chain, id);
    if (i < 0)
    {
        pthread_mutex_unlock(&chain->lock);
        return OMX_CLAP_CHAIN_ERR_NO_MEMBER;
    }
    // the member before it now hands its output to the one after it: the walk is built from the members left
    memmove(&chain->slot[i], &chain->slot[i + 1], (chain->count - (uint32_t)i - 1u) * sizeof(chain->slot[0]));
    chain->count--;
    publish(chain);
    tell = latency_update(chain, &frames);
    unlock_and_tell(chain, tell, frames);
    return OMX_CLAP_CHAIN_OK;
}

int omx_clap_chain_order(struct omx_clap_chain *chain, const uint32_t *ids, uint32_t n)
{
    struct chain_slot slot[OMX_CLAP_CHAIN_MEMBERS_MAX];
    uint32_t taken[OMX_CLAP_CHAIN_MEMBERS_MAX];
    uint32_t i, frames;
    int k, tell;

    if (!chain || (!ids && n))
        return OMX_CLAP_CHAIN_ERR_ARG;
    pthread_mutex_lock(&chain->lock);
    if (n != chain->count)
    {
        pthread_mutex_unlock(&chain->lock);
        return OMX_CLAP_CHAIN_ERR_ARG;
    }
    // the new order built aside, every id one member's and none twice, before the members change at all
    memset(taken, 0, sizeof(taken));
    for (i = 0; i < n; i++)
    {
        k = slot_find(chain, ids[i]);
        if (k < 0 || taken[k])
        {
            pthread_mutex_unlock(&chain->lock);
            return k < 0 ? OMX_CLAP_CHAIN_ERR_NO_MEMBER : OMX_CLAP_CHAIN_ERR_ARG;
        }
        taken[k] = 1;
        slot[i] = chain->slot[k];
    }
    memcpy(chain->slot, slot, n * sizeof(slot[0]));
    publish(chain);
    tell = latency_update(chain, &frames);
    unlock_and_tell(chain, tell, frames);
    return OMX_CLAP_CHAIN_OK;
}

int omx_clap_chain_bypass(struct omx_clap_chain *chain, uint32_t id, int bypass)
{
    uint32_t frames;
    int i, tell;

    if (!chain)
        return OMX_CLAP_CHAIN_ERR_ARG;
    pthread_mutex_lock(&chain->lock);
    i = slot_find(chain, id);
    if (i < 0)
    {
        pthread_mutex_unlock(&chain->lock);
        return OMX_CLAP_CHAIN_ERR_NO_MEMBER;
    }
    if (chain->slot[i].bypass != (uint32_t)(bypass != 0))
    {
        chain->slot[i].bypass = bypass != 0;
        publish(chain);
    }
    tell = latency_update(chain, &frames);
    unlock_and_tell(chain, tell, frames);
    return OMX_CLAP_CHAIN_OK;
}

int omx_clap_chain_member_latency_set(struct omx_clap_chain *chain, uint32_t id, uint32_t frames)
{
    uint32_t sum;
    int i, tell;

    if (!chain)
        return OMX_CLAP_CHAIN_ERR_ARG;
    pthread_mutex_lock(&chain->lock);
    i = slot_find(chain, id);
    if (i < 0 || chain->slot[i].instance)
    {
        pthread_mutex_unlock(&chain->lock);
        return i < 0 ? OMX_CLAP_CHAIN_ERR_NO_MEMBER : OMX_CLAP_CHAIN_ERR_ARG;
    }
    chain->slot[i].latency = frames;
    tell = latency_update(chain, &sum);
    unlock_and_tell(chain, tell, sum);
    return OMX_CLAP_CHAIN_OK;
}

int omx_clap_chain_refresh(struct omx_clap_chain *chain)
{
    uint32_t i, frames;
    int tell;

    if (!chain)
        return OMX_CLAP_CHAIN_ERR_ARG;
    pthread_mutex_lock(&chain->lock);
    for (i = 0; i < chain->count; i++)
        if (chain->slot[i].instance)
            chain->slot[i].latency = omx_clap_host_latency(chain->slot[i].instance);
    tell = latency_update(chain, &frames);
    unlock_and_tell(chain, tell, frames);
    return OMX_CLAP_CHAIN_OK;
}

int omx_clap_chain_report(const struct omx_clap_chain *chain, struct omx_clap_chain_report *out)
{
    if (!chain || !out)
        return OMX_CLAP_CHAIN_ERR_ARG;
    if (!abi_ok(out->abi, out->size, sizeof(*out)))
        return OMX_CLAP_CHAIN_ERR_ABI;
    out->channels = chain->channels;
    out->members = atomic_load_explicit(&chain->members, memory_order_relaxed);
    out->running = atomic_load_explicit(&chain->running, memory_order_relaxed);
    out->latency = atomic_load_explicit(&chain->latency, memory_order_relaxed);
    out->revision = atomic_load_explicit(&chain->revision, memory_order_relaxed);
    out->passed = atomic_load_explicit(&chain->passed, memory_order_relaxed);
    return OMX_CLAP_CHAIN_OK;
}

int omx_clap_chain_member_info(struct omx_clap_chain *chain, uint32_t position, struct omx_clap_chain_member_info *out)
{
    const struct chain_slot *slot;

    if (!chain || !out)
        return OMX_CLAP_CHAIN_ERR_ARG;
    if (!abi_ok(out->abi, out->size, sizeof(*out)))
        return OMX_CLAP_CHAIN_ERR_ABI;
    pthread_mutex_lock(&chain->lock);
    if (position >= chain->count)
    {
        pthread_mutex_unlock(&chain->lock);
        return OMX_CLAP_CHAIN_ERR_NO_MEMBER;
    }
    slot = &chain->slot[position];
    out->id = slot->id;
    out->position = position;
    out->in_channels = slot->in_channels;
    out->out_channels = slot->out_channels;
    out->latency = slot->latency;
    out->bypass = slot->bypass;
    out->instance = slot->instance;
    out->ctx = slot->ctx;
    pthread_mutex_unlock(&chain->lock);
    return OMX_CLAP_CHAIN_OK;
}

/* a mono member's step, on the audio thread: both channels summed into its input, as JACK sums two links into one port,
 * the member run on it (or its input passed, bypassed), its output given to both channels. No allocation, no lock */
static void run_mono(struct omx_clap_chain *chain, const struct chain_step *step, const float *const *from, float *const *to,
                     uint32_t frames)
{
    float *in = chain->mono_in, *out = chain->mono_out;
    const float *const ins[1] = { in };
    float *const outs[1] = { out };
    uint32_t i;

    for (i = 0; i < frames; i++)
        in[i] = from[0][i] + from[1][i];
    if (step->instance)
        omx_clap_host_run_ports(step->instance, ins, outs, frames);
    else if (step->process)
        step->process(step->ctx, ins, outs, frames);
    else
        memcpy(out, in, frames * sizeof(float));
    memcpy(to[0], out, frames * sizeof(float));
    memcpy(to[1], out, frames * sizeof(float));
}

int omx_clap_chain_process(struct omx_clap_chain *chain, const float *const *inputs, float *const *outputs, uint32_t frames)
{
    const struct chain_plan *plan;
    const float *const *from;
    float *const *to = outputs;
    uint32_t s;

    if (!chain || !inputs || !outputs)
        return OMX_CLAP_CHAIN_ERR_ARG;
    // a cycle longer than the blocks: the chain's input to its output, no member run
    if (frames > OMX_CLAP_CHAIN_FRAMES_MAX)
    {
        pass_dry(chain, inputs, outputs, frames);
        atomic_fetch_add_explicit(&chain->passed, 1u, memory_order_relaxed);
        return OMX_CLAP_CHAIN_PASSED;
    }
    atomic_store(&chain->in_cycle, 1u);
    plan = atomic_load(&chain->live);
    if (plan->steps == 0)
        pass_dry(chain, inputs, outputs, frames);
    // the first member reads the chain's input, the last writes its output, each between them the block the member
    // before it wrote, the two blocks taken in turn so none is handed its input as its output
    for (s = 0, from = inputs; s < plan->steps; s++, from = (const float *const *)to)
    {
        const struct chain_step *step = &plan->step[s];

        to = s + 1 == plan->steps ? outputs : chain->block[s & 1u];
        if (step->mono)
            run_mono(chain, step, from, to, frames);
        else if (step->instance)
            omx_clap_host_run_ports(step->instance, from, to, frames);
        else
            step->process(step->ctx, from, to, frames);
    }
    atomic_store_explicit(&chain->in_cycle, 0u, memory_order_release);
    return OMX_CLAP_CHAIN_OK;
}
