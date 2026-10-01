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
* The control-thread host of libomx-clap-core: the only unit that loads a CLAP file and the CLAP main thread of every
* instance it hosts. It owns everything clap_stage.h (the RT body) does not: load, judge the ports, activate, warm up,
* publish, unpublish, rate change, restart, parameters as rows, the read-back shadow, state, latency, the host object.
*
* Every function here runs on the control thread unless its comment says otherwise. None is called from the RT: the RT
* sees only the stage, through omx_clap_run. Functions return 0 or -1, and name a refusal by the hosting code of
* clap_host_limits.h in `why`.
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
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "clap_stage.h"


/*
************************************************************************************************************************
*           CONFIGURATION DEFINES
************************************************************************************************************************
*/

/* The version of the structures below, the stage's and the instance's: a field is only ever appended, and a removal or
 * a reorder is a new major of the library with a new ABI number. */
#define OMX_CLAP_CORE_ABI               1u

/* How many bytes a refusal's hosting code needs, with its NUL. */
#define OMX_CLAP_WHY_MAX                64

#if defined(__GNUC__)
#define OMX_CLAP_EXPORT                 __attribute__((visibility("default")))
#else
#define OMX_CLAP_EXPORT
#endif


/*
************************************************************************************************************************
*           DATA TYPES
************************************************************************************************************************
*/

/* What a host differs in from the defaults, set once per process. The strings must outlive the process's use. */
struct omx_clap_host_config
{
    uint32_t abi;               // OMX_CLAP_CORE_ABI of the header the caller was compiled against
    uint32_t size;              // sizeof of this structure as the caller was compiled: a field appended later is the default to it
    int clamp;                  // clamp the plugin's output at CLAP_HOST_CLAMP_DBFS
    int nonfinite;              // scan the output for non-finite samples and strike the stage on them
    int warmup;                 // warm the plugin up before publish and restart it after
    int note_inputs;            // admit an instrument and one note input; refuse any note input when 0
    int preset_load;            // offer the clap.preset-load host extension besides the declared list
    const char *name;           // the host the plugin is told it is in
    const char *vendor;
    const char *url;
    const char *version;
    int track_info;             // offer clap.track-info besides the declared list: get answers omx_clap_host_track_info_set
    int remote_controls;        // offer clap.remote-controls besides the declared list: see omx_clap_host_remote_controls_changed
};

/* One parameter of a hosted plugin as a row: what a slot serves. A parameter that is not a row (hidden, read-only, the
 * plugin's own bypass) is never enumerated. The units are the plugin's own. */
struct omx_clap_param_row
{
    clap_id id;
    char name[CLAP_NAME_SIZE];
    double min, max, def;
    int stepped;                // CLAP_PARAM_IS_STEPPED: integer travel
    int enumerated;             // CLAP_PARAM_IS_ENUM
    void *cookie;               // what get_info returned; rides every event for this id
};

/* The read-back shadow of one parameter: what the host delivered and the cycle that consumed it, kept beside what the
 * plugin says it applied. */
struct omx_clap_shadow
{
    clap_id id;
    double delivered;
    uint32_t cycle;             // the stage's `runs` when the write was enqueued; consumed once runs > cycle
    int valid;
};

struct omx_clap_binary;         // one loaded .clap, or one linked entry, reference-counted across its instances

/* What the control thread displaces while it holds the audio role: see omx_clap_host_take_role. */
struct omx_clap_role
{
    uint32_t state;
    pthread_t thread;
    int held;
};

/* One hosted instance. Opaque to the RT except for `stage`, which the consumer's RT thread runs. */
struct omx_clap_instance
{
    struct omx_clap_binary *bin;
    const clap_plugin_descriptor_t *desc;
    const clap_plugin_t *plugin;
    const clap_plugin_params_t *params;
    const clap_plugin_latency_t *latency;
    const clap_plugin_audio_ports_t *audio_ports;
    const clap_plugin_note_ports_t *note_ports;
    const clap_plugin_state_t *state;
    const clap_plugin_preset_load_t *preset_load;
    clap_host_t host;           // this instance's host object; host_data is the instance

    struct omx_clap_stage stage;
    uint32_t channels;          // the main output width the ports admitted (1 or 2); an effect's input has the same
    uint32_t in_channels;       // the main input width: 0 for an instrument
    uint32_t aux_outputs;       // auxiliary output ports, left unconnected
    uint32_t aux_channels[CLAP_HOST_AUX_OUTPUTS];
    uint32_t note_inputs;       // 0 or 1; the dialect the host feeds it
    uint32_t note_dialect;
    double rate;
    uint32_t max_block;
    int active;
    float *bounce_map;          // mmap: [guard][6 x max_block floats][guard]
    size_t bounce_map_len;
    struct omx_clap_param_record *recs;
    uint32_t rec_cap;

