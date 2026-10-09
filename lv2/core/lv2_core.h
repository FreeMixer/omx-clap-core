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
* The LV2 host of libomx-clap-lv2, with no CLAP in it: lilv loaded lazily, one bundle at a time, its ports read and
* refused with the verdict's codes before any binary is opened, the feature providers, one instance's lifecycle on the
* main thread, its worker thread, and the LV2 half of a block on the audio thread. lv2/clap is the shim that presents it
* as a CLAP plugin. Internal to the library: nothing here is installed.
*
* Moved from the console (packages/pipewire-native): the bundle reader and the refusals are mix_lv2_host.c's, the
* providers and the worker mix_host_backend.c's LV2 half, the run half mix_lv2.h's. The guards that surround a run (the
* bounce, the crossfade, the clamp, the strikes, the warm-up) are not here: they are the CLAP body's, hosted_stage.h.
*
* Threads. Every function runs on the main thread unless its comment says otherwise. lv2_instance_run and the control
* and output accessors it names run on the audio thread, and touch no lilv, no lock, no allocation and no system call.
* The worker thread calls the plugin's work() and the URID map, nothing else.
*
************************************************************************************************************************
*/

#ifndef LV2_CORE_H
#define LV2_CORE_H


/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <stdatomic.h>
#include <stdint.h>

#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/log/log.h>
#include <lv2/options/options.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>


/*
************************************************************************************************************************
*           CONFIGURATION DEFINES
************************************************************************************************************************
*/

/* bytes a refusal code needs, with its NUL */
#define LV2_CORE_WHY_MAX                96

/* the library the first bundle loads unless the configuration names another */
#define LV2_CORE_LILV_SONAME            "liblilv-0.so.0"

/* every feature URI a provider here serves, NULL-terminated: what lv2_core_provided holds, for a list of the same */
#define LV2_CORE_PROVIDED_INIT \
    { LV2_URID__map, LV2_URID__unmap, LV2_OPTIONS__options, LV2_BUF_SIZE__boundedBlockLength, LV2_WORKER__schedule, \
      LV2_STATE__loadDefaultState, LV2_LOG__log, NULL }

/* the refusals, the verdict's spelling */
#define LV2_CODE_NO_REALISATION         "hosting.no-realisation"
#define LV2_CODE_MIDI_IN                "hosting.features.midi-in-fed-empty"
#define LV2_CODE_CV_PORTS               "hosting.features.cv-ports"
#define LV2_CODE_FEATURES_MISSING       "hosting.features.missing"
#define LV2_CODE_NO_AUDIO_INPUT         "hosting.topology.no-audio-input"
#define LV2_CODE_NO_AUDIO_OUTPUT        "hosting.topology.no-audio-output"
#define LV2_CODE_EXTRA_INPUTS           "hosting.topology.extra-inputs-fed-silence"
#define LV2_CODE_WIDER_THAN_STRIP       "hosting.topology.wider-than-strip"
#define LV2_CODE_STATE_UNREADABLE       "hosting.state.unreadable"

/* what a control port declares, as lv2_control.props reads it */
#define LV2_PROP_INTEGER                (1u << 0)
#define LV2_PROP_TOGGLED                (1u << 1)
#define LV2_PROP_ENUMERATION            (1u << 2)
#define LV2_PROP_ENUM_COMPLETE          (1u << 3)   // a scale point on every integer from min to max
#define LV2_PROP_NOT_ON_GUI             (1u << 4)
#define LV2_PROP_TRIGGER                (1u << 5)
#define LV2_PROP_LOGARITHMIC            (1u << 6)


/*
************************************************************************************************************************
*           DATA TYPES
************************************************************************************************************************
*/

/* What the host hands the LV2 host once: the features it may provide, NULL-terminated, and the numbers it obeys. */
struct lv2_core_config
{
    const char *const *features;
    uint32_t worker_poll_us;
    uint32_t worker_ring_bytes;     // a power of two, >= 16
    uint32_t atom_buffer_bytes;
    const char *lilv_soname;        // NULL: LV2_CORE_LILV_SONAME
};

