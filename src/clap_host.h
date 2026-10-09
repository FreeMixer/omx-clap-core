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
 * @brief The control-thread host of libomx-clap-core: load, judge, activate, publish and tear down a CLAP plugin. */

/*
************************************************************************************************************************
*
* The control-thread host of libomx-clap-core: the only unit that loads a CLAP file and the CLAP main thread of every
* instance it hosts. It owns everything clap_stage.h (the RT body) does not: load, judge the ports, activate, warm up,
* publish, unpublish, rate change, restart, parameters as rows, the read-back shadow, state, latency, the host object.
*
* Every function here runs on the control thread unless its comment says otherwise. The RT calls none of the exported
* ones: it sees the stage, through omx_clap_run, or the instance through the inline omx_clap_host_run, the header's only
* RT code. Functions return 0 or -1, and name a refusal by the hosting code of clap_host_limits.h in `why`.
*
* The library is configured once per process (omx_clap_host_configure) and otherwise runs the defaults. A
* consumer is compiled against the headers of the library it runs with: OMX_CLAP_CORE_ABI is checked at configure.
*
************************************************************************************************************************
*/

#ifndef CLAP_HOST_H
#define CLAP_HOST_H


/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <clap/clap.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "clap_host_extensions.h"
#include "clap_stage.h"


/*
************************************************************************************************************************
*           CONFIGURATION DEFINES
************************************************************************************************************************
*/

/** The version of the structures below, the stage's and the instance's: a field is only ever appended, and each append
 * raises it (2: the instance's tempo and transport, 0.3; 3: the auxiliary inputs and their binding). configure accepts every ABI from OMX_CLAP_CORE_ABI_OLDEST up
 * to its own, so a consumer of an older header keeps working, and refuses a newer one: a consumer whose inline code
 * reaches an appended field is refused by a library whose instance lacks it. A removal or a reorder is a new major of
 * the library. */
#define OMX_CLAP_CORE_ABI               3u
#define OMX_CLAP_CORE_ABI_OLDEST        1u  ///< the oldest header ABI the library accepts

/** How many bytes a refusal's hosting code needs, with its NUL. */
#define OMX_CLAP_WHY_MAX                64

/** The roster's refusal: an index below count() whose get_info does not answer. */
#define OMX_CLAP_PARAM_ROW_UNREADABLE   "clap.param-row-unreadable"
/** The widest params->count() the core trusts, under the name openmixer's host gave it (CLAP_HOST_PARAM_COUNT_MAX). */
#define OMX_CLAP_PARAM_COUNT_MAX        CLAP_HOST_PARAM_COUNT_MAX

#if defined(__GNUC__)
#define OMX_CLAP_EXPORT                 __attribute__((visibility("default")))  ///< marks a function as exported from the library
#else
#define OMX_CLAP_EXPORT  ///< marks a function as exported from the library
#endif


/*
************************************************************************************************************************
*           DATA TYPES
************************************************************************************************************************
*/

/** What a host differs in from the defaults, set once per process. The strings must outlive the process's use. */
struct omx_clap_host_config
{
    uint32_t abi;               ///< OMX_CLAP_CORE_ABI of the header the caller was compiled against
    uint32_t size;              ///< sizeof of this structure as the caller was compiled: a field appended later is the default to it
    int clamp;                  ///< clamp the plugin's output at CLAP_HOST_CLAMP_DBFS
    int nonfinite;              ///< scan the output for non-finite samples and strike the stage on them
    int warmup;                 ///< warm the plugin up before publish and restart it after
    int note_inputs;            ///< admit an instrument and one note input; refuse any note input when 0
    int preset_load;            ///< offer the clap.preset-load host extension besides the declared list
    const char *name;           ///< the host the plugin is told it is in
    const char *vendor;  ///< the vendor the plugin is told it is in
    const char *url;  ///< the host URL the plugin is told
    const char *version;  ///< the host version the plugin is told
    int track_info;             ///< offer clap.track-info besides the declared list: get answers omx_clap_host_track_info_set
    int remote_controls;        ///< offer clap.remote-controls besides the declared list: see omx_clap_host_remote_controls_changed
};

/** One parameter of a hosted plugin as a row: what a slot serves. A parameter that is not a row (hidden, read-only, the
 * plugin's own bypass) is never enumerated. The units are the plugin's own. */