    // the threads: the control thread that opened it, and whoever holds the audio role
    pthread_t main_thread;
    pthread_t audio_thread;
    int audio_role_held;        // nonzero while a thread is designated the audio thread

    uint32_t bypass_wanted;     // the commanded bypass, kept across a deactivate and applied to the next stage

    // one record per parameter id the host has written
    struct omx_clap_shadow *shadow;
    uint32_t shadow_cap, shadow_n;

    // what the plugin asked of the host, read by the control thread's tick
    _Atomic uint32_t restart_requested;
    _Atomic uint32_t callback_requested;
    _Atomic uint32_t flush_requested;
    _Atomic uint32_t latency_changed;
    _Atomic uint32_t params_rescan_flags;
    _Atomic uint32_t ports_rescan_requested;
    _Atomic uint32_t state_dirty;
    _Atomic uint32_t thread_violations;     // a [main-thread] host call from the audio role
    _Atomic uint32_t log_calls;
    char last_log[CLAP_HOST_LOG_BYTES];     // the last message, written by the control thread's tick
    _Atomic uint32_t log_pending;
    _Atomic uint32_t log_fresh;             // last_log changed since omx_clap_host_log_take
    char log_ring[CLAP_HOST_LOG_BYTES];     // a message lands here: one slot, a lock-free single-message ring

    // clap.track-info: what the host's get answers, set by omx_clap_host_track_info_set; the plugin's own extension
    const clap_plugin_track_info_t *track_info;
    clap_track_info_t track;
    int track_set;
    // clap.remote-controls: the plugin's own extension, and its call of the host's changed, read and cleared by
    // omx_clap_host_remote_controls_changed
    const clap_plugin_remote_controls_t *remote_controls;
    _Atomic uint32_t remote_controls_changed;
};


/*
************************************************************************************************************************
*           FUNCTION PROTOTYPES
************************************************************************************************************************
*/

/* The defaults: clamp, scan and warm-up on, note inputs refused, the declared extensions only, the host named for the
 * library. Fills
 * `abi` and `size` too: start from it. */
OMX_CLAP_EXPORT void omx_clap_host_config_default(struct omx_clap_host_config *config);

/* Set the process's configuration, once, before the first binary is opened. -1: the ABI differs, a binary is open
 * already or a configuration was set. */
OMX_CLAP_EXPORT int omx_clap_host_configure(const struct omx_clap_host_config *config);

/* The version of the library at run time, major * 10000 + minor * 100 + patch. */
OMX_CLAP_EXPORT uint32_t omx_clap_core_version(void);

/* a plugin file and one instance of a plugin in it with no layout judged: what the host and a scanner both start from */
OMX_CLAP_EXPORT struct omx_clap_binary *omx_clap_host_binary_open(const char *path, char *reason, size_t reason_size);
OMX_CLAP_EXPORT void omx_clap_host_binary_close(struct omx_clap_binary *binary);
OMX_CLAP_EXPORT uint32_t omx_clap_host_binary_count(const struct omx_clap_binary *binary);
OMX_CLAP_EXPORT const clap_plugin_descriptor_t *omx_clap_host_binary_descriptor(const struct omx_clap_binary *binary, uint32_t index);
OMX_CLAP_EXPORT uint32_t omx_clap_host_binaries_open(void);
OMX_CLAP_EXPORT int omx_clap_host_create(struct omx_clap_binary *binary, const clap_plugin_descriptor_t *desc, struct omx_clap_instance **out);

/*
 * Load the .clap at `path`, create the plugin `id` (NULL: the factory's first descriptor), init it and read its
 * extensions, each step judged: the descriptor must carry audio-effect (or, where the configuration admits note inputs,
 * instrument), init must succeed headless with only the declared host extensions offered, the audio ports must declare
 * one main output and one main input of the same width, 1 or 2 channels (no main input for an instrument), no extra
 * input, and no note input unless the configuration admits one. An auxiliary output is admitted and left unconnected.
 * Returns 0 with `*out` set, or -1 with `why` the deciding hosting code and nothing left loaded. The binary is
 * reference-counted: one dlopen per path, deinit and dlclose after its last instance.
 */
OMX_CLAP_EXPORT int omx_clap_host_open(const char *path, const char *id, struct omx_clap_instance **out, char why[OMX_CLAP_WHY_MAX]);

/* The registry of a process's own plugins, linked in: `entry` is one of them, `id` the descriptor to create. The same
 * judgment and codes; the binary is keyed by the entry, init once and deinit after its last instance, and nothing is
 * loaded or unloaded. */
