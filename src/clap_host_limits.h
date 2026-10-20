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
 * Rendered from HOSTED_STAGE_LIMITS and HOSTING_CODES of data/hosting.json, where this library declares
 * its stage limits and hosting refusal codes. The rendered file is kept in this repository so the library
 * builds on its own; the numbers and codes are changed where they are declared and the file is re-rendered,
 * never edited here.
 */

/** @file
 * @brief The limits and refusal codes of the hosting code. */

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

/** the refusals the core returns, as the hosting codes the console's verdicts use */
#define CLAP_HOST_CODE_HEADLESS_FAILED "hosting.clap.headless-failed"
#define CLAP_HOST_CODE_NOT_AUDIO_EFFECT "hosting.clap.not-audio-effect"  ///< the plugin is not an audio effect
#define CLAP_HOST_CODE_NOTE_INPUT "hosting.clap.note-input"  ///< the plugin declares a note input the configuration refuses
#define CLAP_HOST_CODE_NO_AUDIO_OUTPUT "hosting.topology.no-audio-output"  ///< the plugin has no audio output, no MIDI output and no audio input: nothing to carry and nothing to pass through
#define CLAP_HOST_CODE_WIDER_THAN_STRIP "hosting.topology.wider-than-strip"  ///< a main port is wider than the lane the caller runs the plugin on
#define CLAP_HOST_CODE_TOO_MANY_PORTS "hosting.topology.too-many-ports"  ///< more than the limit of auxiliary ports on one side, or more than one main port on a side
#define CLAP_HOST_CODE_PORT_TOO_WIDE "hosting.topology.port-too-wide"  ///< a port of more channels than the limit, or an ambisonic port whose channels are not (order+1)²
#define CLAP_HOST_CODE_PORT_WITHOUT_CHANNELS "hosting.topology.port-without-channels"  ///< a port of zero channels
#define CLAP_HOST_CODE_CHANNEL_MAP_UNKNOWN "hosting.topology.channel-map-unknown"  ///< a surround port names a channel id the host does not know, or refuses the carrier's channel mask
#define CLAP_HOST_CODE_AMBISONIC_CONFIG "hosting.topology.ambisonic-config"  ///< an ambisonic port refuses the carrier's ordering or normalisation; raised by a layout request only
#define CLAP_HOST_CODE_CRASHED_LIVE "hosting.stability.crashed-live-on-this-rig"  ///< the plugin crashed live on this rig
#define CLAP_HOST_CODE_OUTPUT_NON_FINITE "hosting.output.non-finite"  ///< the plugin produced non-finite output

#endif /* CLAP_HOST_LIMITS_H */