struct omx_clap_param_row
{
    clap_id id;  ///< the CLAP parameter id
    char name[CLAP_NAME_SIZE];  ///< the parameter's name
    double min,                 ///< the minimum, in the plugin's units
           max,                 ///< the maximum, in the plugin's units
           def;                 ///< the default, in the plugin's units
    int stepped;                ///< CLAP_PARAM_IS_STEPPED: integer travel
    int enumerated;             ///< CLAP_PARAM_IS_ENUM
    void *cookie;               ///< what get_info returned; rides every event for this id
};

/** The read-back shadow of one parameter: what the host delivered and the cycle that consumed it, kept beside what the
 * plugin says it applied. */
struct omx_clap_shadow
{
    clap_id id;  ///< the CLAP parameter id
    double delivered;  ///< the value the host delivered
    uint32_t cycle;             ///< the stage's `runs` when the write was enqueued; consumed once runs > cycle
    int valid;  ///< nonzero once a write has been delivered
};

/** Names the threads, beyond the one named directly, that hold the audio role: nonzero for `self` when it is one. Runs on
 * the plugin's calling thread, the RT one included. */
typedef int (*omx_clap_audio_role_fn)(void *ctx, pthread_t self);

struct omx_clap_binary;         ///< one loaded .clap, or one linked entry, reference-counted across its instances

/** What the control thread displaces while it holds the audio role: see omx_clap_host_take_role. */
struct omx_clap_role
{
    uint32_t state;  ///< the displaced role state
    pthread_t thread;  ///< the thread that holds the role
    int held;  ///< nonzero while the role is held
};

/** A host's tempo: one published word, the bpm as the bits of a double, 0 for none. An instance given it hands its plugin
 * a transport carrying that tempo and nothing else (no playhead, no meter); with no tempo the transport is NULL, never an
 * invented one. */
struct omx_clap_tempo
{
    _Atomic uint64_t bpm_bits;  ///< the bpm as the bits of a double, 0 for none
};

/** One hosted instance. Opaque to the RT except for `stage`, which the consumer's RT thread runs. */
struct omx_clap_instance
{
    struct omx_clap_binary *bin;  ///< the loaded binary the plugin comes from
    const clap_plugin_descriptor_t *desc;  ///< the plugin's descriptor
    const clap_plugin_t *plugin;  ///< the plugin instance
    const clap_plugin_params_t *params;  ///< the plugin's clap.params extension
    const clap_plugin_latency_t *latency;  ///< the plugin's clap.latency extension
    const clap_plugin_audio_ports_t *audio_ports;  ///< the plugin's clap.audio-ports extension
    const clap_plugin_note_ports_t *note_ports;  ///< the plugin's clap.note-ports extension
    const clap_plugin_state_t *state;  ///< the plugin's clap.state extension
    const clap_plugin_preset_load_t *preset_load;  ///< the plugin's clap.preset-load extension
    clap_host_t host;           ///< this instance's host object; host_data is the instance

    struct omx_clap_stage stage;  ///< the RT stage the consumer's audio thread runs
    uint32_t channels;          ///< the main output width the ports admitted (1 or 2); an effect's input has the same
    uint32_t in_channels;       ///< the main input width: 0 for an instrument
    uint32_t aux_outputs;       ///< auxiliary output ports, left unconnected
    uint32_t aux_channels[CLAP_HOST_AUX_OUTPUTS];  ///< the channel count of each auxiliary output port
    uint32_t note_inputs;       ///< 0 or 1; the dialect the host feeds it
    uint32_t note_dialect;  ///< the note dialect the host feeds the plugin
    uint32_t note_dialects;  ///< every dialect the note input's port declared, not just note_dialect
    double rate;  ///< the sample rate the instance is activated at
    uint32_t max_block;  ///< the largest block in frames
    int active;  ///< nonzero while the plugin is active
    float *bounce_map;          ///< mmap: [guard][6 x max_block floats][guard]
    size_t bounce_map_len;  ///< the size of the bounce mapping in bytes
    struct omx_clap_param_record *recs;  ///< the parameter write queue storage
    uint32_t rec_cap;  ///< the capacity of the write queue in records

    // the threads: the control thread that opened it, and whoever holds the audio role
    pthread_t main_thread;  ///< the control thread that opened the instance
    pthread_t audio_thread;  ///< the thread designated as the audio thread
    int audio_role_held;        ///< nonzero while a thread is designated the audio thread