OMX_CLAP_EXPORT int omx_clap_host_open_entry(const clap_plugin_entry_t *entry, const char *id, struct omx_clap_instance **out, char why[OMX_CLAP_WHY_MAX]);

/* The same as omx_clap_host_open behind a joinable timed worker: a file whose load never returns is abandoned after
 * `timeout_ms`, `why` reads CLAP_HOST_CODE_CRASHED_LIVE, and the caller is never blocked past the timeout. */
OMX_CLAP_EXPORT int omx_clap_host_open_timed(const char *path, const char *id, unsigned timeout_ms, struct omx_clap_instance **out, char why[OMX_CLAP_WHY_MAX]);

/*
 * Activate at `rate` for blocks up to `max_block`: take the bounce (guard pages around it) and the parameter ring, bind
 * the stage, and, where the configuration warms up, warm it up on this thread holding the audio role and restart the
 * plugin after it; read latency.get() and publish it. The stage is left IDLE: publishing is omx_clap_host_publish or
 * omx_clap_host_arm. -1 with `why` (CLAP_HOST_CODE_HEADLESS_FAILED when activate refuses, CLAP_HOST_CODE_OUTPUT_NON_FINITE
 * when the warm-up did).
 */
OMX_CLAP_EXPORT int omx_clap_host_activate(struct omx_clap_instance *in, double rate, uint32_t max_block, char why[OMX_CLAP_WHY_MAX]);

/* Publish: the stage is ARMED, the RT's first block takes the audio role. `rt` names the thread the RT body runs on. */
OMX_CLAP_EXPORT void omx_clap_host_publish(struct omx_clap_instance *in, pthread_t rt);

/* A client whose thread is known only once it runs (JACK's thread-init callback): name the audio thread, then arm. */
OMX_CLAP_EXPORT void omx_clap_host_set_audio_thread(struct omx_clap_instance *in, pthread_t thread);
OMX_CLAP_EXPORT void omx_clap_host_arm(struct omx_clap_instance *in);

/* Unpublish: ask the RT to stop and wait, off the RT, polling every `poll_us` for at most `timeout_us`, for it to say
 * STOPPED. 0 when it did, -1 on the timeout (the RT is not calling the stage; the caller must not deactivate). A stage
 * that never ran stops at once. */
OMX_CLAP_EXPORT int omx_clap_host_unpublish(struct omx_clap_instance *in, unsigned poll_us, unsigned timeout_us);

/* Control thread, once no cycle can run anymore: stop_processing when the stage was still running, the audio role
 * released, the stage IDLE. */
OMX_CLAP_EXPORT void omx_clap_host_stop(struct omx_clap_instance *in);

/* While cycles keep running: stop the stage (taking the audio role when no cycle comes), deactivate, activate again at the
 * same rate and block, arm. The cycles pass the lane through until the plugin is back. -1 when it could not. */
OMX_CLAP_EXPORT int omx_clap_host_restart(struct omx_clap_instance *in);

/* Deactivate a STOPPED (or never published) instance and release the bounce and the ring. */
OMX_CLAP_EXPORT void omx_clap_host_deactivate(struct omx_clap_instance *in);

/* A rate change, off the RT: the instance must be unpublished; deactivate, activate at the new rate, re-read the latency. */
OMX_CLAP_EXPORT int omx_clap_host_set_rate(struct omx_clap_instance *in, double rate, uint32_t max_block, char why[OMX_CLAP_WHY_MAX]);

/* Destroy the instance (deactivating first if needed) and drop its binary reference. */
OMX_CLAP_EXPORT void omx_clap_host_close(struct omx_clap_instance *in);

/* The declared latency.get() as last published, in frames. */
OMX_CLAP_EXPORT uint32_t omx_clap_host_latency(const struct omx_clap_instance *in);

/* The control thread takes the audio role from whoever holds it: a cycle that arrives meanwhile passes the lane through,
 * one already running is waited for (CLAP_HOST_ROLE_TIMEOUT_US). `role` remembers what to give back. */
OMX_CLAP_EXPORT void omx_clap_host_take_role(struct omx_clap_instance *in, struct omx_clap_role *role);
OMX_CLAP_EXPORT void omx_clap_host_release_role(struct omx_clap_instance *in, const struct omx_clap_role *role, uint32_t state);

/* The host's own bypass: one crossfade to the dry lane on the next cycle, then the plugin idles. */
OMX_CLAP_EXPORT void omx_clap_host_bypass(struct omx_clap_instance *in, int on);
OMX_CLAP_EXPORT int omx_clap_host_bypassed(const struct omx_clap_instance *in);

/* Parameter rows: how many, and the `index`-th, in get_info order with the non-rows skipped. */
OMX_CLAP_EXPORT uint32_t omx_clap_host_param_count(struct omx_clap_instance *in);
OMX_CLAP_EXPORT int omx_clap_host_param_row(struct omx_clap_instance *in, uint32_t index, struct omx_clap_param_row *row);

