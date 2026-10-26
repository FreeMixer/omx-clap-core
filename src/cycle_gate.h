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

/** @file
 * @brief The gate between a carrier's audio cycles and its control thread's deactivate. */

/*
 * cycle_gate.h: a carrier whose cycles keep running while its control thread restarts or re-describes a plugin (a JACK
 * client: omx-clap-host's restart and relayout) shuts its cycles out of the instance for the deactivate. The stage's own
 * stop (unpublish, the restart's wait) leaves a cycle passing the lane through, but that cycle still reads the stage,
 * the bounce and the active flag, which the deactivate clears and unmaps, and the activate binds again. A cycle enters
 * the gate before it touches the instance and leaves it after; the control thread closes it, waiting for the cycle
 * under way, before the deactivate, and opens it once the instance is armed again. A cycle that finds the gate closed
 * passes its audio dry without reading the instance.
 *
 * The enter (store in_cycle, load closed) and the close (store closed, load in_cycle) are sequentially consistent, so
 * either the cycle sees the gate closed or the control thread sees the cycle and waits for its leave.
 */

#ifndef CYCLE_GATE_H
#define CYCLE_GATE_H

#include <stdatomic.h>
#include <stdint.h>
#include <unistd.h>

#include "clap_host.h"

/** A gate: open while both are zero, as a zeroed one is. */
struct omx_cycle_gate {
    _Atomic uint32_t closed;    ///< the control thread shut the cycles out
    _Atomic uint32_t in_cycle;  ///< a cycle is inside the gate
    _Atomic uint32_t resize_to; ///< a block size the carrier changed to, for the control thread's next tick; 0: none
    /** the lock a carrier writes the instance's audio role under from a thread of its own: the restart and the resize
     * read and write the role holding it, the resize across its wait for the cycle under way and the plugin's stop,
     * deactivate and activate, the restart across the plugin's restart. NULL: no such thread. omx-clap-host gives none:
     * its JACK thread names itself in the core's slot (omx_clap_host_name_slot) and writes no role. */
    pthread_mutex_t *role;
};

static inline void omx_cycle_gate_role_lock(struct omx_cycle_gate *g)
{
    if (g->role)
        pthread_mutex_lock(g->role);
}

static inline void omx_cycle_gate_role_unlock(struct omx_cycle_gate *g)
{
    if (g->role)
        pthread_mutex_unlock(g->role);
}

/** The carrier's own thread (JACK's buffer-size callback): the new block size recorded, and nothing of the plugin done
 * there, since a deactivate and an activate are main-thread calls. A later change before the tick replaces it. */
static inline void omx_cycle_gate_resize_record(struct omx_cycle_gate *g, uint32_t nframes)
{
    atomic_store(&g->resize_to, nframes);
}

/** RT, inside the gate: nonzero while a recorded size waits for its tick; the cycle passes dry until it lands, since the
 * instance is still activated for the old size. */
static inline int omx_cycle_gate_resizing(struct omx_cycle_gate *g)
{
    return atomic_load_explicit(&g->resize_to, memory_order_acquire) != 0u;
}

/** RT, before the cycle touches the instance: nonzero when it may; either way omx_cycle_gate_leave follows. */
static inline int omx_cycle_gate_enter(struct omx_cycle_gate *g)
{
    atomic_store(&g->in_cycle, 1u);
    return atomic_load(&g->closed) == 0u;
}

/** RT, once the cycle is done with the instance. */
static inline void omx_cycle_gate_leave(struct omx_cycle_gate *g)
{
    atomic_store_explicit(&g->in_cycle, 0u, memory_order_release);
}

/** Control thread: shut the cycles out and wait, polling every `poll_us`, for the one under way to leave. A cycle always
 * leaves, so this does not time out. */
static inline void omx_cycle_gate_close(struct omx_cycle_gate *g, unsigned poll_us)
{
    atomic_store(&g->closed, 1u);
    while (atomic_load(&g->in_cycle) != 0u)
        usleep(poll_us);
}

/** Control thread: let the cycles in again, once the instance is armed or left inactive. */
static inline void omx_cycle_gate_open(struct omx_cycle_gate *g)
{
    atomic_store_explicit(&g->closed, 0u, memory_order_release);
}

/** Control thread, the cycles running: the stage stopped by them (omx_clap_host_unpublish, which the restart's own wait
 * then finds done; one that did not stop in time is the restart's to stop), the gate closed around
 * omx_clap_host_restart, and the audio role the cycles held given back. Returns the restart's answer: 0 restarted and
 * armed; OMX_CLAP_RESTART_RESCANNED inactive with the rescan reported and its audio role as before the unpublish, for
 * the caller to re-describe and activate (omx_cycle_gate_close around it again); -1 a failure. The gate is open on
 * return. The thread given back is read after the unpublish, under the gate's role lock: one named meanwhile is it. */
