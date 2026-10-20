// SPDX-License-Identifier: GPL-3.0-or-later
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
 * cycle_gate_test: omx-clap-host's restart and relayout (effects.c clap_idle, ports_relayout) against a cycle thread
 * that never stops, as a JACK client's does not: the stereo-to-mono fake, whose right leg is a bounce block the
 * deactivate unmaps, restarted and re-described through cycle_gate.h while the cycle enters the gate, reads the active
 * flag and runs omx_clap_host_run_ports as effects.c process() does. Built with -fsanitize=thread (make
 * test-transport-tsan): a cycle reading the stage, the bounce or the flag while the control thread clears them is a
 * report, and the run halts on it. A block-size change (effects.c buffer_size) racing a relayout that leaves the
 * instance inactive leaves it inactive.
 *
 * usage: cycle_gate_test <fake.clap>
 */

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "clap_host.h"
#include "cycle_gate.h"

#define BLOCK       64u
#define SAMPLE_RATE 48000.0
#define ROUNDS      40u
#define STEREO_MONO "org.omx-clap-host.test.stereo-mono"

static int g_failures;

#define CHECK(cond, ...)                         \
    do                                           \
    {                                            \
        if (cond)                                \
            printf("ok   ");                     \
        else                                     \
        {                                        \
            printf("FAIL ");                     \
            g_failures++;                        \
        }                                        \
        printf(__VA_ARGS__);                     \
        printf("\n");                            \
    } while (0)

struct rig {
    struct omx_clap_instance *in;
    struct omx_cycle_gate gate;
    _Atomic int stop;
    _Atomic uint32_t cycles;
    _Atomic uint32_t nonfinite;
};

/* effects.c process(), its ports two in and one out: the gate entered, the flag and a recorded size read, the stage run
 * or the lane dry */
static void *cycles_run(void *arg)
{
    struct rig *r = arg;
    float in_l[BLOCK], in_r[BLOCK], out[BLOCK];
    const float *ins[2] = { in_l, in_r };
    float *outs[1] = { out };
    uint32_t i;

    for (i = 0; i < BLOCK; i++)
        in_l[i] = 0.25f, in_r[i] = -0.125f;
    while (!atomic_load(&r->stop))
    {
        if (omx_cycle_gate_enter(&r->gate) && __atomic_load_n(&r->in->active, __ATOMIC_ACQUIRE) && !omx_cycle_gate_resizing(&r->gate))
            omx_clap_host_run_ports(r->in, ins, outs, BLOCK);
        else
            memcpy(out, in_l, sizeof(out));
        omx_cycle_gate_leave(&r->gate);
        for (i = 0; i < BLOCK; i++)
            if (!isfinite(out[i]))
            {
                atomic_fetch_add(&r->nonfinite, 1u);
                break;
            }
        atomic_fetch_add(&r->cycles, 1u);
        usleep(20);
    }
    return NULL;
}

/* wait until the cycle thread ran `n` more cycles */
static void cycles_wait(struct rig *r, uint32_t n)
{
    const uint32_t from = atomic_load(&r->cycles);
    unsigned waited = 0;

    while (atomic_load(&r->cycles) - from < n && waited < 2000000u)
    {
        usleep(100);
        waited += 100;
    }
}

/* ports_relayout's body: the cycles shut out, the description read again, activated with the cycle's audio role, armed */
static int relayout(struct rig *r, pthread_t thread)
{
    char why[OMX_CLAP_WHY_MAX] = "";
    int ok;

    if (omx_cycle_gate_deactivate(&r->gate, r->in, CLAP_HOST_ROLE_POLL_US, CLAP_HOST_ROLE_TIMEOUT_US) != 0)
        return 0;
    ok = omx_clap_host_request_layout(r->in, NULL, 0, why) >= 0 && omx_clap_host_activate(r->in, SAMPLE_RATE, BLOCK, why) == 0;
    if (ok)
    {
        omx_clap_host_set_audio_thread(r->in, thread);
        omx_clap_host_arm(r->in);
    }
    omx_cycle_gate_open(&r->gate);
    return ok;
}

/* one arm: `knob` set for the fake (NULL none), ROUNDS restarts, each followed by a relayout when it answered
 * OMX_CLAP_RESTART_RESCANNED and by a relayout of the active instance otherwise, the cycle thread running throughout */
