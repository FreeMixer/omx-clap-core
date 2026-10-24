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

#ifndef CLAP_HOST_LIMITS_H
#define CLAP_HOST_LIMITS_H
/*
 * GENERATED — DO NOT EDIT BY HAND.
 * Rendered from HOSTED_STAGE_LIMITS, HOSTING_IDLE_CODES and HOSTING_WINDOW_CODES of data/hosting.json, where this
 * tree declares its stage limits and the codes of the dimensions it owns. The rendered file is kept in this repository so the
 * library builds on its own; the numbers and codes are changed where they are declared and the file is
 * re-rendered, never edited here. Every other refusal code is declared in data/hosting/ and rendered as
 * omx_clap_core_vocabulary.h (clap_host.h's CLAP_HOST_CODE_*).
 */

/** @file
 * @brief The limits of the hosting code, and the codes of the idle and window dimensions. */

#include <stddef.h>

/** the stage and the host: every number the core reads */
#define CLAP_HOST_WARMUP_BLOCKS 64u
#define CLAP_HOST_WARMUP_LEVEL_DBFS -6.0f  ///< the level of the warm-up signal, in dBFS
#define CLAP_HOST_NON_FINITE_STRIKES 3u  ///< the blocks of non-finite output that strike a stage
#define CLAP_HOST_CLAMP_DBFS 24.0f  ///< the level the output is clamped at, in dBFS
#define CLAP_HOST_PARAM_QUEUE_DEPTH 256u  ///< the records the parameter queue holds
#define CLAP_HOST_EVENTS_PER_BLOCK 64u  ///< the parameter events delivered in one block
#define CLAP_HOST_UNPUBLISH_POLL_US 100u  ///< the poll interval while an unpublish waits for the audio thread, in microseconds
#define CLAP_HOST_UNPUBLISH_TIMEOUT_US 2000000u  ///< the longest an unpublish waits for the audio thread, in microseconds
#define CLAP_HOST_NOTES_PER_BLOCK 256u  ///< the note events delivered in one block
#define CLAP_HOST_PORT_CHANNELS 64u  ///< the channels a port is limited to, main or auxiliary
#define CLAP_HOST_AUX_OUTPUTS 64u  ///< the auxiliary output ports the core accepts beyond the main one
#define CLAP_HOST_AUX_INPUTS 64u  ///< the auxiliary input ports the core accepts beyond the main one
#define CLAP_HOST_STATE_MAX_BYTES 1048576u  ///< the largest plugin state the core saves or loads, in bytes
#define CLAP_HOST_LOG_BYTES 256u  ///< the size of a plugin log message, in bytes
#define CLAP_HOST_ROLE_POLL_US 1000u  ///< the poll interval while the audio role is taken, in microseconds
#define CLAP_HOST_ROLE_TIMEOUT_US 200000u  ///< the longest the audio role is waited for, in microseconds
#define CLAP_HOST_PARAM_COUNT_MAX 2048u  ///< the widest parameter count the core trusts
#define CLAP_HOST_LATENCY_HOLD_MS 250u  ///< the hold before a hosted stage's new latency takes effect, in milliseconds
#define CLAP_HOST_IDLE_HOLD_US 10u  ///< the latency the library holds /dev/cpu_dma_latency at while an instance is active, in microseconds: 10 keeps the cores in C1

/** the idle dimension, what the library answers about its hold of /dev/cpu_dma_latency (#296), and the window dimension, why omx-clap-host shows no plugin window (#180) */
#define CLAP_HOST_CODE_IDLE_HOLD_REFUSED "hosting.idle.hold-refused"  ///< the device could not be opened for writing (no permission, or no such device): the host runs with no hold
#define CLAP_HOST_CODE_WINDOW_NO_DISPLAY "hosting.window.no-display"  ///< no display to show it on: none is named, or its server refused the connection; the plugin runs on
#define CLAP_HOST_CODE_WINDOW_UNSUPPORTED "hosting.window.unsupported"  ///< the plugin has no clap.gui, or its clap.gui offers no window API the host shows, embedded or floating
#define CLAP_HOST_CODE_WINDOW_REFUSED "hosting.window.refused"  ///< the plugin's clap.gui answered false when asked to create, embed or show its window

#endif /* CLAP_HOST_LIMITS_H */
