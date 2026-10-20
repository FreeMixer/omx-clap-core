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
};

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
 * return. */
static inline int omx_cycle_gate_restart(struct omx_cycle_gate *g, struct omx_clap_instance *in, unsigned poll_us, unsigned timeout_us)
{
    const pthread_t thread = in->audio_thread;
    const int held = in->audio_role_held;
    int ret;

    (void)omx_clap_host_unpublish(in, poll_us, timeout_us);
    omx_cycle_gate_close(g, poll_us);
    ret = omx_clap_host_restart(in);
    if (ret == 0 && held)
        omx_clap_host_set_audio_thread(in, thread);
    else if (ret == OMX_CLAP_RESTART_RESCANNED)
    {
        in->audio_thread = thread;
        in->audio_role_held = held;
    }
    omx_cycle_gate_open(g);
    return ret;
}

/** Control thread, the cycles running: unpublish (the cycles stop the stage), stop, close the gate and deactivate. 0 with
 * the gate CLOSED, for the caller to open once the instance is armed again or left inactive; -1 when the stage did not
 * stop in time, nothing deactivated and the gate open. An inactive instance only has the gate closed. */
static inline int omx_cycle_gate_deactivate(struct omx_cycle_gate *g, struct omx_clap_instance *in, unsigned poll_us, unsigned timeout_us)
{
    if (in->active)
    {
        if (omx_clap_host_unpublish(in, poll_us, timeout_us) != 0)
            return -1;
        omx_clap_host_stop(in);
    }
    omx_cycle_gate_close(g, poll_us);
    omx_clap_host_deactivate(in);
    return 0;
}

/** A change of the block size, on the carrier's own thread (JACK's buffer-size callback): under `lock`, the lock every
 * restart and relayout of the instance holds, an instance still active at another block is stopped, deactivated and
 * activated at `nframes`, the cycles shut out around it, and armed with the audio role it had. Its state is read under
 * the lock only: an instance a relayout or restart left inactive meanwhile (a layout refused, ports no longer the
 * carrier's, a rescan waiting for its relayout) is the relayout's to activate, at the size the carrier then reads, and
 * this leaves it inactive. Nonzero when it activated the instance again. */
static inline int omx_cycle_gate_resize(struct omx_cycle_gate *g, struct omx_clap_instance *in, pthread_mutex_t *lock, double rate,
                                        uint32_t nframes, unsigned poll_us)
{
    int ok = 0;

    pthread_mutex_lock(lock);
    if (in->active && nframes != in->max_block)
    {
        // stop takes the audio role for this thread and releases it: the cycle's is given back after
        const pthread_t thread = in->audio_thread;
        const int held = in->audio_role_held;

        omx_cycle_gate_close(g, poll_us);
        omx_clap_host_stop(in);
        omx_clap_host_deactivate(in);
        if (omx_clap_host_activate(in, rate, nframes, NULL) == 0)
        {
            if (held)
                omx_clap_host_set_audio_thread(in, thread);
            omx_clap_host_arm(in);
            ok = 1;
        }
        omx_cycle_gate_open(g);
    }
    pthread_mutex_unlock(lock);
    return ok;
}

#endif
