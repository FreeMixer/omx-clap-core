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

/** @file
 * @brief The chain of libomx-clap-core: an ordered list of stages one audio cycle runs in order (docs/chain.md). */

/*
************************************************************************************************************************
*
* The chain: members in order, each one's output the next one's input, run by one call in the caller's audio thread.
* A member is an instance the library hosts (struct omx_clap_instance, run by omx_clap_host_run_ports) or a stage of
* the caller's own (a process function and its context). Each has a bypass flag (a bypassed member is not run, copied
* or counted), its latency and its channel layout; the chain's latency is the sum of its non-bypassed members', and
* re-reported on every change. omx_clap_chain_remove takes a member out and the chain closes around it.
*
* The chain is generic: it names no trim, fader, strip or bus. It is not a latency win (docs/bench-chain-2026-10-21.md):
* it saves a JACK client and memory per plugin.
*
* Every function is a control-thread call, serialised by the chain's lock, except omx_clap_chain_process, the audio
* thread's, which takes no lock, allocates nothing and waits for nothing, and omx_clap_chain_report, which reads without
* the lock from any thread but the audio one. Functions answer one of the OMX_CLAP_CHAIN_* codes.
*
* Every structure a consumer allocates carries `abi` and `size` first: the library refuses one whose `abi` it does not
* know or whose `size` is not the size that ABI gives it, OMX_CLAP_CHAIN_ERR_ABI, reading and writing nothing (#301).
* Fill them with the omx_clap_chain_*_init helpers, which compile into the consumer with its own header's values.
*
************************************************************************************************************************
*/

#ifndef OMX_CLAP_CHAIN_H
#define OMX_CLAP_CHAIN_H


/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <stdint.h>
#include <string.h>

#include "clap_host.h"


/*
************************************************************************************************************************
*           CONFIGURATION DEFINES
************************************************************************************************************************
*/

/** The version of the chain's structures: a field is only ever appended, and each append raises it. The library accepts
 * every ABI from OMX_CLAP_CHAIN_ABI_OLDEST up to its own, each at exactly the size that ABI gives a structure. */
#define OMX_CLAP_CHAIN_ABI              1u
#define OMX_CLAP_CHAIN_ABI_OLDEST       1u  ///< the oldest chain ABI the library accepts

/** The most members one chain holds. */
#define OMX_CLAP_CHAIN_MEMBERS_MAX      64u
/** The longest cycle the chain runs, in frames: the length of each block between members. A longer cycle passes the
 * chain's input to its output dry and answers OMX_CLAP_CHAIN_PASSED. */
#define OMX_CLAP_CHAIN_FRAMES_MAX       8192u
/** The alignment of every channel of the blocks between members, in bytes: a JACK port buffer's. */
#define OMX_CLAP_CHAIN_ALIGN            64u
/** omx_clap_chain_add's position after the last member. */
#define OMX_CLAP_CHAIN_END              UINT32_MAX

/** What the chain's functions answer. */
#define OMX_CLAP_CHAIN_OK               0       ///< done
#define OMX_CLAP_CHAIN_PASSED           1       ///< process: the cycle was longer than OMX_CLAP_CHAIN_FRAMES_MAX and passed dry
#define OMX_CLAP_CHAIN_ERR_ABI          (-1)    ///< a structure of an ABI the library does not know, or of another size
#define OMX_CLAP_CHAIN_ERR_ARG          (-2)    ///< a NULL, a position past the end, a member neither or both kinds
#define OMX_CLAP_CHAIN_ERR_WIDTH        (-3)    ///< a member's input or output is not as wide as the chain, or a chain
                                                ///< width out of 1 to CLAP_HOST_PORT_CHANNELS
#define OMX_CLAP_CHAIN_ERR_FULL         (-4)    ///< the chain holds OMX_CLAP_CHAIN_MEMBERS_MAX members already
#define OMX_CLAP_CHAIN_ERR_NO_MEMBER    (-5)    ///< no member by that id, or no member at that position
#define OMX_CLAP_CHAIN_ERR_MEMORY       (-6)    ///< the chain or its blocks could not be allocated


