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
* The guards that surround a run (the bounce, the crossfade, the clamp, the strikes, the warm-up) are not here: they are
* the CLAP body's, hosted_stage.h.
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
#include <lv2/event/event.h>
#include <lv2/log/log.h>
#include <lv2/options/options.h>
#include <lv2/port-groups/port-groups.h>
#include <lv2/state/state.h>
#include <lv2/uri-map/uri-map.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#include "omx_lv2_features.h"
#include "lv2_rtmempool.h"
#include "omx_clap_core_vocabulary.h"

/* The pool's URI as the specification spells it: the header's LV2_RTSAFE_MEMORY_POOL__Pool macro drops the '#'
 * ("…/rtmempoolPool"), and that is what a plugin compiled against it asks for, while a description written by hand
 * says "…/rtmempool#Pool". The host serves both, and the deprecated URI, over the one pool. */
#define LV2_RTMEMPOOL_POOL_SPELLED_URI  LV2_RTSAFE_MEMORY_POOL_PREFIX "Pool"


/*
************************************************************************************************************************
*           CONFIGURATION DEFINES
************************************************************************************************************************
*/

/* bytes a refusal code needs, with its NUL */
#define LV2_CORE_WHY_MAX                96

/* the smallest atom buffer an instance gets, whatever the configuration says: bytes, a multiple of 8 */
#define LV2_CORE_ATOM_BYTES_MIN         1024u

/* the channels one audio port may have, the core's CLAP_HOST_PORT_CHANNELS; one more is refused */
#define LV2_CORE_PORT_CHANNELS          64

/* the auxiliary audio ports a side may have, the core's CLAP_HOST_AUX_INPUTS and CLAP_HOST_AUX_OUTPUTS, the CV ports of
 * that side counted with them (each is one more auxiliary port to the core); one more is refused */
#define LV2_CORE_AUX_PORTS              64

/* the atom inputs that are neither MIDI, the patch input nor the time port (a control port for the plugin's GUI) a plugin
 * may have: each gets a sequence buffer of its own, emptied before every block; one more is refused */
#define LV2_CORE_ATOM_IN_PORTS          16

/* the atom outputs that are neither MIDI nor the patch output (a notify port for the plugin's GUI) a plugin may have: each
 * gets a buffer of its own, offered every block and discarded after it; one more is refused */
#define LV2_CORE_NOTIFY_PORTS           16

/* the longest string or path property value, with its NUL, the patch port carries either way */
#define LV2_CORE_TEXT_MAX               4096u

/* the library the first bundle loads unless the configuration names another */
#define LV2_CORE_LILV_SONAME            "liblilv-0.so.0"

/* every feature URI a provider here serves, NULL-terminated: what lv2_core_provided holds, for a list of the same */
#define LV2_CORE_PROVIDED_INIT \
    { LV2_URID__map, LV2_URID__unmap, LV2_OPTIONS__options, LV2_BUF_SIZE__boundedBlockLength, LV2_WORKER__schedule, \
      LV2_STATE__loadDefaultState, LV2_LOG__log, LV2_RTSAFE_MEMORY_POOL__Pool, LV2_RTMEMPOOL_POOL_SPELLED_URI, LV2_RTSAFE_MEMORY_POOL_DEPRECATED_URI, \
      LV2_STATE__makePath, LV2_STATE__mapPath, LV2_STATE__freePath, LV2_URI_MAP_URI, LV2_EVENT_URI, NULL }