    uint32_t bypass_wanted;     ///< the commanded bypass, kept across a deactivate and applied to the next stage

    // one record per parameter id the host has written
    struct omx_clap_shadow *shadow;  ///< the read-back shadows, one per written parameter
    uint32_t shadow_cap,        ///< the capacity of the shadow array
             shadow_n;          ///< the shadows in use

    // what the plugin asked of the host, read by the control thread's tick
    _Atomic uint32_t restart_requested;  ///< the plugin asked for a restart
    _Atomic uint32_t callback_requested;  ///< the plugin asked for a main-thread callback
    _Atomic uint32_t flush_requested;  ///< the plugin asked for a parameter flush
    _Atomic uint32_t latency_changed;  ///< the plugin reported a latency change
    _Atomic uint32_t params_rescan_flags;  ///< the CLAP_PARAM_RESCAN_* flags the plugin asked for
    _Atomic uint32_t ports_rescan_requested;  ///< the plugin asked for a ports rescan
    _Atomic uint32_t state_dirty;  ///< the plugin's state changed
    _Atomic uint32_t thread_violations;     ///< a [main-thread] host call from the audio role
    _Atomic uint32_t log_calls;  ///< the count of log calls from the plugin
    char last_log[CLAP_HOST_LOG_BYTES];     ///< the last message, written by the control thread's tick
    _Atomic uint32_t log_pending;  ///< a log message is waiting in `log_ring`
    _Atomic uint32_t log_fresh;             ///< last_log changed since omx_clap_host_log_take
    char log_ring[CLAP_HOST_LOG_BYTES];     ///< a message lands here: one slot, a lock-free single-message ring

    // clap.track-info: what the host's get answers, set by omx_clap_host_track_info_set; the plugin's own extension
    const clap_plugin_track_info_t *track_info;  ///< the plugin's clap.track-info extension
    clap_track_info_t track;  ///< the track information the host answers with
    int track_set;  ///< nonzero once the track information is set
    // clap.remote-controls: the plugin's own extension, and its call of the host's changed, read and cleared by
    // omx_clap_host_remote_controls_changed
    const clap_plugin_remote_controls_t *remote_controls;  ///< the plugin's clap.remote-controls extension
    _Atomic uint32_t remote_controls_changed;  ///< the plugin called the host's remote-controls changed
    // the audio role beyond `audio_thread`: a predicate and its context, written under `audio_role_seq` (odd while a
    // write is under way) so a reader never pairs one predicate with another's context
    _Atomic uint32_t audio_role_seq;  ///< the sequence guarding the audio role predicate and its context, odd while a write is under way
    _Atomic omx_clap_audio_role_fn audio_role_is;  ///< the predicate naming the threads that hold the audio role
    void *_Atomic audio_role_ctx;  ///< the context passed to `audio_role_is`
    // the tempo omx_clap_host_run carries (NULL: none), set by omx_clap_host_set_tempo, and the transport record it
    // hands the plugin, written only inside the stage's cycle on a processing block, never while the stage is idle,
    // warming up or held
    const struct omx_clap_tempo *tempo;  ///< the tempo word carried by omx_clap_host_run, NULL for none
    clap_event_transport_t transport;  ///< the transport record handed to the plugin

    // the auxiliary inputs (ABI 3): one port each, 1 or 2 channels, and the caller's pair bound to it (NULL: the silence)
    uint32_t aux_inputs;  ///< auxiliary input ports, after the main one
    uint32_t aux_in_channels[CLAP_HOST_AUX_INPUTS];  ///< the channel count of each auxiliary input port
    float *aux_in_bound[CLAP_HOST_AUX_INPUTS][2];  ///< the caller's buffers per port and channel, NULL: silence
};


/*
************************************************************************************************************************
*           FUNCTION PROTOTYPES
************************************************************************************************************************
*/

/** The defaults: clamp, scan and warm-up on, note inputs refused, the declared extensions only, the host named for the
 * library. Fills
 * `abi` and `size` too: start from it. */
OMX_CLAP_EXPORT void omx_clap_host_config_default(struct omx_clap_host_config *config);

/** Set the process's configuration, once, before the first binary is opened. -1: the ABI differs, a binary is open
 * already or a configuration was set. */
OMX_CLAP_EXPORT int omx_clap_host_configure(const struct omx_clap_host_config *config);

/** The version of the library at run time, major * 10000 + minor * 100 + patch. */
OMX_CLAP_EXPORT uint32_t omx_clap_core_version(void);