static void arm(const char *path, const char *knob, int want_rescanned)
{
    struct rig r;
    char why[OMX_CLAP_WHY_MAX] = "";
    pthread_t t;
    uint32_t i, restarted = 0, rescanned = 0, failed = 0, relaid = 0, runs;

    memset(&r, 0, sizeof(r));
    if (knob)
        setenv(knob, "1", 1);
    CHECK(omx_clap_host_open(path, STEREO_MONO, &r.in, why) == 0 && r.in, "%s opens (%s)", STEREO_MONO, knob ? knob : "no knob");
    if (knob)
        unsetenv(knob);
    if (!r.in)
        return;
    CHECK(omx_clap_host_activate(r.in, SAMPLE_RATE, BLOCK, why) == 0, "it activates (%s)", why);
    (void)omx_clap_host_ports_rescanned(r.in);
    if (pthread_create(&t, NULL, cycles_run, &r) != 0)
    {
        CHECK(0, "the cycle thread starts");
        omx_clap_host_close(r.in);
        return;
    }
    omx_clap_host_publish(r.in, t);
    cycles_wait(&r, 8);
    for (i = 0; i < ROUNDS; i++)
    {
        const int ret = omx_cycle_gate_restart(&r.gate, r.in, CLAP_HOST_ROLE_POLL_US, CLAP_HOST_ROLE_TIMEOUT_US);

        if (ret == 0)
            restarted++;
        else if (ret == OMX_CLAP_RESTART_RESCANNED)
            rescanned++;
        else
            failed++;
        cycles_wait(&r, 2);
        (void)omx_clap_host_ports_rescanned(r.in);
        relaid += relayout(&r, t);
        cycles_wait(&r, 2);
    }
    runs = atomic_load(&r.in->stage.h.runs);
    cycles_wait(&r, 8);
    CHECK(failed == 0 && (want_rescanned ? rescanned == ROUNDS : restarted == ROUNDS),
          "%u restarts under running cycles: %u restarted, %u stopped by a rescan, %u failed", ROUNDS, restarted, rescanned, failed);
    CHECK(relaid == ROUNDS && r.in->active, "and %u relayouts under them, each armed again (%u)", ROUNDS, relaid);
    CHECK(atomic_load(&r.in->stage.h.runs) > runs, "the plugin runs on the cycle thread after them (%u, then %u)", runs,
          atomic_load(&r.in->stage.h.runs));
    CHECK(atomic_load(&r.nonfinite) == 0, "no cycle wrote a non-finite sample (%u cycles)", atomic_load(&r.cycles));
    atomic_store(&r.stop, 1);
    pthread_join(t, NULL);
    omx_clap_host_stop(r.in);
    omx_clap_host_close(r.in);
}

/* the carrier's blocks as effects.c keeps its auxiliary ones: stale at another length, made again by the hook, which
 * counts its calls, checks the cycles are shut out, and fails on demand */
struct blocks {
    struct rig *r;
    uint32_t frames;
    int calls, open_calls, fail;
    uint32_t record;    // a size the carrier records while the hook runs (a change during the tick); 0: none
};

static int blocks_stale(void *ctx, uint32_t nframes)
{
    return ((struct blocks *)ctx)->frames != nframes;
}

static int blocks_reblock(void *ctx, uint32_t nframes)
{
    struct blocks *b = ctx;

    b->calls++;
    if (atomic_load(&b->r->gate.closed) == 0u)
        b->open_calls++;
    if (b->record)
        omx_cycle_gate_resize_record(&b->r->gate, b->record);
    if (b->fail)
        return -1;
    b->frames = nframes;
    return 0;
}

struct resize_call {
    struct rig *r;
    struct omx_cycle_gate_blocks *blocks;
    struct omx_cycle_gate_pending *pending;
    pthread_mutex_t *lock;
    uint32_t nframes;
    _Atomic int started;
    int ret;
};

/* effects.c buffer_size on a thread of its own, as JACK calls it */
static void *resize_run(void *arg)
{
    struct resize_call *c = arg;

    atomic_store(&c->started, 1);
    char why[OMX_CLAP_WHY_MAX] = "";

    c->ret = omx_cycle_gate_resize(&c->r->gate, c->r->in, c->lock, SAMPLE_RATE, c->nframes, CLAP_HOST_ROLE_POLL_US, c->blocks,
                                   c->pending, why);
    return NULL;
}

/* A block-size change racing a relayout that leaves the instance inactive (its layout refused, its ports no longer the
 * carrier's): the change waits for the relayout's lock, then finds the instance inactive and leaves it so, never
 * activating it on a description nobody read again; an active instance is re-activated at the new size, and runs */