/* What a control port is to the instance. */
enum lv2_control_kind
{
    LV2_CONTROL_INPUT = 0,          // a row: written by the host
    LV2_CONTROL_BYPASS = 1,         // the plugin's own bypass (lv2:enabled, or a port whose symbol is bypass): held
    LV2_CONTROL_OUTPUT = 2,         // an output other than the latency port: the plugin writes it, the host reads it
};

struct lv2_scale_point
{
    float value;
    char *label;
};

/* One control port, in port index order. */
struct lv2_control
{
    uint32_t port;
    enum lv2_control_kind kind;
    char *symbol;
    char *name;
    float min, max, def;            // def: lv2:default, else lv2:minimum, else 0; the hold value for the bypass
    uint32_t props;                 // LV2_PROP_*
    struct lv2_scale_point *points;
    uint32_t n_points;
};

/* One loaded bundle, reference-counted. */
struct lv2_bundle;

/* One plugin read from its bundle: its ports and required features, read before its binary is opened. */
struct lv2_plugin
{
    struct lv2_bundle *bundle;
    char *uri;
    char *bundle_path;              // the directory, with its trailing '/'
    uint32_t n_ports;
    uint32_t legs;                  // 1 or 2
    uint32_t in_ports[2], out_ports[2];
    int32_t latency_port;           // -1: none
    struct lv2_control *controls;   // every control port but the latency port
    uint32_t n_controls;
    char **required;                // lv2:requiredFeature, NULL-terminated
    void *binary;                   // dlopen'ed by lv2_plugin_load
    const LV2_Descriptor *desc;
    void *uri_node;                 // the lilv node of the URI, for the default state
    void *lilv_plugin;              // the LilvPlugin, owned by the world: for the state the instance saves
};

/* One running instance of a plugin. Opaque. */
struct lv2_instance;

/* What the host supplies to say whether the calling thread holds the audio role of the instance (its RT thread, a
 * worker of a split chain, or the control thread during the warm-up). */
typedef int (*lv2_audio_role_fn)(void *ctx);

/* What the instance counts, read relaxed. */
struct lv2_counters
{
    uint32_t schedule_refused;      // schedule_work on a full request ring (NO_SPACE)
    uint32_t responses_refused;     // respond on a full response ring (NO_SPACE)
    uint32_t respond_strikes;       // respond() called outside work(): refused
    uint32_t map_on_audio;          // urid:map called on the audio role: a thread violation
    uint32_t schedule_off_audio;    // schedule_work called off the audio role: a thread violation
    uint32_t log_on_audio;          // log:log called on the audio role (the sink writes nothing either way)
};


/*
************************************************************************************************************************
*           FUNCTION PROTOTYPES
************************************************************************************************************************
*/

/* Once per process: 0, or -1 with `why`. A list naming a feature no provider serves, a ring size that is not a power
 * of two >= 16, or a poll of 0 is refused; a second call is refused unless it says exactly what the first said. */
int lv2_core_configure(const struct lv2_core_config *config, char why[LV2_CORE_WHY_MAX]);

/* The configuration taken, or NULL before lv2_core_configure. */
const struct lv2_core_config *lv2_core_config(void);

/* Whether lilv is mapped into the process by this library: 0 until the first bundle is loaded. */
int lv2_core_lilv_loaded(void);

/* Every feature URI a provider here serves, NULL-terminated. */
extern const char *const lv2_core_provided[];

/* The bundle at `path` (an absolute directory, with or without its trailing '/'), loaded on its first reference, lilv
 * loaded on the first bundle: all or nothing, and a failed load does not latch. NULL with `why` set when lilv cannot be
 * loaded, the library is not configured, or the bundle holds no plugin. */
struct lv2_bundle *lv2_bundle_ref(const char *path, char why[LV2_CORE_WHY_MAX]);
void lv2_bundle_unref(struct lv2_bundle *bundle);

/* The plugins the bundle holds (its own, never another bundle's), with their URI and name. */
uint32_t lv2_bundle_count(const struct lv2_bundle *bundle);
const char *lv2_bundle_uri(const struct lv2_bundle *bundle, uint32_t index);
const char *lv2_bundle_name(const struct lv2_bundle *bundle, uint32_t index);

/* Read the plugin `uri` of `bundle`: its ports and its required features, refused with the verdict's code before any
 * binary is opened, including a required feature outside the configured list. Takes a reference on the bundle. */