static inline int omx_cycle_gate_restart(struct omx_cycle_gate *g, struct omx_clap_instance *in, unsigned poll_us, unsigned timeout_us)
{
    pthread_t thread;
    int held, ret;

    omx_cycle_gate_role_lock(g);
    held = in->audio_role_held;
    omx_cycle_gate_role_unlock(g);
    (void)omx_clap_host_unpublish(in, poll_us, timeout_us);
    omx_cycle_gate_close(g, poll_us);
    omx_cycle_gate_role_lock(g);
    thread = in->audio_thread;
    held = held || in->audio_role_held;
    ret = omx_clap_host_restart(in);
    if (ret == 0 && held)
        omx_clap_host_set_audio_thread(in, thread);
    else if (ret == OMX_CLAP_RESTART_RESCANNED)
    {
        in->audio_thread = thread;
        in->audio_role_held = held;
    }
    omx_cycle_gate_role_unlock(g);
    omx_cycle_gate_open(g);
    return ret;
}

/** Control thread, the cycles running: unpublish (the cycles stop the stage), stop, close the gate and deactivate. 0 with
 * the gate CLOSED, for the caller to open once the instance is armed again or left inactive; -1 when the stage did not
 * stop in time, nothing deactivated and the gate open. An inactive instance only has the gate closed. The stop, which
 * names this thread for the plugin's stop_processing, holds the gate's role lock. */
static inline int omx_cycle_gate_deactivate(struct omx_cycle_gate *g, struct omx_clap_instance *in, unsigned poll_us, unsigned timeout_us)
{
    if (in->active)
    {
        if (omx_clap_host_unpublish(in, poll_us, timeout_us) != 0)
            return -1;
        omx_cycle_gate_role_lock(g);
        omx_clap_host_stop(in);
        omx_cycle_gate_role_unlock(g);
    }
    omx_cycle_gate_close(g, poll_us);
    omx_clap_host_deactivate(in);
    return 0;
}

/** An activation a relayout left waiting for the carrier's next block-size change (its blocks were shorter than the new
 * block): set, with the audio role the relayout saved, under the lock every restart, relayout and resize holds. */
struct omx_cycle_gate_pending {
    int set;
    pthread_t thread;   ///< the audio role to give back
    int held;
};

/** What a carrier keeps beside the instance across a change of the block size: buffers sized by the block (omx-clap-host's
 * auxiliary blocks). `stale(ctx, nframes)` says whether they need making again for `nframes`; `reblock(ctx, nframes)`
 * makes and binds them, the instance deactivated and the cycles shut out, 0 or -1 when it cannot. Either may be NULL:
 * nothing kept. */
struct omx_cycle_gate_blocks {
    int (*stale)(void *ctx, uint32_t nframes);
    int (*reblock)(void *ctx, uint32_t nframes);
    void *ctx;
};

/** omx_cycle_gate_resize's answers */
#define OMX_CYCLE_GATE_RESIZED          1   ///< activated at the new size
#define OMX_CYCLE_GATE_UNCHANGED        0   ///< nothing to do, or an inactive instance left to its relayout
#define OMX_CYCLE_GATE_REFUSED          (-1)    ///< the activation refused, `why` says why; the instance inactive
#define OMX_CYCLE_GATE_REBLOCK_FAILED   (-2)    ///< the carrier's blocks could not be made; the instance inactive, pending
#define OMX_CYCLE_GATE_RESCANNED        2   ///< the plugin rescanned its ports (in the deactivate, or before): inactive,
                                            ///< the rescan reported, for the carrier's relayout to read again

/**
 * A change of the block size, on the control thread (the plugin's deactivate and activate are main-thread calls; the
 * carrier records the size, omx_cycle_gate_resize_record, and its tick applies it, omx_cycle_gate_resize_tick), under
 * `lock`, the lock every restart and relayout of the instance holds. When the instance is active at another block, the carrier's blocks of an
 * active or pending instance are stale, or `pending` holds a relayout's waiting activation, the cycles are shut out: an active instance is stopped and
 * deactivated, the carrier's blocks made again (`blocks`, inside the closed gate, for the cycles copy them), and the
 * instance activated at `nframes` when it was active or pending, armed with the audio role it had (the pending one's
 * saved role), then the gate opened. A failed reblock leaves the instance inactive and pending, its role saved, for the
 * next change to try again; a refused activation leaves it inactive with `why` set. A ports rescan the plugin raised in
 * the deactivate (or one not yet answered) leaves it inactive with the rescan reported, its audio role kept and nothing
 * pending: its blocks are still made at `nframes`, and the carrier's relayout reads the description again and
 * activates. An instance a relayout left inactive and not pending is the relayout's to activate, at the size the carrier then reads. Its state is read under the lock
 * only. Returns one of OMX_CYCLE_GATE_*.
 */