/* the refusals, the verdict's spelling: each the code data/hosting/ declares, from its omx_clap_core_vocabulary.h */
#define LV2_CODE_NO_REALISATION         OMX_HOSTING_NO_REALISATION
#define LV2_CODE_MIDI_IN                OMX_HOSTING_FEATURES_MIDI_IN_FED_EMPTY
#define LV2_CODE_ATOM_IN                OMX_HOSTING_FEATURES_ATOM_IN_UNFED  // a non-MIDI atom input no buffer here can feed
#define LV2_CODE_FEATURES_MISSING       OMX_HOSTING_FEATURES_MISSING
#define LV2_CODE_NO_AUDIO_OUTPUT        OMX_HOSTING_TOPOLOGY_NO_AUDIO_OUTPUT
#define LV2_CODE_TOO_MANY_PORTS         OMX_HOSTING_TOPOLOGY_TOO_MANY_PORTS
#define LV2_CODE_PORT_TOO_WIDE          OMX_HOSTING_TOPOLOGY_PORT_TOO_WIDE
#define LV2_CODE_STATE_UNREADABLE       OMX_HOSTING_STATE_UNREADABLE
#define LV2_CODE_PRESET_NOT_FOUND       OMX_HOSTING_PRESET_NOT_FOUND
#define LV2_CODE_PATH_OUTSIDE           OMX_HOSTING_STATE_PATH_OUTSIDE     // makePath: empty, absolute, or a `..` component
#define LV2_CODE_PATH_SYMLINK           OMX_HOSTING_STATE_PATH_SYMLINK     // makePath: a component is a symbolic link
#define LV2_CODE_PATH_TOO_LONG          OMX_HOSTING_STATE_PATH_TOO_LONG    // makePath: PATH_MAX or more, or a long component
#define LV2_CODE_PATH_ON_AUDIO          OMX_HOSTING_STATE_PATH_ON_AUDIO    // makePath: called on the audio role
#define LV2_CODE_PATH_UNMAKEABLE        OMX_HOSTING_STATE_PATH_UNMAKEABLE  // makePath: a directory could not be made
#define LV2_CODE_NOT_A_COPY             OMX_HOSTING_STATE_NOT_A_COPY       // not a save's copy directory of the root

#define LV2_CODE_PATCH_NOT_WRITABLE     OMX_HOSTING_PATCH_NOT_WRITABLE
#define LV2_CODE_PATCH_TOO_LARGE        OMX_HOSTING_PATCH_TOO_LARGE
#define LV2_CODE_PATCH_BUSY             OMX_HOSTING_PATCH_BUSY

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
    const char *state_root;         // the directory state:makePath makes each instance's directory in, absolute; NULL: the
                                    // feature is not offered, whatever the list says
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
    char *unit;                     // units:unit, the local name of one of LV2's units ("hz", "db"); NULL: none, or another
    uint32_t range_steps;           // pprops:rangeSteps, 0: none
    struct lv2_scale_point *points;
    uint32_t n_points;
};

/* One loaded bundle, reference-counted. */
struct lv2_bundle;

/* the map entry of a mid-side group's side channel (pg:side), which no CLAP surround id names */
#define LV2_CORE_MAP_SIDE               0xfeu

/* What an audio port carries, as its group's type says. */
enum lv2_port_kind
{
    LV2_PORT_UNKNOWN = 0,
    LV2_PORT_MONO,
    LV2_PORT_STEREO,
    LV2_PORT_SURROUND,
    LV2_PORT_AMBISONIC,
};

/* One audio port as the host sees it: a port group's members, or one ungrouped port. */
struct lv2_audio_port
{
    uint32_t n;                                     // channels, 0 for an absent main
    uint32_t port[LV2_CORE_PORT_CHANNELS];          // the plugin's port index of each channel, in map order
    char *name;                                     // the group's label or name, else the port's name
    uint32_t kind;                                  // an lv2_port_kind
    uint8_t map[LV2_CORE_PORT_CHANNELS];            // CLAP surround ids for a surround kind; ACN indices for ambisonic and
                                                    // a mixed-order ambisonic group (kind unknown); all 0 for a port no
                                                    // group type orders
};