struct lv2_plugin *lv2_plugin_open(struct lv2_bundle *bundle, const char *uri, char why[LV2_CORE_WHY_MAX]);

/* Open the plugin's binary RTLD_NOW and walk lv2_descriptor to its URI: 0, or -1 with `why`. */
int lv2_plugin_load(struct lv2_plugin *plugin, char why[LV2_CORE_WHY_MAX]);

/* The binary closed, the bundle's reference dropped. NULL is a no-op. */
void lv2_plugin_close(struct lv2_plugin *plugin);

/* An instance of a loaded plugin, nothing instantiated yet: LV2 needs the rate. */
struct lv2_instance *lv2_instance_new(const struct lv2_plugin *plugin);

/* First activation, a rate or a longest block that changed: instantiate with the configured features and the options
 * (rate, min, max), connect every port to the instance's own memory, restore the default state, then LV2 activate and
 * start the worker. The same rate and block as the live instance: LV2 activate and the worker only. 0, or -1 with
 * `why`. */
int lv2_instance_activate(struct lv2_instance *in, double rate, uint32_t min_frames, uint32_t max_frames,
                          char why[LV2_CORE_WHY_MAX]);

/* Stop and join the worker, then LV2 deactivate. The instance is kept for the next activate. */
void lv2_instance_deactivate(struct lv2_instance *in);

/* [control] The re-engage after a steady bypass, called by the host's bypass-off verb holding the audio role, never
 * from process(): the worker stopped and joined, LV2 deactivate, both worker rings emptied, LV2 activate, the worker
 * started again. */
void lv2_instance_reset(struct lv2_instance *in);

/* Deactivate when active, LV2 cleanup, free. */
void lv2_instance_free(struct lv2_instance *in);

/* The audio buffers the plugin's ports are connected to, `max_frames` each, valid from the activate that sized them. */
float *lv2_instance_audio_in(struct lv2_instance *in, uint32_t leg);
float *lv2_instance_audio_out(struct lv2_instance *in, uint32_t leg);

/* [audio] Write the input control `k` (the index in plugin->controls); the bypass and the outputs are not writable. */
void lv2_instance_control_set(struct lv2_instance *in, uint32_t k, float value);

/* [thread-safe] The value of control `k`: an input as last written, the bypass's hold, an output as the last run
 * published it. */
float lv2_instance_control_get(const struct lv2_instance *in, uint32_t k);

/* [audio] The plugin's half of a block: worker responses drained through work_response, then `before` (the caller's
 * control writes, NULL for none), run, end_run, the flush-to-zero bits re-asserted, the latency port and the outputs
 * read. */
void lv2_instance_run(struct lv2_instance *in, uint32_t n, void (*before)(void *ctx), void *ctx);

/* [thread-safe] The latency port's last valid reading in frames (0 without one, or before the first run). */
uint32_t lv2_instance_latency(const struct lv2_instance *in);

/* [thread-safe] The counters. */
void lv2_instance_counters(const struct lv2_instance *in, struct lv2_counters *out);

/* The host's audio-role predicate: urid:map, log:log and schedule_work are classified by it, never by a thread check of
 * the library's own. NULL classifies nothing. */
void lv2_instance_set_role(struct lv2_instance *in, lv2_audio_role_fn is_audio, void *ctx);

/* Block until the worker has serviced every pending request (a test pins the schedule with it). */
void lv2_instance_worker_quiesce(struct lv2_instance *in);

/* Whether the instance runs a worker thread now. */
int lv2_instance_has_worker(const struct lv2_instance *in);

/* [main] The instance's state as LV2 Turtle, lilv's own format, malloc'd for the caller to free. NULL before the first
 * activate (there is no handle to read), or when lilv refuses. */
char *lv2_instance_state_save(struct lv2_instance *in);

/* [main] Hold a state, as lv2_instance_state_save wrote it, for the instance: restored after the default state each time
 * the instance is instantiated and at each activate that follows a load, before LV2 activate. The state is held, not
 * applied, so a load while active takes effect at the activate the host's restart brings. -1 with `why` when the text
 * does not parse; the held state is then unchanged. */
int lv2_instance_state_load(struct lv2_instance *in, const char *text, char why[LV2_CORE_WHY_MAX]);


/*
************************************************************************************************************************
*           END HEADER
************************************************************************************************************************
*/

#endif
