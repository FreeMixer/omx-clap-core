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
 * Rendered from HOSTED_STAGE_LIMITS of data/hosting.json, every number spelled OMX_HOSTED_STAGE_<FIELD> as
 * omx-contract's omx_contract_limits.h spelled it, the worker fields included, for a host that reads those
 * names. The rendered file is kept in this repository so the library builds on its own; the numbers are
 * changed where they are declared and the file is re-rendered, never edited here.
 */

/** @file
 * @brief The hosted stage's numbers, as the OMX_HOSTED_STAGE_ names a host reads. */

/** every number of the stage, sorted by name: an integer literal, any other a float literal and its _DOUBLE spelling */
#define OMX_HOSTED_STAGE_AUX_INPUTS 8
#define OMX_HOSTED_STAGE_AUX_OUTPUTS 8
#define OMX_HOSTED_STAGE_CLAMP_DBFS 24
#define OMX_HOSTED_STAGE_EVENTS_PER_BLOCK 64
#define OMX_HOSTED_STAGE_LATENCY_HOLD_MS 250
#define OMX_HOSTED_STAGE_LOG_BYTES 256
#define OMX_HOSTED_STAGE_MAIN_PORT_CHANNELS 2
#define OMX_HOSTED_STAGE_NON_FINITE_STRIKES 3
#define OMX_HOSTED_STAGE_NOTES_PER_BLOCK 256
#define OMX_HOSTED_STAGE_PARAM_COUNT_MAX 2048
#define OMX_HOSTED_STAGE_PARAM_QUEUE_DEPTH 256
#define OMX_HOSTED_STAGE_ROLE_POLL_US 1000
#define OMX_HOSTED_STAGE_ROLE_TIMEOUT_US 200000
#define OMX_HOSTED_STAGE_STATE_MAX_BYTES 1048576
#define OMX_HOSTED_STAGE_UNPUBLISH_POLL_US 100
#define OMX_HOSTED_STAGE_UNPUBLISH_TIMEOUT_US 2000000
#define OMX_HOSTED_STAGE_WARMUP_BLOCKS 64
#define OMX_HOSTED_STAGE_WARMUP_LEVEL_DBFS -6
#define OMX_HOSTED_STAGE_WORKER_POLL_US 1000
#define OMX_HOSTED_STAGE_WORKER_RING_BYTES 16384

#endif /* HOSTED_STAGE_LIMITS_H */