/*
************************************************************************************************************************
*           DATA TYPES
************************************************************************************************************************
*/

/** A stage of the caller's own, in the audio thread: `inputs` and `outputs` one per channel, `frames` long, never the
 * same buffers. */
typedef void (*omx_clap_chain_process_fn)(void *ctx, const float *const *inputs, float *const *outputs, uint32_t frames);

/** The chain's latency changed: the new sum of its non-bypassed members', in frames, on the control thread, after the
 * chain's lock is released. */
typedef void (*omx_clap_chain_latency_fn)(void *ctx, uint32_t frames);

/** A chain to create. */
struct omx_clap_chain_config
{
    uint32_t abi;               ///< OMX_CLAP_CHAIN_ABI of the header the caller was compiled against
    uint32_t size;              ///< sizeof of this structure as the caller was compiled
    uint32_t channels;          ///< the chain's width, 1 to CLAP_HOST_PORT_CHANNELS: every member's input and output
    omx_clap_chain_latency_fn latency_changed;  ///< called when the latency changes; NULL: nothing called
    void *ctx;                  ///< handed to latency_changed
};

/** A member to add: an instance the library hosts, or a stage of the caller's own, never both. */
struct omx_clap_chain_member
{
    uint32_t abi;               ///< OMX_CLAP_CHAIN_ABI of the header the caller was compiled against
    uint32_t size;              ///< sizeof of this structure as the caller was compiled
    struct omx_clap_instance *instance;  ///< an instance, opened, activated and published by the caller; NULL for a stage
    omx_clap_chain_process_fn process;   ///< a stage of the caller's own; NULL for an instance
    void *ctx;                  ///< handed to process
    uint32_t in_channels;       ///< a stage's input width; an instance's is its main input's
    uint32_t out_channels;      ///< a stage's output width; an instance's is its main output's
    uint32_t latency;           ///< a stage's latency in frames; an instance's is omx_clap_host_latency's
    uint32_t bypass;            ///< nonzero: added bypassed
};

/** The chain as it stands. */
struct omx_clap_chain_report
{
    uint32_t abi;               ///< OMX_CLAP_CHAIN_ABI of the header the caller was compiled against
    uint32_t size;              ///< sizeof of this structure as the caller was compiled
    uint32_t channels;          ///< the chain's width
    uint32_t members;           ///< the members, bypassed or not
    uint32_t running;           ///< the members a cycle runs: the non-bypassed ones
    uint32_t latency;           ///< the sum of the non-bypassed members' latencies, in frames
    uint32_t revision;          ///< the walks published since create: one per add, removal and bypass change
    uint64_t passed;            ///< the cycles longer than OMX_CLAP_CHAIN_FRAMES_MAX, passed dry
};

/** One member as it stands. */
struct omx_clap_chain_member_info
{
    uint32_t abi;               ///< OMX_CLAP_CHAIN_ABI of the header the caller was compiled against
    uint32_t size;              ///< sizeof of this structure as the caller was compiled
    uint32_t id;                ///< the id omx_clap_chain_add answered
    uint32_t position;          ///< its place in the chain, 0 first
    uint32_t in_channels;       ///< its input width
    uint32_t out_channels;      ///< its output width
    uint32_t latency;           ///< its latency, in frames, counted or not
    uint32_t bypass;            ///< 1 while bypassed
    struct omx_clap_instance *instance;  ///< the instance, NULL for a stage
    void *ctx;                  ///< the stage's context, NULL for an instance
};

/** A chain, allocated and freed by the library. */
struct omx_clap_chain;


/*
************************************************************************************************************************
*           FUNCTION PROTOTYPES
************************************************************************************************************************
*/

/** A chain of `config->channels`, empty, its two blocks between members allocated: OMX_CLAP_CHAIN_OK with `*out` set,
 * or a refusal with `*out` NULL. */
OMX_CLAP_EXPORT int omx_clap_chain_create(const struct omx_clap_chain_config *config, struct omx_clap_chain **out);

/** Free a chain, no cycle running it. Its members are the caller's: nothing is done to them. */
OMX_CLAP_EXPORT void omx_clap_chain_destroy(struct omx_clap_chain *chain);