/* What a port is connected to, read with the ports: the first of these, in this order, that names its index. */
enum lv2_port_role
{
    LV2_ROLE_NONE = 0,              // nothing the instance connects
    LV2_ROLE_MAIN_IN,               // a main input channel: channel
    LV2_ROLE_MAIN_OUT,              // a main output channel: channel
    LV2_ROLE_PATCH_IN,              // the patch input, when it is not the MIDI input
    LV2_ROLE_PATCH_OUT,             // the patch output, when it is not the MIDI output
    LV2_ROLE_MIDI_IN,
    LV2_ROLE_MIDI_OUT,
    LV2_ROLE_TIME,
    LV2_ROLE_NOTIFY,                // notify_out[leg]
    LV2_ROLE_ATOM_IN,               // atom_in[leg]
    LV2_ROLE_EVENT_IN,              // event_in[leg]
    LV2_ROLE_EVENT_OUT,             // event_out[leg]
    LV2_ROLE_AUX_IN,                // aux_in[leg], its channel
    LV2_ROLE_AUX_OUT,               // aux_out[leg], its channel
    LV2_ROLE_CV_IN,                 // cv_in[leg]
    LV2_ROLE_CV_OUT,                // cv_out[leg]
    LV2_ROLE_LATENCY,
    LV2_ROLE_CONTROL,               // controls[leg]
};

/* One port's role, by its index. */
struct lv2_port_bind
{
    uint16_t role;                  // an lv2_port_role
    uint16_t channel;
    uint32_t leg;
};