static void resize_checks(const char *path)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    struct rig r;
    struct resize_call c;
    struct blocks b = { &r, BLOCK, 0, 0, 0, 0 };
    struct omx_cycle_gate_blocks hook = { blocks_stale, blocks_reblock, &b };
    struct omx_cycle_gate_pending pending = { 0, 0, 0 };
    char why[OMX_CLAP_WHY_MAX] = "";
    pthread_t t, rt;
    uint32_t runs;

    memset(&r, 0, sizeof(r));
    CHECK(omx_clap_host_open(path, STEREO_MONO, &r.in, why) == 0 && r.in, "resize: %s opens", STEREO_MONO);
    if (!r.in)
        return;
    CHECK(omx_clap_host_activate(r.in, SAMPLE_RATE, BLOCK, why) == 0, "resize: it activates (%s)", why);
    if (pthread_create(&t, NULL, cycles_run, &r) != 0)
    {
        omx_clap_host_close(r.in);
        return;
    }
    omx_clap_host_publish(r.in, t);
    cycles_wait(&r, 8);

    // the relayout holds the lock while the block size changes; it leaves the instance inactive
    memset(&c, 0, sizeof(c));
    c.r = &r;
    c.lock = &lock;
    c.nframes = 2 * BLOCK;
    c.blocks = &hook;
    c.pending = &pending;
    pthread_mutex_lock(&lock);
    if (pthread_create(&rt, NULL, resize_run, &c) == 0)
    {
        while (!atomic_load(&c.started))
            usleep(100);
        usleep(50000);  // the change is waiting on the lock, the instance still active
        CHECK(omx_cycle_gate_deactivate(&r.gate, r.in, CLAP_HOST_ROLE_POLL_US, CLAP_HOST_ROLE_TIMEOUT_US) == 0,
              "resize: the relayout deactivates the instance under the lock");
        omx_cycle_gate_open(&r.gate);
        pthread_mutex_unlock(&lock);
        pthread_join(rt, NULL);
        CHECK(c.ret == OMX_CYCLE_GATE_UNCHANGED && !r.in->active && !pending.set,
              "resize: the change that waited for it leaves the instance inactive (%d, active %d)", c.ret, r.in->active);
        CHECK(b.calls == 0 && b.frames == BLOCK,
              "resize: a dead instance (inactive, nothing pending) has its stale blocks left alone (%d calls)", b.calls);
    }
    else
        pthread_mutex_unlock(&lock);

    // the relayout's own activate, then a change on the active instance: re-activated at the new size
    pthread_mutex_lock(&lock);
    omx_cycle_gate_close(&r.gate, CLAP_HOST_ROLE_POLL_US);
    CHECK(omx_clap_host_activate(r.in, SAMPLE_RATE, BLOCK, why) == 0, "resize: the relayout activates it (%s)", why);
    omx_clap_host_set_audio_thread(r.in, t);
    omx_clap_host_arm(r.in);
    omx_cycle_gate_open(&r.gate);
    pthread_mutex_unlock(&lock);
    cycles_wait(&r, 4);
    CHECK(omx_cycle_gate_resize(&r.gate, r.in, &lock, SAMPLE_RATE, 2 * BLOCK, CLAP_HOST_ROLE_POLL_US, &hook, &pending, why)
              == OMX_CYCLE_GATE_RESIZED && r.in->active && r.in->max_block == 2 * BLOCK && b.calls == 1 && b.open_calls == 0
              && b.frames == 2 * BLOCK,
          "resize: an active instance is re-activated at the new size, its stale blocks made again once with the cycles shut out "
          "(%u, %d calls, %d open)", r.in->max_block, b.calls, b.open_calls);
    runs = atomic_load(&r.in->stage.h.runs);
    cycles_wait(&r, 8);
    CHECK(atomic_load(&r.in->stage.h.runs) > runs, "resize: and runs on the cycle thread after it");

    // the blocks cannot be made: the instance left inactive and pending, its role saved; the next change activates it
    b.fail = 1;
    CHECK(omx_cycle_gate_resize(&r.gate, r.in, &lock, SAMPLE_RATE, 4 * BLOCK, CLAP_HOST_ROLE_POLL_US, &hook, &pending, why)
              == OMX_CYCLE_GATE_REBLOCK_FAILED && !r.in->active && pending.set && b.open_calls == 0,
          "resize: a failed reblock leaves the instance inactive and pending (active %d, pending %d)", r.in->active, pending.set);
    b.fail = 0;
    CHECK(omx_cycle_gate_resize(&r.gate, r.in, &lock, SAMPLE_RATE, 4 * BLOCK, CLAP_HOST_ROLE_POLL_US, &hook, &pending, why)
              == OMX_CYCLE_GATE_RESIZED && r.in->active && !pending.set && r.in->max_block == 4 * BLOCK && b.frames == 4 * BLOCK,
          "resize: the next change makes the blocks and activates the pending instance (%u)", r.in->max_block);
    runs = atomic_load(&r.in->stage.h.runs);
    cycles_wait(&r, 8);
    CHECK(atomic_load(&r.in->stage.h.runs) > runs, "resize: and it runs on the cycle thread, the saved role given back");

    // a relayout that waits for the blocks (pending, its role saved): a change with nothing stale activates it
    pthread_mutex_lock(&lock);
    CHECK(omx_cycle_gate_deactivate(&r.gate, r.in, CLAP_HOST_ROLE_POLL_US, CLAP_HOST_ROLE_TIMEOUT_US) == 0, "resize: a relayout deactivates");
    pending = (struct omx_cycle_gate_pending){ 1, t, 1 };
    omx_cycle_gate_open(&r.gate);
    pthread_mutex_unlock(&lock);
    CHECK(omx_cycle_gate_resize(&r.gate, r.in, &lock, SAMPLE_RATE, 4 * BLOCK, CLAP_HOST_ROLE_POLL_US, &hook, &pending, why)
              == OMX_CYCLE_GATE_RESIZED && r.in->active && !pending.set,
          "resize: a pending relayout is activated by the change, the size unchanged (active %d)", r.in->active);
    runs = atomic_load(&r.in->stage.h.runs);
    cycles_wait(&r, 8);
    CHECK(atomic_load(&r.in->stage.h.runs) > runs, "resize: and runs on the cycle thread after it");

    // the carrier records the sizes on its own thread (twice before a tick, the last one wins) and the cycles pass dry
    // meanwhile; the control thread's tick applies the last and clears the record
    {
        uint32_t applied = 0, cycles;

        omx_cycle_gate_resize_record(&r.gate, 2 * BLOCK);
        omx_cycle_gate_resize_record(&r.gate, 8 * BLOCK);
        runs = atomic_load(&r.in->stage.h.runs);
        cycles = atomic_load(&r.cycles);
        cycles_wait(&r, 8);
        CHECK(omx_cycle_gate_resizing(&r.gate) && atomic_load(&r.in->stage.h.runs) == runs && atomic_load(&r.cycles) > cycles,
              "resize tick: a recorded size, the cycles pass dry until the tick (%u runs)", atomic_load(&r.in->stage.h.runs) - runs);
        CHECK(omx_cycle_gate_resize_tick(&r.gate, r.in, &lock, SAMPLE_RATE, CLAP_HOST_ROLE_POLL_US, &hook, &pending, why, &applied)
                  == OMX_CYCLE_GATE_RESIZED && applied == 8 * BLOCK && r.in->max_block == 8 * BLOCK && b.frames == 8 * BLOCK,
              "resize tick: the tick applies the last of two recorded sizes (%u, %u)", applied, r.in->max_block);
        CHECK(!omx_cycle_gate_resizing(&r.gate), "resize tick: and clears the record");
        runs = atomic_load(&r.in->stage.h.runs);
        cycles_wait(&r, 8);
        CHECK(atomic_load(&r.in->stage.h.runs) > runs, "resize tick: the cycles run the plugin again after it");
        CHECK(omx_cycle_gate_resize_tick(&r.gate, r.in, &lock, SAMPLE_RATE, CLAP_HOST_ROLE_POLL_US, &hook, &pending, why, &applied)
                  == OMX_CYCLE_GATE_UNCHANGED && applied == 0, "resize tick: with nothing recorded, nothing done");

        // a size the carrier records while the tick resizes (between its load and its clear) survives it: the next tick
        // applies that one
        omx_cycle_gate_resize_record(&r.gate, 2 * BLOCK);
        b.record = 4 * BLOCK;
        CHECK(omx_cycle_gate_resize_tick(&r.gate, r.in, &lock, SAMPLE_RATE, CLAP_HOST_ROLE_POLL_US, &hook, &pending, why, &applied)
                  == OMX_CYCLE_GATE_RESIZED && applied == 2 * BLOCK && r.in->max_block == 2 * BLOCK,
              "resize tick: the recorded size applied (%u) while another is recorded during it", applied);
        b.record = 0;
        CHECK(omx_cycle_gate_resizing(&r.gate), "resize tick: the size recorded during the tick is still recorded after it");
        CHECK(omx_cycle_gate_resize_tick(&r.gate, r.in, &lock, SAMPLE_RATE, CLAP_HOST_ROLE_POLL_US, &hook, &pending, why, &applied)
                  == OMX_CYCLE_GATE_RESIZED && applied == 4 * BLOCK && r.in->max_block == 4 * BLOCK && !omx_cycle_gate_resizing(&r.gate),
              "resize tick: the next tick applies it and clears the record (%u)", applied);
    }
    atomic_store(&r.stop, 1);
    pthread_join(t, NULL);
    omx_clap_host_stop(r.in);
    omx_clap_host_close(r.in);
}