/** a plugin file and one instance of a plugin in it with no layout judged: what the host and a scanner both start from */
OMX_CLAP_EXPORT struct omx_clap_binary *omx_clap_host_binary_open(const char *path, char *reason, size_t reason_size);
/** Close a plugin file opened by omx_clap_host_binary_open, once every instance created from it is destroyed. */
OMX_CLAP_EXPORT void omx_clap_host_binary_close(struct omx_clap_binary *binary);
/** The number of plugins the file declares. */
OMX_CLAP_EXPORT uint32_t omx_clap_host_binary_count(const struct omx_clap_binary *binary);
/** The descriptor of the plugin at `index` in the file, NULL for an index out of range. */
OMX_CLAP_EXPORT const clap_plugin_descriptor_t *omx_clap_host_binary_descriptor(const struct omx_clap_binary *binary, uint32_t index);
/** The number of plugin files open in the process. */
OMX_CLAP_EXPORT uint32_t omx_clap_host_binaries_open(void);
/** Create one instance of the plugin `desc` describes, with no layout judged, into `*out`: 0, or -1 on a refusal. */
OMX_CLAP_EXPORT int omx_clap_host_create(struct omx_clap_binary *binary, const clap_plugin_descriptor_t *desc, struct omx_clap_instance **out);

/**
 * Load the .clap at `path`, create the plugin `id` (NULL: the factory's first descriptor), init it and read its
 * extensions, each step judged: the descriptor must carry audio-effect (or, where the configuration admits note inputs,
 * instrument), init must succeed headless with only the declared host extensions offered, the audio ports must declare
 * one main output and one main input of the same width, 1 or 2 channels (no main input for an instrument), no note input
 * unless the configuration admits one. An auxiliary output is admitted and left unconnected; an auxiliary input (a side
 * chain) is admitted and fed silence until the caller binds its buffers with omx_clap_host_bind_aux_input.
 * The read-back table of the parameters must be allocated, or the open is refused (CLAP_HOST_CODE_HEADLESS_FAILED).
 * Returns 0 with `*out` set, or -1 with `why` the deciding hosting code and nothing left loaded. The binary is
 * reference-counted: one dlopen per path, deinit and dlclose after its last instance.
 */
OMX_CLAP_EXPORT int omx_clap_host_open(const char *path, const char *id, struct omx_clap_instance **out, char why[OMX_CLAP_WHY_MAX]);

/** The registry of a process's own plugins, linked in: `entry` is one of them, `id` the descriptor to create. The same
 * judgment and codes; the binary is keyed by the entry, init once and deinit after its last instance, and nothing is
 * loaded or unloaded. */
OMX_CLAP_EXPORT int omx_clap_host_open_entry(const clap_plugin_entry_t *entry, const char *id, struct omx_clap_instance **out, char why[OMX_CLAP_WHY_MAX]);

/** The same as omx_clap_host_open behind a joinable timed worker: a file whose load never returns is abandoned after
 * `timeout_ms`, `why` reads CLAP_HOST_CODE_CRASHED_LIVE, and the caller is never blocked past the timeout. */
OMX_CLAP_EXPORT int omx_clap_host_open_timed(const char *path, const char *id, unsigned timeout_ms, struct omx_clap_instance **out, char why[OMX_CLAP_WHY_MAX]);

/**
 * Activate at `rate` for blocks up to `max_block`: take the bounce (guard pages around it) and the parameter ring, bind
 * the stage, and, where the configuration warms up, warm it up on this thread holding the audio role and restart the
 * plugin after it; read latency.get() and publish it. The stage is left IDLE: publishing is omx_clap_host_publish or
 * omx_clap_host_arm. -1 with `why` (CLAP_HOST_CODE_HEADLESS_FAILED when activate refuses, CLAP_HOST_CODE_OUTPUT_NON_FINITE
 * when the warm-up did).
 */
OMX_CLAP_EXPORT int omx_clap_host_activate(struct omx_clap_instance *in, double rate, uint32_t max_block, char why[OMX_CLAP_WHY_MAX]);

/** Publish: the stage is ARMED, the RT's first block takes the audio role. `rt` names the thread the RT body runs on. */
OMX_CLAP_EXPORT void omx_clap_host_publish(struct omx_clap_instance *in, pthread_t rt);