static inline int omx_cycle_gate_resize(struct omx_cycle_gate *g, struct omx_clap_instance *in, pthread_mutex_t *lock, double rate,
                                        uint32_t nframes, unsigned poll_us, const struct omx_cycle_gate_blocks *blocks,
                                        struct omx_cycle_gate_pending *pending, char why[OMX_CLAP_WHY_MAX])
{
    int ret = OMX_CYCLE_GATE_UNCHANGED;

    pthread_mutex_lock(lock);
    omx_cycle_gate_role_lock(g);
    {
        const int active = in->active;
        const int waiting = pending && pending->set;
        // an instance a relayout left inactive and not pending is not run again until it is added again: its blocks are
        // not made either
        const int stale = (active || waiting) && blocks && blocks->stale && blocks->stale(blocks->ctx, nframes);
        // a pending relayout's stop left the role released: a thread the carrier named since holds it, and is newer than
        // the one the relayout saved
        const int named = waiting && !active && in->audio_role_held;

        if ((active && nframes != in->max_block) || stale || waiting)
        {
            // stop takes the audio role for this thread and releases it: the cycle's is given back after, a pending
            // relayout's is the one it saved
            const pthread_t thread = active || named ? in->audio_thread : waiting ? pending->thread : in->audio_thread;
            const int held = active || named ? in->audio_role_held : waiting ? pending->held : 0;

            if (pending)
                pending->set = 0;
            omx_cycle_gate_close(g, poll_us);
            if (active)
            {
                omx_clap_host_stop(in);
                omx_clap_host_deactivate(in);
            }
            if (stale && blocks->reblock && blocks->reblock(blocks->ctx, nframes) != 0)
            {
                ret = OMX_CYCLE_GATE_REBLOCK_FAILED;
                if (pending)
                {
                    pending->set = 1;
                    pending->thread = thread;
                    pending->held = held;
                }
            }
            // a ports rescan the plugin raised in that deactivate, or one still unanswered: the description is stale, so the
            // instance stays inactive and the rescan reported (omx_clap_host_ports_rescanned), as a restart leaves it;
            // the carrier's relayout reads it again and activates, nothing pends here
            else if ((active || waiting) && atomic_exchange_explicit(&in->ports_rescan_requested, 0u, memory_order_relaxed))
            {
                atomic_store_explicit(&in->ports_rescanned, 1u, memory_order_relaxed);
                in->audio_thread = thread;      // the relayout's activate gives back the role the cycles held
                in->audio_role_held = held;
                ret = OMX_CYCLE_GATE_RESCANNED;
            }
            else if (active || waiting)
            {
                if (omx_clap_host_activate(in, rate, nframes, why) == 0)
                {
                    if (held)
                        omx_clap_host_set_audio_thread(in, thread);
                    omx_clap_host_arm(in);
                    ret = OMX_CYCLE_GATE_RESIZED;
                }
                else
                    ret = OMX_CYCLE_GATE_REFUSED;
            }
            omx_cycle_gate_open(g);
        }
    }
    omx_cycle_gate_role_unlock(g);
    pthread_mutex_unlock(lock);
    return ret;
}

/**
 * Control thread, its tick, before the relayout: a block size the carrier recorded applied by omx_cycle_gate_resize (the
 * last one when it changed twice before the tick), then the record cleared, unless the carrier recorded another
 * meanwhile, which the next tick applies. `*nframes` is the size applied. Returns OMX_CYCLE_GATE_UNCHANGED with nothing
 * recorded, else the resize's answer.
 */
static inline int omx_cycle_gate_resize_tick(struct omx_cycle_gate *g, struct omx_clap_instance *in, pthread_mutex_t *lock, double rate,
                                             unsigned poll_us, const struct omx_cycle_gate_blocks *blocks,
                                             struct omx_cycle_gate_pending *pending, char why[OMX_CLAP_WHY_MAX], uint32_t *nframes)
{
    uint32_t to = atomic_load(&g->resize_to);
    int ret;

    *nframes = to;
    if (to == 0u)
        return OMX_CYCLE_GATE_UNCHANGED;
    ret = omx_cycle_gate_resize(g, in, lock, rate, to, poll_us, blocks, pending, why);
    (void)atomic_compare_exchange_strong(&g->resize_to, &to, 0u);
    return ret;
}

#endif