/* One plugin read from its bundle: its ports and required features, read before its binary is opened. */
struct lv2_plugin
{
    struct lv2_bundle *bundle;
    char *uri;
    char *bundle_path;              // the directory, with its trailing '/'
    uint32_t n_ports;
    uint32_t legs;                  // the main output's channels, main_out.n (the input's for a pass-through tool)
    uint32_t in_legs;               // the main input's channels, main_in.n: 0 for a source (an instrument or a generator)
    struct lv2_audio_port main_in, main_out;        // the main port of each side; n 0: none
    uint32_t n_aux_in, n_aux_out;   // the auxiliary ports of each side, after the main one: side chains, the other groups,
                                    // ungrouped ports outside the main one
    struct lv2_audio_port *aux_in, *aux_out;        // LV2_CORE_AUX_PORTS each, the first n_aux_in and n_aux_out used
    uint32_t n_cv_in, n_cv_out;     // the CV inputs and outputs (lv2:CVPort), in port index order: one mono CLAP audio
                                    // port each, counted with the side's auxiliary ports against LV2_CORE_AUX_PORTS
    uint32_t cv_in[LV2_CORE_AUX_PORTS], cv_out[LV2_CORE_AUX_PORTS];          // the plugin's port index of each
    char *cv_in_name[LV2_CORE_AUX_PORTS], *cv_out_name[LV2_CORE_AUX_PORTS];  // lv2:name, else lv2:symbol
    int32_t latency_port;           // -1: none
    float latency_def;              // where its buffer starts: lv2:default, else lv2:minimum, else 0, as every control's
    int32_t midi_in_port;           // the one MIDI input port (an atom:Sequence or an ev:EventPort supporting
                                    // midi:MidiEvent, or an llext:MidiPort), -1: none
    int midi_time;                  // the MIDI input also supports time:Position: the transport is forged into it
    int32_t midi_out_port;          // the one MIDI output port, -1: none
    int midi_in_event;              // the MIDI input is an ev:EventPort: an LV2_Event_Buffer, not an atom sequence
    int midi_out_event;             // the MIDI output is an ev:EventPort
    int midi_in_ll;                 // the MIDI input is an llext:MidiPort (ll-plugins' MIDI port, zynadd's): an LV2_MIDI
                                    // (lv2_ll_midi.h), each event a double timestamp, a size_t size and the bytes
    int midi_out_ll;                // the MIDI output is an llext:MidiPort
    uint32_t n_event_in;            // the ev:EventPort MIDI inputs after the MIDI input: fed an empty buffer
    uint32_t event_in[LV2_CORE_ATOM_IN_PORTS];
    uint32_t n_event_out;           // the ev:EventPort MIDI outputs after the MIDI output: offered a buffer, never read
    uint32_t event_out[LV2_CORE_NOTIFY_PORTS];
    int32_t time_port;              // -1: none; the first atom input that takes time:Position, no MIDI and no patch:Message (§6.6)
    struct lv2_control *controls;   // every control port but the latency port
    uint32_t n_controls;
    int32_t patch_in, patch_out;    // the atom ports carrying patch:Message, -1: none
    int patch_time;                 // the patch input also supports time:Position: the transport is forged into it
    int passthrough;                // no audio output: hosted with the strip's audio passing through it untouched, the
                                    // host's placement (a MIDI tool, or an analyser with audio inputs); legs is then
                                    // the input count, 0 for a tool with no audio at all
    int patch_in_shared;            // the patch input IS the MIDI input: one sequence both writers fill, in frame order
    int patch_out_shared;           // the patch output IS the MIDI output: one sequence both readers read
    uint32_t atom_min;              // the largest rsz:minimumSize of those ports, 0: none
    struct lv2_patch *patches;      // the numeric patch:writable properties, in their URIs' order
    uint32_t n_patches;
    struct lv2_patch_text *texts;   // the string and path patch:writable properties, in their URIs' order
    uint32_t n_texts;
    uint32_t n_atom_in;             // the atom inputs that are neither MIDI, the patch input nor the time port: fed empty
    uint32_t atom_in[LV2_CORE_ATOM_IN_PORTS];
    uint32_t atom_in_min;           // the largest rsz:minimumSize of those inputs, 0: none
    uint32_t n_notify;              // the atom outputs that are neither MIDI nor the patch output, discarded
    uint32_t notify_out[LV2_CORE_NOTIFY_PORTS];
    uint32_t notify_min;            // the largest rsz:minimumSize of those outputs, 0: none
    struct lv2_port_bind *roles;    // n_ports, by port index: what the instance connects each to
    char **required;                // lv2:requiredFeature, NULL-terminated
    int safe_restore;               // state:threadSafeRestore among its features: restore() may run beside run()
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

/* A numeric lv2:Parameter the plugin lists as patch:writable: a CLAP parameter with the id `id`, above the port
 * indices. The value type is the atom type the plugin's property takes. */
enum lv2_patch_type
{
    LV2_PATCH_FLOAT = 0,            // atom:Float
    LV2_PATCH_DOUBLE = 1,           // atom:Double
    LV2_PATCH_INT = 2,              // atom:Int
    LV2_PATCH_LONG = 3,             // atom:Long
    LV2_PATCH_BOOL = 4,             // atom:Bool
};

struct lv2_patch
{
    uint32_t id;                    // the CLAP id: lv2_plugin.n_ports plus the property's index in the URIs' order
    char *uri;                      // the property's URI, as the patch messages name it
    char *symbol;                   // lv2:symbol, else the URI's fragment or last segment, as a C identifier
    char *name;                     // rdfs:label, else the symbol
    enum lv2_patch_type type;
    float min, max, def;            // lv2:minimum, lv2:maximum, lv2:default; absent bounds are -1e9 and 1e9
    uint32_t props;                 // LV2_PROP_INTEGER and LV2_PROP_TOGGLED
};

/* A string or path lv2:Parameter the plugin lists as patch:writable: written and read as text through the patch port. */
struct lv2_patch_text
{
    char *uri;                      // the property's URI, as the patch messages name it
    char *name;                     // rdfs:label, else the URI
    int is_path;                    // 1: atom:Path, 0: atom:String
};

/* What the instance counts, read relaxed. */
struct lv2_counters
{
    uint32_t schedule_refused;      // schedule_work on a full request ring (NO_SPACE)
    uint32_t responses_refused;     // respond on a full response ring (NO_SPACE)
    uint32_t respond_strikes;       // respond() called outside work(): refused
    uint32_t map_on_audio;          // urid:map called on the audio role: a thread violation
    uint32_t schedule_off_audio;    // schedule_work called off the audio role: a thread violation
    uint32_t log_on_audio;          // log:log called on the audio role (the sink writes nothing either way)
    uint32_t patch_dropped;         // patch events of a block that did not fit the atom buffer, and text values the plugin
                                    // reported past LV2_CORE_TEXT_MAX
    uint32_t midi_in_dropped;       // a MIDI input event with no room left in the input buffer: dropped, never waited for
    uint32_t make_path_refused;     // state:makePath answered NULL: a path outside the instance's directory, a call on the
                                    // audio role, or a directory that could not be made
};

/* What lv2_instance_midi_out_each passes for each event the plugin wrote: its frame, as written, and the bytes. */
typedef void (*lv2_midi_out_fn)(void *ctx, int64_t frame, const uint8_t *data, uint32_t size);


/*
************************************************************************************************************************
*           FUNCTION PROTOTYPES
************************************************************************************************************************
*/

/* Once per process: 0, or -1 with `why`. A list naming a feature no provider serves, a ring size that is not a power
 * of two >= 16, or a poll of 0 is refused; so is a state root that is not absolute or cannot be made
 * (lv2.configure.state-root), which is made here and taken by its real path. A second call is refused unless it says
 * exactly what the first said. */
int lv2_core_configure(const struct lv2_core_config *config, char why[LV2_CORE_WHY_MAX]);

/* The feature options of the plugins opened and the instances made from now on, NULL being the defaults: 0, or -1 with
 * `why` (lv2.configure.options) when they do not check, the options then unchanged. Within the configured list: a
 * feature the list does not name is never offered, whatever the options say. The capture buffer and the sink they name
 * must outlive every instance made under them. */
int lv2_core_feature_options_set(const struct omx_lv2_feature_options *o, char why[LV2_CORE_WHY_MAX]);

/* The feature options now. */
const struct omx_lv2_feature_options *lv2_core_feature_options(void);

/* The configuration taken, or NULL before lv2_core_configure. */
const struct lv2_core_config *lv2_core_config(void);

/* Whether lilv is mapped into the process by this library: 0 until the first bundle is loaded. */
int lv2_core_lilv_loaded(void);

/* Every feature URI a provider here serves, NULL-terminated. */
extern const char *const lv2_core_provided[];

/* The bundle at `path` (an absolute directory, with or without its trailing '/'), loaded on its first reference and kept
 * loaded for the life of the process (lv2_bundle_unref drops a reference, not the bundle), lilv
 * loaded on the first bundle: all or nothing, and a failed load does not latch. NULL with `why` set when lilv cannot be
 * loaded, the library is not configured, or the bundle holds neither a plugin nor a preset. A bundle of presets only
 * holds no plugin (its count is 0): its presets join the world, the presets of the plugins they apply to (#320). */
struct lv2_bundle *lv2_bundle_ref(const char *path, char why[LV2_CORE_WHY_MAX]);
void lv2_bundle_unref(struct lv2_bundle *bundle);

/* The plugins the bundle holds (its own, never another bundle's), with their URI and name. */
uint32_t lv2_bundle_count(const struct lv2_bundle *bundle);
const char *lv2_bundle_uri(const struct lv2_bundle *bundle, uint32_t index);
const char *lv2_bundle_name(const struct lv2_bundle *bundle, uint32_t index);

/* one preset of a bundle: the plugin it applies to, its URI (the load key clap.preset-load takes) and its label; return
 * nonzero to stop the listing */
typedef int (*lv2_preset_fn)(void *ctx, const char *plugin_uri, const char *preset_uri, const char *label);

/* [main] Every pset:Preset of the bundle that applies to one of its plugins, plugin by plugin in the bundle's order:
 * the presets listed, or -1 for no bundle. */
int lv2_bundle_presets(const struct lv2_bundle *bundle, lv2_preset_fn fn, void *ctx);

/* The field dump (omx-clap-scan --fields): one predicate the dump asks a plugin and its ports about, by its absolute URI;
 * by_label: a resource it names is passed by its label, not its URI (a port group) */
struct lv2_field_ask
{
    const char *uri;
    int by_label;
};

/* how a value is passed: a URI, a number literal in %.17g, any other text (a literal, a label, a scale point) */
enum lv2_field_kind
{
    LV2_FIELD_URI,
    LV2_FIELD_NUMBER,
    LV2_FIELD_TEXT,
};

/* one value: port -1 for the plugin, else the port's lv2:index; ask the index into the asks */
typedef void (*lv2_field_fn)(void *ctx, int32_t port, uint32_t ask, enum lv2_field_kind kind, const char *value);

/* [main] Every value the plugin `uri` of `bundle` states for each ask, then every value each of its ports states, in
 * index order: read from the world alone, no binary opened and nothing instantiated, whatever the adapter would refuse.
 * A resource that is no URI (a blank node) is passed by its label (lv2:name, rdfs:label or foaf:name) and left out when
 * it has none; lv2:scalePoint as "<value> <label>". 0, or -1 with `why` when the bundle holds no such plugin. */
int lv2_bundle_fields(struct lv2_bundle *bundle, const char *uri, const struct lv2_field_ask *asks, uint32_t count,
                      lv2_field_fn fn, void *ctx, char why[LV2_CORE_WHY_MAX]);

/* Read the plugin `uri` of `bundle`: its ports and its required features, refused with the verdict's code before any
 * binary is opened, including, under strict feature options, a required feature the configured list and the options do
 * not offer between them. Takes a reference on the bundle. */
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

/* Whether an activate at this rate and these block lengths may keep the instance as it runs: 1 when it is still LV2
 * active (no lv2_instance_deactivate since its last activate), at the rate and the block lengths that activate told the
 * plugin (a shorter block told through options:interface included), with no state load
 * held for the next activate. The caller then skips lv2_instance_activate: no LV2 deactivate or activate, the worker left
 * running, the plugin's history whole. 0 otherwise: lv2_instance_activate, which deactivates first. */
int lv2_instance_keep(const struct lv2_instance *in, double rate, uint32_t min_frames, uint32_t max_frames);

/* Stop and join the worker, then LV2 deactivate. The instance is kept for the next activate. */
void lv2_instance_deactivate(struct lv2_instance *in);

/* [control] The re-engage after a steady bypass, called by the host's bypass-off verb holding the audio role, never
 * from process(): the worker stopped and joined, LV2 deactivate, both worker rings emptied, LV2 activate, the worker
 * started again. 0, or -1 when the worker cannot start again: the instance is then LV2 deactivated, as a failed
 * activate leaves it, until the next activate. */
int lv2_instance_reset(struct lv2_instance *in);

/* Deactivate when active, LV2 cleanup, free. */
void lv2_instance_free(struct lv2_instance *in);

/* The audio buffers the plugin's ports are connected to, `max_frames` each, valid from the activate that sized them. */
float *lv2_instance_audio_in(struct lv2_instance *in, uint32_t c);   // c < main_in.n
float *lv2_instance_audio_out(struct lv2_instance *in, uint32_t c);  // c < main_out.n

/* The buffer channel `c` of auxiliary port `port` (in plugin->aux_in and aux_out order) is connected to, as above. */
float *lv2_instance_aux_in(struct lv2_instance *in, uint32_t port, uint32_t c);
float *lv2_instance_aux_out(struct lv2_instance *in, uint32_t port, uint32_t c);

/* The buffers CV input `k` and CV output `k` (in plugin->cv_in and cv_out order) are connected to, `max_frames` each, as
 * the audio buffers are: the host writes an input before the run and reads an output after it. */
float *lv2_instance_cv_in(struct lv2_instance *in, uint32_t k);
float *lv2_instance_cv_out(struct lv2_instance *in, uint32_t k);

/* [audio] Write the input control `k` (the index in plugin->controls); the bypass and the outputs are not writable. */
void lv2_instance_control_set(struct lv2_instance *in, uint32_t k, float value);

/* [thread-safe] The value of control `k`: an input as last written, the bypass's hold, an output as the last run
 * published it. */
float lv2_instance_control_get(const struct lv2_instance *in, uint32_t k);

/* The transport a block hands a plugin's time:Position input, the host's own fields as LV2 names them. `frame` and
 * `playing` (time:speed, 1 or 0) are always present; each other field is present only when its LV2_TIME_HAS_* bit is
 * set in `has`. */
#define LV2_TIME_HAS_BPM                (1u << 0)   // beatsPerMinute
#define LV2_TIME_HAS_BEATS              (1u << 1)   // bar, barBeat
#define LV2_TIME_HAS_METER              (1u << 2)   // beatUnit, beatsPerBar
struct lv2_time
{
    int64_t frame;                  // time:frame: the frame at the start of the block
    int playing;                    // time:speed: 1 when playing, 0 otherwise
    uint32_t has;                   // LV2_TIME_HAS_* bits
    double bpm;                     // time:beatsPerMinute
    int64_t bar;                    // time:bar
    double bar_beat;                // time:barBeat, in beatUnit notes from the bar's start
    uint32_t beat_unit;             // time:beatUnit
    uint32_t beats_per_bar;         // time:beatsPerBar
};

/* [audio] The time:Position of the block about to run, forged into the plugin's time port (NULL: no transport, and the
 * port reads an empty sequence), and into its MIDI input when that supports time:Position too, there before the block's
 * MIDI messages (call it before lv2_instance_midi_in_add). An object is forged at frame 0 of the sequence on every block
 * while the transport plays, so the plugin reads the frame of each block; while stopped only when the transport differs
 * from the last block's (its frame aside). Does nothing for a plugin with neither. */
void lv2_instance_time(struct lv2_instance *in, const struct lv2_time *t);

/* [audio] The plugin's half of a block: worker responses drained through work_response, then `before` (the caller's
 * control writes, NULL for none), run, end_run, the flush-to-zero bits re-asserted, the latency port and the outputs
 * read. */
void lv2_instance_run(struct lv2_instance *in, uint32_t n, void (*before)(void *ctx), void *ctx);

/* [audio] One MIDI message for the next run, at `frame` of its block: `size` bytes at `data`. Between runs, on the audio
 * role: the input is cleared when the instance has run, so a block's messages are added before its run. 0, or -1 when the
 * plugin has no MIDI input, `size` is 0, or the buffer has no room left: that is counted in midi_in_dropped, never waited
 * for. */
int lv2_instance_midi_in_add(struct lv2_instance *in, uint32_t frame, const uint8_t *data, uint32_t size);

/* [audio] Each event the plugin wrote to its MIDI output in the last run, in order, to fn. A sequence in beats, and an
 * event whose type is not midi:MidiEvent, is not passed. The frames are the plugin's, not clamped. */
void lv2_instance_midi_out_each(const struct lv2_instance *in, lv2_midi_out_fn fn, void *ctx);

/* [thread-safe] The latency port's last valid reading in frames (0 without one, or before the first run). */
uint32_t lv2_instance_latency(const struct lv2_instance *in);

/* [thread-safe] The counters. */
void lv2_instance_counters(const struct lv2_instance *in, struct lv2_counters *out);

/* The host's audio-role predicate: urid:map, log:log and schedule_work are classified by it, never by a thread check of
 * the library's own. NULL classifies nothing. */
void lv2_instance_set_role(struct lv2_instance *in, lv2_audio_role_fn is_audio, void *ctx);

/* [audio] The value of patch parameter `j` (the index in plugin->patches) for the next block's patch:Set. The forged
 * event is written before the block's run; the last value written before a block is the one it carries. */
void lv2_instance_patch_write(struct lv2_instance *in, uint32_t j, float value);

/* [thread-safe] The value of patch parameter `j`: as last written, or as the last run's output set it. */
float lv2_instance_patch_get(const struct lv2_instance *in, uint32_t j);

/* [audio] Whether the last run's output carried a patch:Set or patch:Put for patch parameter `j`; if so, *value is the
 * last such value. A run clears every flag first. */
int lv2_instance_patch_changed(const struct lv2_instance *in, uint32_t j, float *value);

/* [thread-safe] The code of the last state:makePath request the instance answered NULL, "" when none was. */
const char *lv2_instance_make_path_why(const struct lv2_instance *in);

/* The directory a state save of the instance copies its files to, "" without a state root. */
const char *lv2_instance_saved_dir(const struct lv2_instance *in);

/* Remove a save's copy directory, `<state_root>/lv2-<pid>-<n>.saved`, and everything in it, links removed as links: 0,
 * or -1 with `why` (LV2_CODE_NOT_A_COPY) for any other path, a symbolic link, or no state root. */
int lv2_saved_drop(const char *saved_dir, char why[LV2_CORE_WHY_MAX]);

/* The longest value text property values may have, without the NUL: LV2_CORE_TEXT_MAX - 1, or less when the patch
 * input's buffer cannot carry it beside its message; 0 for a plugin with no patch input. */
uint32_t lv2_plugin_text_limit(const struct lv2_plugin *p);

/* [main] Write text property `j` (the index in plugin->texts): carried to the plugin as a patch:Set at frame 0 of the
 * next block that runs, and again to a handle made by a later activate. 0, or -1 with `why`: LV2_CODE_PATCH_NOT_WRITABLE
 * for no such property, LV2_CODE_PATCH_TOO_LARGE for a value past lv2_plugin_text_limit, LV2_CODE_PATCH_BUSY while the
 * previous write waits for its block. */
int lv2_instance_text_set(struct lv2_instance *in, uint32_t j, const char *value, char why[LV2_CORE_WHY_MAX]);

/* [main] Text property `j` as the plugin last reported it in a patch:Set or patch:Put, else as the host last wrote
 * it, else empty: its length without the NUL, and as much of it as fits `size` (NUL-terminated) in `buf`. */
uint32_t lv2_instance_text_get(const struct lv2_instance *in, uint32_t j, char *buf, uint32_t size);

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

/* [main] A state restored into the running instance at once, beside the audio role's run(), for a plugin that declares
 * state:threadSafeRestore: its restore() gets the worker's schedule and finishes through it, so the new state reaches
 * the audio a block or two later and no restart is needed. The state is held as a load holds it, for a later
 * instantiation. 0, or -1 with `why` (hosting.state.unreadable) when the plugin does not declare the feature, has no
 * worker, the instance is not active, or the text does not parse: the caller then holds it for the next activate. */
int lv2_instance_state_restore_live(struct lv2_instance *in, const char *text, char why[LV2_CORE_WHY_MAX]);

/* The block lengths told to the running plugin through its options:interface since it was made: a shorter block that
 * fits its buffers is told that way instead of a new instantiation. */
uint32_t lv2_instance_options_told(const struct lv2_instance *in);

/* [main] Hold the state of the preset `uri` (a pset:Preset that lv2:appliesTo this plugin) as a load does: 0, or -1 with
 * `why` (hosting.preset.not-found) when the bundle holds no such preset for this plugin. */
int lv2_instance_preset_load(struct lv2_instance *in, const char *uri, char why[LV2_CORE_WHY_MAX]);


/*
************************************************************************************************************************
*           END HEADER
************************************************************************************************************************
*/

#endif