/** A client whose thread is known only once it runs (JACK's thread-init callback): name the audio thread, then arm. */
OMX_CLAP_EXPORT void omx_clap_host_set_audio_thread(struct omx_clap_instance *in, pthread_t thread);

/** The same for a walk split across threads: `rt` (or `thread`) holds the audio role, and so does every thread for which
 * `is_audio(ctx, self)` is nonzero. `ctx` stays valid until the instance is unpublished, which clears the predicate;
 * omx_clap_host_publish and omx_clap_host_set_audio_thread clear it too. A NULL `is_audio` names `rt` alone. */
OMX_CLAP_EXPORT void omx_clap_host_publish_role(struct omx_clap_instance *in, pthread_t rt, omx_clap_audio_role_fn is_audio, void *ctx);
/** The audio role of omx_clap_host_publish_role, set on a published instance. */
OMX_CLAP_EXPORT void omx_clap_host_set_audio_role(struct omx_clap_instance *in, pthread_t thread, omx_clap_audio_role_fn is_audio, void *ctx);
/** Arm the published instance: its stage starts to run the plugin on the next cycle. */
OMX_CLAP_EXPORT void omx_clap_host_arm(struct omx_clap_instance *in);

/** Unpublish: ask the RT to stop and wait, off the RT, polling every `poll_us` for at most `timeout_us`, for it to say
 * STOPPED. 0 when it did, -1 on the timeout (the RT is not calling the stage; the caller must not deactivate). A stage
 * that never ran stops at once. */
OMX_CLAP_EXPORT int omx_clap_host_unpublish(struct omx_clap_instance *in, unsigned poll_us, unsigned timeout_us);

/** Control thread, once no cycle can run anymore: stop_processing when the stage was still running, the audio role
 * released, the stage IDLE. */
OMX_CLAP_EXPORT void omx_clap_host_stop(struct omx_clap_instance *in);

/** While cycles keep running: stop the stage (taking the audio role when no cycle comes), deactivate, activate again at the
 * same rate and block, arm. The cycles pass the lane through until the plugin is back. -1 when it could not. */
OMX_CLAP_EXPORT int omx_clap_host_restart(struct omx_clap_instance *in);

/** Deactivate a STOPPED (or never published) instance and release the bounce and the ring. */
OMX_CLAP_EXPORT void omx_clap_host_deactivate(struct omx_clap_instance *in);

/** A rate change, off the RT: the instance must be unpublished; deactivate, activate at the new rate, re-read the latency. */
OMX_CLAP_EXPORT int omx_clap_host_set_rate(struct omx_clap_instance *in, double rate, uint32_t max_block, char why[OMX_CLAP_WHY_MAX]);

/** Destroy the instance (deactivating first if needed) and drop its binary reference. */
OMX_CLAP_EXPORT void omx_clap_host_close(struct omx_clap_instance *in);

/** The declared latency.get() as last published, in frames. */
OMX_CLAP_EXPORT uint32_t omx_clap_host_latency(const struct omx_clap_instance *in);

/** The control thread takes the audio role from whoever holds it: a cycle that arrives meanwhile passes the lane through,
 * one already running is waited for (CLAP_HOST_ROLE_TIMEOUT_US). `role` remembers what to give back. */
OMX_CLAP_EXPORT void omx_clap_host_take_role(struct omx_clap_instance *in, struct omx_clap_role *role);
/** Give the audio role back to what `role` remembers, in the `state` omx_clap_host_take_role left. */
OMX_CLAP_EXPORT void omx_clap_host_release_role(struct omx_clap_instance *in, const struct omx_clap_role *role, uint32_t state);

/** The host's own bypass: one crossfade to the dry lane on the next cycle, then the plugin idles. Off after on re-engages:
 * the control thread takes the audio role, resets a plugin that idled, and the stage fades back in. Off when already off
 * only records it: the stage is not held and the output does not move. */
OMX_CLAP_EXPORT void omx_clap_host_bypass(struct omx_clap_instance *in, int on);
/** Nonzero while the host's bypass is commanded on. */
OMX_CLAP_EXPORT int omx_clap_host_bypassed(const struct omx_clap_instance *in);

/** Parameter rows: how many, and the `index`-th, in get_info order with the non-rows skipped. Every read of the plugin's
 * count() is capped at CLAP_HOST_PARAM_COUNT_MAX (clap_host_limits.h): the rows past it are not served and their ids are
 * not looked up, and the open leaves one log message saying so (omx_clap_host_log_take). The roster is whole or refused: when an index below count() does not answer get_info, the count is 0 and every row is refused. Each call
 * walks the plugin's whole list once (O(n)), so reading every row by index is O(n^2): param_roster reads them in one. */