/** Add `member` at `position` (0 first, OMX_CLAP_CHAIN_END or the member count after the last), its id in `*id` when
 * `id` is not NULL. Both its widths must be the chain's. The cycle after the call returns runs it, unless bypassed. */
OMX_CLAP_EXPORT int omx_clap_chain_add(struct omx_clap_chain *chain, const struct omx_clap_chain_member *member, uint32_t position,
                                       uint32_t *id);

/** Take member `id` out: the member before it hands its output to the member after it. When this returns no cycle
 * reads the member, and the chain never touches it again: the caller may close or free it. */
OMX_CLAP_EXPORT int omx_clap_chain_remove(struct omx_clap_chain *chain, uint32_t id);

/** Bypass member `id` (`bypass` nonzero) or run it again (0). A bypassed member is not run, copied or counted in the
 * latency. When this returns the cycles run the new walk. */
OMX_CLAP_EXPORT int omx_clap_chain_bypass(struct omx_clap_chain *chain, uint32_t id, int bypass);

/** The latency of stage `id` stated again, in frames; an instance's is read by omx_clap_chain_refresh instead,
 * OMX_CLAP_CHAIN_ERR_ARG. */
OMX_CLAP_EXPORT int omx_clap_chain_member_latency_set(struct omx_clap_chain *chain, uint32_t id, uint32_t frames);

/** Every instance member's latency read again (omx_clap_host_latency): after a restart or a relayout of one. */
OMX_CLAP_EXPORT int omx_clap_chain_refresh(struct omx_clap_chain *chain);

/** The chain as it stands into `out`, whose `abi` and `size` the caller set. Any thread but the audio one. */
OMX_CLAP_EXPORT int omx_clap_chain_report(const struct omx_clap_chain *chain, struct omx_clap_chain_report *out);

/** The member at `position` into `out`, whose `abi` and `size` the caller set. */
OMX_CLAP_EXPORT int omx_clap_chain_member_info(struct omx_clap_chain *chain, uint32_t position,
                                               struct omx_clap_chain_member_info *out);

/** RT: one cycle, `inputs` to `outputs`, one buffer per channel of the chain, `frames` long; `inputs` and `outputs` may be
 * the same buffers. The non-bypassed members in order; their input copied to their output when there is none; the input
 * copied to the output dry, no member run, for a cycle longer than OMX_CLAP_CHAIN_FRAMES_MAX (OMX_CLAP_CHAIN_PASSED).
 * One audio thread at a time per chain. */
OMX_CLAP_EXPORT int omx_clap_chain_process(struct omx_clap_chain *chain, const float *const *inputs, float *const *outputs,
                                           uint32_t frames);


/*
************************************************************************************************************************
*           INLINE FUNCTIONS
************************************************************************************************************************
*/

/** A configuration zeroed, `abi` and `size` this header's. */
static inline void omx_clap_chain_config_init(struct omx_clap_chain_config *config)
{
    memset(config, 0, sizeof(*config));
    config->abi = OMX_CLAP_CHAIN_ABI;
    config->size = (uint32_t)sizeof(*config);
}

/** A member zeroed, `abi` and `size` this header's. */
static inline void omx_clap_chain_member_init(struct omx_clap_chain_member *member)
{
    memset(member, 0, sizeof(*member));
    member->abi = OMX_CLAP_CHAIN_ABI;
    member->size = (uint32_t)sizeof(*member);
}

/** A report zeroed, `abi` and `size` this header's. */
static inline void omx_clap_chain_report_init(struct omx_clap_chain_report *report)
{
    memset(report, 0, sizeof(*report));
    report->abi = OMX_CLAP_CHAIN_ABI;
    report->size = (uint32_t)sizeof(*report);
}

/** A member's info zeroed, `abi` and `size` this header's. */
static inline void omx_clap_chain_member_info_init(struct omx_clap_chain_member_info *info)
{
    memset(info, 0, sizeof(*info));
    info->abi = OMX_CLAP_CHAIN_ABI;
    info->size = (uint32_t)sizeof(*info);
}

#endif /* OMX_CLAP_CHAIN_H */