/* A size change on a plugin that rescans its ports in every deactivate (FAKE_RESCAN_ON_DEACTIVATE): the resize leaves it
 * inactive with the rescan reported once and nothing pending, its blocks made at the new size; the relayout that reads
 * the description again activates it, with the role the cycles held, and it runs */
static void resize_rescan_checks(const char *path)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    struct rig r;
    struct blocks b = { &r, BLOCK, 0, 0, 0, 0 };
    struct omx_cycle_gate_blocks hook = { blocks_stale, blocks_reblock, &b };
    struct omx_cycle_gate_pending pending = { 0, 0, 0 };
    char why[OMX_CLAP_WHY_MAX] = "";
    pthread_t t;
    uint32_t runs;
    int ret;

    memset(&r, 0, sizeof(r));
    setenv("FAKE_RESCAN_ON_DEACTIVATE", "1", 1);
    CHECK(omx_clap_host_open(path, STEREO_MONO, &r.in, why) == 0 && r.in, "resize rescan: %s opens", STEREO_MONO);
    unsetenv("FAKE_RESCAN_ON_DEACTIVATE");
    if (!r.in)
        return;
    CHECK(omx_clap_host_activate(r.in, SAMPLE_RATE, BLOCK, why) == 0, "resize rescan: it activates (%s)", why);
    (void)omx_clap_host_ports_rescanned(r.in);
    if (pthread_create(&t, NULL, cycles_run, &r) != 0)
    {
        omx_clap_host_close(r.in);
        return;
    }
    omx_clap_host_publish(r.in, t);
    cycles_wait(&r, 8);
    ret = omx_cycle_gate_resize(&r.gate, r.in, &lock, SAMPLE_RATE, 2 * BLOCK, CLAP_HOST_ROLE_POLL_US, &hook, &pending, why);
    CHECK(ret == OMX_CYCLE_GATE_RESCANNED && !r.in->active && !pending.set,
          "resize rescan: the rescan in the deactivate leaves the instance inactive, nothing pending (%d, active %d, pending %d)", ret,
          r.in->active, pending.set);
    CHECK(b.calls == 1 && b.open_calls == 0 && b.frames == 2 * BLOCK,
          "resize rescan: its blocks made at the new size all the same, the cycles shut out (%d calls, %u)", b.calls, b.frames);
    CHECK(omx_clap_host_ports_rescanned(r.in) == 1 && omx_clap_host_ports_rescanned(r.in) == 0, "resize rescan: the rescan reported once");
    // the relayout: the description read again, then the activate at the new size with the role the cycles held
    pthread_mutex_lock(&lock);
    omx_cycle_gate_close(&r.gate, CLAP_HOST_ROLE_POLL_US);
    CHECK(omx_clap_host_activate(r.in, SAMPLE_RATE, 2 * BLOCK, why) == 0 && r.in->audio_role_held && r.in->audio_thread == t,
          "resize rescan: the relayout activates it, the cycles' role kept (%s)", why);
    omx_clap_host_set_audio_thread(r.in, r.in->audio_thread);
    omx_clap_host_arm(r.in);
    omx_cycle_gate_open(&r.gate);
    pthread_mutex_unlock(&lock);
    runs = atomic_load(&r.in->stage.h.runs);
    cycles_wait(&r, 8);
    CHECK(atomic_load(&r.in->stage.h.runs) > runs, "resize rescan: and it runs on the cycle thread after it");
    atomic_store(&r.stop, 1);
    pthread_join(t, NULL);
    omx_clap_host_stop(r.in);
    omx_clap_host_close(r.in);
}

int main(int argc, char **argv)
{
    struct omx_clap_host_config config;

    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <fake.clap>\n", argv[0]);
        return 2;
    }
    // as omx-clap-host configures the core: a JACK port per main channel
    omx_clap_host_config_default(&config);
    config.lane_channels = 0;
    if (omx_clap_host_configure(&config) != 0)
    {
        fprintf(stderr, "the core refuses the configuration\n");
        return 1;
    }
    arm(argv[1], NULL, 0);
    arm(argv[1], "FAKE_RESCAN_ON_DEACTIVATE", 1);
    resize_checks(argv[1]);
    resize_rescan_checks(argv[1]);
    if (g_failures)
    {
        printf("FAILED\n");
        return 1;
    }
    printf("cycle gate test ok\n");
    return 0;
}