OMX_CLAP_EXPORT uint32_t omx_clap_host_param_count(struct omx_clap_instance *in);
/** The `index`-th parameter row: 0, or -1 for an index that is not a row. */
OMX_CLAP_EXPORT int omx_clap_host_param_row(struct omx_clap_instance *in, uint32_t index, struct omx_clap_param_row *row);

/** The whole roster in one call: every row, in get_info order with the non-rows skipped, read in one pass over count()
 * capped at CLAP_HOST_PARAM_COUNT_MAX. `*rows` is malloc'd (the caller frees it) and the count returned; 0 with `*rows` NULL when there is no
 * row (no parameter, or none of them a row). -1 with `*why` (when `why` is not NULL) OMX_CLAP_PARAM_ROW_UNREADABLE when
 * an index below count() does not answer get_info (never the rows read so far) or the rows cannot be allocated. */
OMX_CLAP_EXPORT int omx_clap_host_param_roster(struct omx_clap_instance *in, struct omx_clap_param_row **rows, const char **why);

/** A row write: one enqueue into the ring plus the shadow record. -1: the ring is full, the id is not a row or the
 * instance is not active. The value is the plugin's, unclamped. */
OMX_CLAP_EXPORT int omx_clap_host_param_write(struct omx_clap_instance *in, clap_id id, double value);

/** params.flush: the same drain run on the control thread while the instance is not processing (before publish, during a
 * rate change). -1 while it is. */
OMX_CLAP_EXPORT int omx_clap_host_param_flush(struct omx_clap_instance *in);

/** The same for a client nothing drives: take the audio role from a running cycle (or an armed stage) and deliver what
 * is queued through params.flush. */
OMX_CLAP_EXPORT void omx_clap_host_param_deliver(struct omx_clap_instance *in);

/** Wait up to `timeout_us` for the queued writes to be consumed by a cycle; when none came, deliver them. */
OMX_CLAP_EXPORT void omx_clap_host_settle(struct omx_clap_instance *in, unsigned timeout_us);

/** Read-back: params.get_value against the shadow. 1 when the delivered value was consumed and the plugin applied exactly
 * it, 0 when it differs (`*applied` says what the plugin holds), -1 when nothing was delivered or the plugin cannot
 * answer. Safe on the control thread while audio runs. */
OMX_CLAP_EXPORT int omx_clap_host_param_compare(struct omx_clap_instance *in, clap_id id, double *applied);

/** params.get_value, plainly. */
OMX_CLAP_EXPORT int omx_clap_host_param_read(struct omx_clap_instance *in, clap_id id, double *value);

/** Whether `id` is a row: a parameter the host may write. 0 for any id get_info does not know. */
OMX_CLAP_EXPORT int omx_clap_host_param_is_row(struct omx_clap_instance *in, clap_id id);

/** Whether `id` is a parameter the plugin lets be read (every one but the hidden), and the row of a writable one. */
OMX_CLAP_EXPORT int omx_clap_host_param_readable(struct omx_clap_instance *in, clap_id id);
/** The row of the parameter `id`: 0, or -1 for an id that is not a row. */
OMX_CLAP_EXPORT int omx_clap_host_param_row_of(struct omx_clap_instance *in, clap_id id, struct omx_clap_param_row *row);

/** What a param_get answers: the value delivered while the RT has not yet consumed it, the plugin's own once it has. */
OMX_CLAP_EXPORT int omx_clap_host_param_value(struct omx_clap_instance *in, clap_id id, double *value);

/** state.save into `buf` (at most `cap` bytes): 0 with `*len`; -1 when the plugin has no state extension, wrote past `cap`
 * (refused, never truncated) or refused. state.load from `buf`, before activate or after. */
OMX_CLAP_EXPORT int omx_clap_host_state_save(struct omx_clap_instance *in, void *buf, size_t cap, size_t *len);
/** Load the plugin state from `buf`, `len` bytes: 0, or -1 when the plugin has no state extension or refuses it. */
OMX_CLAP_EXPORT int omx_clap_host_state_load(struct omx_clap_instance *in, const void *buf, size_t len);

