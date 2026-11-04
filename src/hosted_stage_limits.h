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

#ifndef HOSTED_STAGE_LIMITS_H
#define HOSTED_STAGE_LIMITS_H
/*
 * GENERATED — DO NOT EDIT BY HAND.
 * Rendered from HOSTED_STAGE_LIMITS of data/hosting.json: every number of the stage, spelled once,
 * OMX_HOSTED_STAGE_<FIELD>, as omx-contract's omx_contract_limits.h spelled it, the worker fields included. The rendered
 * file is kept in this repository so the library builds on its own; the numbers are changed where they are declared and
 * the file is re-rendered, never edited here.
 */

/** @file
 * @brief The hosted stage's numbers, the one spelling the core and a host read. */

#include <stddef.h>

/** the stage and the host: every number the core reads */
#define OMX_HOSTED_STAGE_WARMUP_BLOCKS 64u
#define OMX_HOSTED_STAGE_WARMUP_LEVEL_DBFS -6.0f  ///< the level of the warm-up signal, in dBFS
#define OMX_HOSTED_STAGE_NON_FINITE_STRIKES 3u  ///< the blocks of non-finite output that strike a stage
#define OMX_HOSTED_STAGE_CLAMP_DBFS 24.0f  ///< the level the output is clamped at, in dBFS
#define OMX_HOSTED_STAGE_PARAM_QUEUE_DEPTH 256u  ///< the records the parameter queue holds
#define OMX_HOSTED_STAGE_EVENTS_PER_BLOCK 64u  ///< the parameter events delivered in one block
#define OMX_HOSTED_STAGE_UNPUBLISH_POLL_US 100u  ///< the poll interval while an unpublish waits for the audio thread, in microseconds
#define OMX_HOSTED_STAGE_UNPUBLISH_TIMEOUT_US 2000000u  ///< the longest an unpublish waits for the audio thread, in microseconds
#define OMX_HOSTED_STAGE_WORKER_POLL_US 1000u  ///< how often an LV2 instance's worker thread polls its request ring, in microseconds
#define OMX_HOSTED_STAGE_WORKER_RING_BYTES 16384u  ///< the bytes of each of an LV2 instance's two worker rings; a power of two, 16 or more
#define OMX_HOSTED_STAGE_NOTES_PER_BLOCK 256u  ///< the note events delivered in one block
#define OMX_HOSTED_STAGE_PORT_CHANNELS 64u  ///< the channels a port is limited to, main or auxiliary
#define OMX_HOSTED_STAGE_AUX_OUTPUTS 64u  ///< the auxiliary output ports the core accepts beyond the main one
#define OMX_HOSTED_STAGE_AUX_INPUTS 64u  ///< the auxiliary input ports the core accepts beyond the main one
#define OMX_HOSTED_STAGE_STATE_MAX_BYTES 1048576u  ///< the largest plugin state the core saves or loads, in bytes
#define OMX_HOSTED_STAGE_LOG_BYTES 256u  ///< the size of a plugin log message, in bytes
#define OMX_HOSTED_STAGE_ROLE_POLL_US 1000u  ///< the poll interval while the audio role is taken, in microseconds
#define OMX_HOSTED_STAGE_ROLE_TIMEOUT_US 200000u  ///< the wait for the audio role past which a stop gives up or take_role says so, in microseconds
#define OMX_HOSTED_STAGE_PARAM_COUNT_MAX 2048u  ///< the widest parameter count the core trusts
#define OMX_HOSTED_STAGE_LATENCY_HOLD_MS 250u  ///< the hold before a hosted stage's new latency takes effect, in milliseconds
#define OMX_HOSTED_STAGE_IDLE_HOLD_US 10u  ///< the latency the library holds /dev/cpu_dma_latency at while an instance is active, in microseconds: 10 keeps the cores in C1

#endif /* HOSTED_STAGE_LIMITS_H */