/* A row write: one enqueue into the ring plus the shadow record. -1: the ring is full, the id is not a row or the
 * instance is not active. The value is the plugin's, unclamped. */
OMX_CLAP_EXPORT int omx_clap_host_param_write(struct omx_clap_instance *in, clap_id id, double value);

/* params.flush: the same drain run on the control thread while the instance is not processing (before publish, during a
 * rate change). -1 while it is. */
OMX_CLAP_EXPORT int omx_clap_host_param_flush(struct omx_clap_instance *in);

/* The same for a client nothing drives: take the audio role from a running cycle (or an armed stage) and deliver what
 * is queued through params.flush. */
OMX_CLAP_EXPORT void omx_clap_host_param_deliver(struct omx_clap_instance *in);

/* Wait up to `timeout_us` for the queued writes to be consumed by a cycle; when none came, deliver them. */
OMX_CLAP_EXPORT void omx_clap_host_settle(struct omx_clap_instance *in, unsigned timeout_us);

/* Read-back: params.get_value against the shadow. 1 when the delivered value was consumed and the plugin applied exactly
 * it, 0 when it differs (`*applied` says what the plugin holds), -1 when nothing was delivered or the plugin cannot
 * answer. Safe on the control thread while audio runs. */
OMX_CLAP_EXPORT int omx_clap_host_param_compare(struct omx_clap_instance *in, clap_id id, double *applied);

/* params.get_value, plainly. */
OMX_CLAP_EXPORT int omx_clap_host_param_read(struct omx_clap_instance *in, clap_id id, double *value);

/* Whether `id` is a row: a parameter the host may write. 0 for any id get_info does not know. */
OMX_CLAP_EXPORT int omx_clap_host_param_is_row(struct omx_clap_instance *in, clap_id id);

/* Whether `id` is a parameter the plugin lets be read (every one but the hidden), and the row of a writable one. */
OMX_CLAP_EXPORT int omx_clap_host_param_readable(struct omx_clap_instance *in, clap_id id);
OMX_CLAP_EXPORT int omx_clap_host_param_row_of(struct omx_clap_instance *in, clap_id id, struct omx_clap_param_row *row);

/* What a param_get answers: the value delivered while the RT has not yet consumed it, the plugin's own once it has. */
OMX_CLAP_EXPORT int omx_clap_host_param_value(struct omx_clap_instance *in, clap_id id, double *value);

/* state.save into `buf` (at most `cap` bytes): 0 with `*len`; -1 when the plugin has no state extension, wrote past `cap`
 * (refused, never truncated) or refused. state.load from `buf`, before activate or after. */
OMX_CLAP_EXPORT int omx_clap_host_state_save(struct omx_clap_instance *in, void *buf, size_t cap, size_t *len);
OMX_CLAP_EXPORT int omx_clap_host_state_load(struct omx_clap_instance *in, const void *buf, size_t len);

/* preset-load from a file location; -1 when the plugin has no such extension or refuses. */
OMX_CLAP_EXPORT int omx_clap_host_preset_load(struct omx_clap_instance *in, const char *location);

/* clap.track-info, where the configuration offers it: store what the host's get answers from now on and call the
 * plugin's changed. `name` NULL or "" for none, `color` NULL for none; `flags` the CLAP_TRACK_INFO_IS_FOR_* bits, the
 * HAS_ bits are set here. The name is cut at CLAP_NAME_SIZE - 1 bytes. -1 when the configuration does not offer it. */
OMX_CLAP_EXPORT int omx_clap_host_track_info_set(struct omx_clap_instance *in, const char *name, const clap_color_t *color,
                                                 uint64_t flags);

/* Whether the plugin called the host's remote_controls.changed since the last call; reading clears it. */
OMX_CLAP_EXPORT int omx_clap_host_remote_controls_changed(struct omx_clap_instance *in);

/* The control thread's tick: run on_main_thread if the plugin asked, drain the log, republish a changed latency. Returns
 * nonzero when the plugin asked for a restart (the caller performs it). */
OMX_CLAP_EXPORT int omx_clap_host_tick(struct omx_clap_instance *in);

/* The last log message the tick drained, once: NULL when it was taken already. */
OMX_CLAP_EXPORT const char *omx_clap_host_log_take(struct omx_clap_instance *in);

/* Whether the plugin declares `feature` (a CLAP_PLUGIN_FEATURE_* string). */
OMX_CLAP_EXPORT int omx_clap_host_has_feature(const clap_plugin_descriptor_t *desc, const char *feature);


/*
************************************************************************************************************************
*           END HEADER
************************************************************************************************************************
*/

#endif