/** preset-load from a file location; -1 when the plugin has no such extension or refuses. */
OMX_CLAP_EXPORT int omx_clap_host_preset_load(struct omx_clap_instance *in, const char *location);

/** clap.track-info, where the configuration offers it: store what the host's get answers from now on and call the
 * plugin's changed. `name` NULL or "" for none, `color` NULL for none; `flags` the CLAP_TRACK_INFO_IS_FOR_* bits, the
 * HAS_ bits are set here. The name is cut at CLAP_NAME_SIZE - 1 bytes. -1 when the configuration does not offer it. */
OMX_CLAP_EXPORT int omx_clap_host_track_info_set(struct omx_clap_instance *in, const char *name, const clap_color_t *color,
                                                 uint64_t flags);

/** Whether the plugin called the host's remote_controls.changed since the last call; reading clears it. */
OMX_CLAP_EXPORT int omx_clap_host_remote_controls_changed(struct omx_clap_instance *in);

/** The control thread's tick: run on_main_thread if the plugin asked, drain the log, republish a changed latency. Returns
 * nonzero when the plugin asked for a restart (the caller performs it). */
OMX_CLAP_EXPORT int omx_clap_host_tick(struct omx_clap_instance *in);

/** The last log message the tick drained, once: NULL when it was taken already. */
OMX_CLAP_EXPORT const char *omx_clap_host_log_take(struct omx_clap_instance *in);

/** Whether the plugin declares `feature` (a CLAP_PLUGIN_FEATURE_* string). */
OMX_CLAP_EXPORT int omx_clap_host_has_feature(const clap_plugin_descriptor_t *desc, const char *feature);


/*
************************************************************************************************************************
*           END HEADER
************************************************************************************************************************
*/

/** Control thread, before publish: the tempo omx_clap_host_run hands the plugin from now on (NULL: none). The word must
 * outlive the instance's publication. */
OMX_CLAP_EXPORT void omx_clap_host_set_tempo(struct omx_clap_instance *in, const struct omx_clap_tempo *tempo);

/**
 * Control thread, while the instance is not running (never published, or unpublished): bind the buffers auxiliary input
 * `port` reads, `l` for its first channel and `r` for its second (a mono port reads `l` only). `l` NULL unbinds it, back
 * to the silence every unbound port reads. The buffers are not copied: each must hold as many frames as the largest block
 * passed to omx_clap_host_activate. The caller keeps them alive until it unbinds or closes the instance, and fills them on
 * the audio role before each block that runs the instance. The binding outlives a deactivate and activate. Returns 0, or
 * -1 when `port` is not an auxiliary input, a stereo port is bound without `r`, or the instance is running.
 */
OMX_CLAP_EXPORT int omx_clap_host_bind_aux_input(struct omx_clap_instance *in, uint32_t port, float *l, float *r);


/*
************************************************************************************************************************
*           INLINE FUNCTIONS
************************************************************************************************************************
*/

/** Control thread: publish the bpm (a finite bpm > 0; anything else withdraws the tempo). Seen by the next block. */
static inline void omx_clap_tempo_publish(struct omx_clap_tempo *tempo, double bpm)
{
    uint64_t bits = 0;

    if (isfinite(bpm) && bpm > 0.0)
        memcpy(&bits, &bpm, sizeof(bits));
    atomic_store_explicit(&tempo->bpm_bits, bits, memory_order_relaxed);
}

/** Any thread: the published bpm, 0 when none is. */
static inline double omx_clap_tempo_read(const struct omx_clap_tempo *tempo)
{
    const uint64_t bits = atomic_load_explicit(&tempo->bpm_bits, memory_order_relaxed);
    double bpm;

    memcpy(&bpm, &bits, sizeof(bpm));
    return bits ? bpm : 0.0;
}

/** The audio role, one block: the instance's stage over `l` (and `r`, NULL for a mono lane) for `n` frames, the plugin's
 * transport carrying the tempo the instance was given (NULL when it has none or the word holds none). Read once per
 * block, relaxed: a change is seen by the next block. The transport record is written only inside the stage's cycle,
 * on a processing block (omx_clap_run_transport); this function itself writes nothing of the instance. */
static inline void omx_clap_host_run(struct omx_clap_instance *in, float *l, float *r, uint32_t n)
{
    const double bpm = in->tempo ? omx_clap_tempo_read(in->tempo) : 0.0;

    omx_clap_run_transport(&in->stage, l, r, n, &in->transport, bpm);
}

#endif
