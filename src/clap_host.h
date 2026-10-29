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
#include <omxcontract/omx_resource_hints.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "clap_host_extensions.h"
#include "clap_stage.h"
#include "omx_clap_ext.h"
#include "omx_clap_core_vocabulary.h"


/*
************************************************************************************************************************
*           CONFIGURATION DEFINES
************************************************************************************************************************
*/

/** The version of the structures below, the stage's and the instance's: a field is only ever appended, and each append
 * raises it (2: the instance's tempo and transport, 0.3; 3: the host's published transport and the auxiliary inputs, 0.7;
 * 4: the plugin's clap.meters/1 declarations, 0.7; 5: the stage's releases_dropped, in its tail padding, 0.10; 6: the
 * instance's note ids, beside its stage; 7: the instance's note output; 8: the plugin's clap.voice-info, 0.11; 9: the
 * instance's MPE zones, a lower and an upper, 0.11; 10: what the plugin tells the host's note-ports, note-name and
 * voice-info, 0.12; 11: timers, file descriptors, the thread pool, the tail and the sleep, 0.12; 12:
 * libomx-clap-core.so.1, every audio port described (struct omx_clap_port), up to CLAP_HOST_AUX_INPUTS and
 * CLAP_HOST_AUX_OUTPUTS auxiliary ports of up to CLAP_HOST_PORT_CHANNELS channels, and the configuration's
 * lane_channels; 13: the stage's wide bounce, wide_in, wide_out and wide, carried by omx_clap_run_ports; 14: the
 * plugin's clap.surround and clap.ambisonic, which describe a surround and an ambisonic port, the ports rescan the
 * tick reports, the plugin's clap.configurable-audio-ports and clap.audio-ports-config, which
 * omx_clap_host_request_layout asks, and the stage's count of bound auxiliary output channels; 15: the pass-through
 * placement; 16: the CPU idle hold (#296): the configuration's idle_hold_us, the instance's idle_held,
 * omx_clap_host_idle_hold_set and omx_clap_host_idle_hold; 17: the plugin's resource hints (#323), the instance's
 * resource_hints and resource_hints_why, omx_clap_host_resource_hints; 18: the audio role's named slot (#104), the
 * instance's audio_slot, omx_clap_host_name_slot; 19: the thread pool (#395), the instance's pool_use, pool_priority,
 * pool_held and pool_late, struct omx_clap_thread_pool_info, omx_clap_host_thread_pool_use, omx_clap_host_thread_pool and
 * omx_clap_host_thread_pool_late).
 * configure accepts every ABI from OMX_CLAP_CORE_ABI_OLDEST up to its own, so a consumer of an older header keeps
 * working, and refuses a newer one: a consumer whose inline code reaches an appended field is refused by a library
 * whose instance lacks it. The soname-1 train runs 12 to 14 across the PRs of #92, OLDEST following ABI until the
 * first libomx-clap-core.so.1 release, which freezes it: nothing of the major is released, and the stage, embedded in
 * the instance, grows inside it. A removal or a reorder is a new major of the library. */
#define OMX_CLAP_CORE_ABI               19u
#define OMX_CLAP_CORE_ABI_OLDEST        18u  ///< the oldest header ABI the library accepts: follows OMX_CLAP_CORE_ABI until
                                             ///< the first release of libomx-clap-core.so.1

/** The most meters of clap.meters/1 an instance reads; a plugin declaring more has none in process (docs/meters.md). */
#define OMX_CLAP_METER_COUNT_MAX        16u
/** The most channels one meter may have; a meter with more leaves the plugin with no meters in process. */
#define OMX_CLAP_METER_CHANNELS_MAX     64u
/** clap.meters/1's kinds, as struct omx_clap_meter's `kind` carries them: no unit, a gain reduction in dB (0 none,
 * negative a reduction), a level in dBFS. */
#define OMX_CLAP_METER_KIND_OTHER           0u
#define OMX_CLAP_METER_KIND_GAIN_REDUCTION  1u  ///< a gain reduction in dB, 0 none and negative a reduction
#define OMX_CLAP_METER_KIND_LEVEL           2u  ///< a level in dBFS

/** How many bytes a refusal's hosting code needs, with its NUL. */
#define OMX_CLAP_WHY_MAX                64

#define OMX_CLAP_LAYOUT_DECLARED        1   ///< request_layout: the plugin cannot offer it; its declared layout stands

/** The refusals the core returns, by the hosting code a verdict names them by: each the code data/hosting/ declares, from
 * omx_clap_core_vocabulary.h, its render. A code is never reworded. */
#define CLAP_HOST_CODE_HEADLESS_FAILED      OMX_HOSTING_CLAP_HEADLESS_FAILED
#define CLAP_HOST_CODE_NOTE_INPUT           OMX_HOSTING_CLAP_NOTE_INPUT  ///< the plugin declares a note input the configuration refuses
#define CLAP_HOST_CODE_NO_AUDIO_OUTPUT      OMX_HOSTING_TOPOLOGY_NO_AUDIO_OUTPUT  ///< an audio input or an auxiliary port but no main output: nothing to carry (a plugin with no audio port at all passes the strip through instead, #270)
#define CLAP_HOST_CODE_WIDER_THAN_STRIP     OMX_HOSTING_TOPOLOGY_WIDER_THAN_STRIP  ///< a main port is wider than the lane the caller runs the plugin on
#define CLAP_HOST_CODE_TOO_MANY_PORTS       OMX_HOSTING_TOPOLOGY_TOO_MANY_PORTS  ///< more than the limit of auxiliary ports on one side, or more than one main port on a side
#define CLAP_HOST_CODE_PORT_TOO_WIDE        OMX_HOSTING_TOPOLOGY_PORT_TOO_WIDE  ///< a port of more channels than the limit, or an ambisonic port whose channels are not (order+1)²
#define CLAP_HOST_CODE_PORT_WITHOUT_CHANNELS OMX_HOSTING_TOPOLOGY_PORT_WITHOUT_CHANNELS  ///< a port of zero channels
#define CLAP_HOST_CODE_CHANNEL_MAP_UNKNOWN  OMX_HOSTING_TOPOLOGY_CHANNEL_MAP_UNKNOWN  ///< a surround port names a channel id the host does not know, or refuses the carrier's channel mask
#define CLAP_HOST_CODE_AMBISONIC_CONFIG     OMX_HOSTING_TOPOLOGY_AMBISONIC_CONFIG  ///< an ambisonic port refuses the carrier's ordering or normalisation; raised by a layout request only
#define CLAP_HOST_CODE_CRASHED_LIVE         OMX_HOSTING_STABILITY_CRASHED_LIVE_ON_THIS_RIG  ///< the plugin crashed live on this rig
#define CLAP_HOST_CODE_OUTPUT_NON_FINITE    OMX_HOSTING_OUTPUT_NON_FINITE  ///< the plugin produced non-finite output

/** The roster's refusal: an index below count() whose get_info does not answer. */
#define OMX_CLAP_PARAM_ROW_UNREADABLE   "clap.param-row-unreadable"
/** The widest params->count() the core trusts, under its host-limits name (CLAP_HOST_PARAM_COUNT_MAX). */
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
    uint32_t lane_channels;     ///< the widest main port the caller's lane carries, 2 by default; 0 carries any width
                                ///< (activation refuses a wider main port with CLAP_HOST_CODE_WIDER_THAN_STRIP)
    int32_t idle_hold_us;       ///< ABI 16: what /dev/cpu_dma_latency is held at while an instance is active, in
                                ///< microseconds: CLAP_HOST_IDLE_HOLD_US (10, C1 only) by default, 0 refuses every idle
                                ///< state, OMX_CLAP_IDLE_HOLD_OFF holds nothing (omx_clap_host_idle_hold_set changes it later)
};

/** The CPU idle hold (ABI 16, #296). While any instance is active the library keeps one descriptor on
 * /dev/cpu_dma_latency written with the target, so the cores wait in C1 between cycles instead of a deeper state;
 * the kernel drops the request when the descriptor closes, at the last deactivate or at a crash. The device is held
 * by the process, whichever host it is: omx-clap-host, an engine's in-process tier, a daemon's worker. A device that
 * cannot be opened (its default mode is root's; a udev rule gives the audio group write access) refuses nothing: the
 * instance runs on and the refusal is reported by code. OMX_CLAP_IDLE_HOLD_DEVICE in the environment names another
 * device (a test's regular file). */
#define OMX_CLAP_IDLE_HOLD_OFF          (-1)    ///< the target that holds nothing

/** What omx_clap_host_idle_hold answers. */
struct omx_clap_idle_hold
{
    int32_t target_us;          ///< the target: OMX_CLAP_IDLE_HOLD_OFF, 0 or a latency in microseconds
    int32_t held_us;            ///< what the library holds now: the target while an instance is active and the device is open,
                                ///< else OMX_CLAP_IDLE_HOLD_OFF
    int32_t aggregate_us;       ///< the device's own answer, the kernel's limit across every requester; -1 when it cannot be read
    uint32_t active;            ///< the instances active in this process
    char code[OMX_CLAP_WHY_MAX];///< empty, or CLAP_HOST_CODE_IDLE_HOLD_REFUSED since the device could not be opened for writing
};

/** The most helper threads the process's thread pool makes (ABI 19, #395): a worker given more cores uses this many. */
#define OMX_CLAP_THREAD_POOL_HELPERS_MAX    63u

/** The process's thread pool as it stands (ABI 19, #395), what omx_clap_host_thread_pool fills: docs/thread-pool.md. The
 * counts run from the pool's making and are kept after its end, until the next is made. */
struct omx_clap_thread_pool_info
{
    uint32_t helpers;           ///< the helper threads running, 0 while no pool is made
    uint32_t holders;           ///< the active instances holding the pool
    uint32_t cores;             ///< the cores of the affinity mask when the pool was last asked for
    int32_t priority;           ///< the helpers' SCHED_FIFO priority, 0 the normal class
    char state[CLAP_HOST_LOG_BYTES];  ///< empty, or why the last instance that asked for the pool runs serial
    uint64_t requests;          ///< the requests the pool ran
    uint64_t busy;              ///< the requests run serial because another audio thread held the pool
    uint64_t late;              ///< the requests whose wait for the helpers ran past the block
    uint64_t tasks_audio;       ///< the tasks the requesting audio threads ran themselves
    uint64_t helper_tasks[OMX_CLAP_THREAD_POOL_HELPERS_MAX];    ///< the tasks each helper ran
    uint64_t helper_busy_ns[OMX_CLAP_THREAD_POOL_HELPERS_MAX];  ///< the time each helper spent inside tasks, in nanoseconds
    int32_t helper_core[OMX_CLAP_THREAD_POOL_HELPERS_MAX];      ///< the core each helper is pinned to, -1 past `helpers`
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

/** The plugin's own events about its parameters the instance keeps for the control thread, at most this many between
 * two takes (omx_clap_host_param_events_take); one past them is dropped and counted. */
#define OMX_CLAP_PARAM_OUT_DEPTH        256u

/** What the plugin said of a parameter on its own, in an output event of process() or params.flush */
enum omx_clap_param_event_kind
{
    OMX_CLAP_PARAM_EVENT_VALUE = 0,          ///< CLAP_EVENT_PARAM_VALUE: the plugin moved the parameter to `value`
    OMX_CLAP_PARAM_EVENT_GESTURE_BEGIN = 1,  ///< CLAP_EVENT_PARAM_GESTURE_BEGIN: the user took hold of it
    OMX_CLAP_PARAM_EVENT_GESTURE_END = 2     ///< CLAP_EVENT_PARAM_GESTURE_END: the user let it go
};

/** One event of the plugin about a parameter, as omx_clap_host_param_events_take hands it over. */
struct omx_clap_param_event
{
    uint32_t kind;  ///< an omx_clap_param_event_kind
    clap_id id;  ///< the CLAP parameter id
    double value;  ///< the value, for OMX_CLAP_PARAM_EVENT_VALUE; 0 for a gesture
};

/** The instance's sink for the plugin's output events: a lock-free single-producer, single-consumer ring the audio role
 * (process()) or the control thread (params.flush, never at once) fills and the control thread empties. Nothing in it
 * allocates, locks or calls out. */
struct omx_clap_param_out
{
    struct omx_clap_stage *stage;  ///< the stage whose counters a push still raises
    _Atomic uint32_t head;  ///< consumer: the next event to take
    _Atomic uint32_t tail;  ///< producer: the next slot to fill
    _Atomic uint32_t dropped;  ///< events the ring had no room for
    struct omx_clap_param_event ring[OMX_CLAP_PARAM_OUT_DEPTH];  ///< the events
};

/** What a host record that is no plain value carries: the kind it becomes in the block (a CLAP_EVENT_PARAM_MOD, or a
 * gesture's begin or end) and the plugin's own cookie. Such a record rides the stage's parameter ring like a write, its
 * cookie pointing at its tag; the instance's input event list turns it into its kind when the plugin reads it. */
struct omx_clap_param_tag
{
    uint32_t type;  ///< the CLAP event type the record becomes
    void *cookie;  ///< the cookie get_info returned, which the event carries
};

/** The instance's input event list over the stage's: the stage's own list, and one tag per ring position over two laps
 * of the ring, so a tag is written again only once the block that read it is long over. */
struct omx_clap_param_in
{
    clap_input_events_t stage_in;  ///< the stage's own list, which this one reads through
    struct omx_clap_param_tag tag[2 * CLAP_HOST_PARAM_QUEUE_DEPTH];  ///< the tags, by ring position modulo two laps
};

/** What a parameter's value means, from the plugin's org.openmixer.param-contract/1 (its symbol, unit, scale and step)
 * and its clap.params info (its range, default and flags) as get_info gives them when asked. */
struct omx_clap_param_meaning
{
    clap_id id;  ///< the CLAP parameter id
    char symbol[OMX_CLAP_PARAM_CONTRACT_SYMBOL_SIZE];  ///< the symbol the contract keeps across versions
    uint32_t unit;  ///< an OMX_CLAP_PARAM_UNITS code: omx_clap_param_unit_name names it
    uint32_t scale;  ///< an OMX_CLAP_PARAM_SCALES code: omx_clap_param_scale_name names it
    double step;  ///< the plain-unit quantum the plugin resolves: 0 continuous, 1 stepped
    double min,  ///< the minimum, in the unit
           max,  ///< the maximum, in the unit
           def;  ///< the default, in the unit
    uint32_t flags;  ///< get_info's CLAP_PARAM_* flags
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

struct clap_plugin_meters;

/** One meter the plugin declares through clap.meters/1, as the instance read it at open (docs/meters.md). */
struct omx_clap_meter
{
    clap_id id;  ///< the meter's id, what omx_clap_host_meter_values takes
    char name[CLAP_NAME_SIZE];  ///< the meter's displayable name
    uint32_t kind;  ///< clap.meters/1's kind, one of OMX_CLAP_METER_KIND_*
    uint32_t flags;  ///< clap.meters/1's flags (1: every value of one read comes from the same block)
    uint32_t channel_count;  ///< the values the meter has, 1 to OMX_CLAP_METER_CHANNELS_MAX
    double min_value;  ///< the display range the plugin suggests, in the meter's unit
    double max_value;  ///< the top of that range
};

/** The timers one instance serves at once (clap.timer-support); a register past them is refused. */
#define OMX_CLAP_TIMERS                 16u
/** The file descriptors one instance watches at once (clap.posix-fd-support); a register past them is refused. */
#define OMX_CLAP_FDS                    16u
/** The shortest timer period the tick serves, in milliseconds: a shorter one is served at this period. */
#define OMX_CLAP_TIMER_MIN_MS           1u

/** One timer of clap.timer-support: due at `due_ns` on CLOCK_MONOTONIC, then every `period_ms`. */
struct omx_clap_timer
{
    uint32_t used;          ///< 1 while registered
    clap_id id;             ///< what register_timer answered
    uint32_t period_ms;     ///< the period asked for, at least OMX_CLAP_TIMER_MIN_MS
    uint64_t due_ns;        ///< when the tick calls on_timer next
};

/** One file descriptor of clap.posix-fd-support and the CLAP_POSIX_FD_* events it is watched for. */
struct omx_clap_fd
{
    int fd;                 ///< -1 when the slot is free
    uint32_t flags;         ///< CLAP_POSIX_FD_READ, _WRITE, _ERROR
};

/**
 * The sleep of an idle plugin (CLAP_PROCESS_SLEEP and its kin), kept by the RT. A plugin that answers SLEEP sleeps; one
 * that answers CONTINUE_IF_NOT_QUIET sleeps after a block whose input and output were silent; one that answers TAIL
 * sleeps once its input has been silent for its tail and its output is silent. Whatever it answers, it sleeps only once
 * its output has been silent for quiet_min_frames (OMX_CLAP_SLEEP_QUIET_MS at the activated rate) and no note the host
 * gave it an id for is held or inside its hold after a note off: an instrument whose voice starts silent, or that says it
 * may sleep while a voice is still to sound, is kept awake. That guard covers the notes the host gave an id for, which are the
 * CLAP-dialect ports'; a MIDI-dialect port keeps no note bookkeeping, so notes sent to one are not held awake by it. While asleep, a block whose input is silent and that carries no note and no
 * parameter event is not processed: the lane keeps its silence and the steady time moves on. Anything else wakes it, and
 * so does the plugin's request_process, a queued sysex, and a change of the transport handed to it: its flags (playing,
 * loop), its tempo, or a position that is not the last one moved on by this block. A stage with a bound side chain never sleeps.
 */
struct omx_clap_sleep
{
    uint32_t asleep;                ///< 1 while the plugin is not processed on silence
    uint64_t quiet_frames;          ///< frames of silent input since the last sound, for a TAIL plugin
    uint64_t quiet_out_frames;      ///< frames of silent output since the last sound
    uint32_t quiet_min_frames;      ///< the silent output a sleep needs first; set at activate, a caller may change it
    _Atomic uint32_t wake;          ///< the plugin's request_process, from any thread
    _Atomic uint64_t skipped;       ///< blocks not processed while asleep
    struct omx_clap_transport last; ///< the transport the previous block was handed, awake or asleep
    int have_last;                  ///< 1 once `last` holds a block's transport
    _Atomic uint32_t sleeps;        ///< times the plugin went to sleep
};

/** How long a plugin's output must have been silent before it may sleep, in milliseconds. */
#define OMX_CLAP_SLEEP_QUIET_MS         200u

/** The quiet of a block: every sample of the lane exactly 0. */
static inline int omx_clap_block_silent(const float *l, const float *r, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (l[i] != 0.0f || (r && r[i] != 0.0f))
            return 0;
    return 1;
}


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
    uint32_t channels;          ///< the main output width, main_out.channels
    uint32_t in_channels;       ///< the main input width, main_in.channels: 0 for a source
    struct omx_clap_port main_in,  ///< the main input port: channels 0 is none (a source)
                         main_out; ///< the main output port
    uint32_t aux_outputs;       ///< auxiliary output ports, after the main one
    struct omx_clap_port aux_out[CLAP_HOST_AUX_OUTPUTS];  ///< each auxiliary output port, in the plugin's order
    float *aux_out_bound[CLAP_HOST_AUX_OUTPUTS * CLAP_HOST_PORT_CHANNELS];  ///< the caller's buffers per output channel,
                                ///< port i's from aux_out_base[i]; NULL: the plugin writes its scratch
    uint32_t aux_out_base[CLAP_HOST_AUX_OUTPUTS];  ///< where each auxiliary output's channels start in aux_out_bound
    uint32_t aux_out_frames[CLAP_HOST_AUX_OUTPUTS];  ///< the frames each bound output holds
    uint32_t note_inputs;       ///< 0 or 1; the dialect the host feeds it
    uint32_t note_dialect;  ///< the note dialect the host feeds the plugin
    uint32_t note_dialects;  ///< every dialect the note input's port declared, not just note_dialect
    double rate;  ///< the sample rate the instance is activated at
    uint32_t max_block;  ///< the largest block in frames
    int active;  ///< nonzero while the plugin is active
    float *bounce_map;          ///< mmap: [guard][bounce_blocks x max_block floats][guard]
    size_t bounce_map_len;  ///< the size of the bounce mapping in bytes
    uint32_t bounce_blocks;     ///< the blocks of max_block floats the bounce holds, from the port description
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
    // the host's published transport, set by omx_clap_host_set_transport (NULL: the tempo word alone decides), and the
    // last whole record the audio role read from it, kept for a block whose read does not come whole
    const struct omx_clap_transport_src *transport_src;  ///< the published transport omx_clap_host_run reads, NULL for none
    struct omx_clap_transport transport_rec;  ///< the last whole record read from transport_src

    // the auxiliary inputs: one port each, and the caller's buffers bound to it (NULL: the silence)
    uint32_t aux_inputs;  ///< auxiliary input ports, after the main one
    struct omx_clap_port aux_in[CLAP_HOST_AUX_INPUTS];  ///< each auxiliary input port, in the plugin's order
    float *aux_in_bound[CLAP_HOST_AUX_INPUTS * CLAP_HOST_PORT_CHANNELS];  ///< the caller's buffers per input channel,
                                ///< port i's from aux_in_base[i]; NULL: the silence
    uint32_t aux_in_base[CLAP_HOST_AUX_INPUTS];  ///< where each auxiliary input's channels start in aux_in_bound
    uint32_t aux_in_frames[CLAP_HOST_AUX_INPUTS];  ///< the frames each bound input holds
    struct omx_clap_aux_in aux_list;  ///< the auxiliary inputs' process list

    // the plugin's clap.meters/1 (ABI 4): read once at open and fixed for the instance's life, so any thread reads them
    const struct clap_plugin_meters *meters;  ///< the plugin's clap.meters/1 extension, NULL when it has none read whole
    uint32_t meter_count;  ///< the meters declared, 0 with no extension
    struct omx_clap_meter meter[OMX_CLAP_METER_COUNT_MAX];  ///< the declarations, meter_count of them

    // what the plugin says of its parameters on its own (ABI 0.10): the values it moves and the gestures of its output
    // events, kept for the control thread, and the rescans it asked for
    struct omx_clap_param_out param_out;  ///< the output events' sink, bound at activate
    _Atomic uint32_t params_rescanned;  ///< the CLAP_PARAM_RESCAN_* flags since omx_clap_host_params_rescanned
    // the host's modulations and gestures (ABI 0.10): the input event list that turns their records into their kind
    struct omx_clap_param_in param_in;  ///< the input event list over the stage's, bound at activate

    // the plugin's org.openmixer.param-contract/1 (ABI 0.10): read and checked at open and again after each params.rescan
    // (R3); read by the control thread only
    omx_clap_param_contract_entry_t *contract;  ///< one entry per parameter by params index, NULL when uncontracted
    uint32_t contract_count;  ///< the entries, 0 when uncontracted
    uint32_t contract_version;  ///< the contract's version, 0 when uncontracted
    char contract_why[128];  ///< "" when contracted; "absent" with no extension; else the rule it broke and how

    // the note ids, the MPE zone and the note ends of the stage (ABI 6): bound to it at activate, freed by the plugin's
    // NOTE_END or a hold of OMX_CLAP_NOTE_ID_HOLD_MS; the RT owns them, the control thread resets them holding the role
    struct omx_clap_note_ids note_ids;  ///< fed by omx_clap_host_note_in

    // the plugin's note output (ABI 7): what it sends out on its first note output port inside process(), as MIDI, for
    // one reader (omx_clap_host_notes_out); the warm-up and params.flush on the control thread put nothing in it
    uint32_t note_outputs;  ///< the plugin's note output ports, read at open
    struct omx_clap_note_out note_out;  ///< filled by the instance's output event list inside a processing block

    // the plugin's clap.voice-info (ABI 8): asked at every activate, which the extension requires, and kept until the
    // next one; read by the control thread only
    uint32_t voice_info_read;  ///< 1 while the last activate read a declaration, 0 when the plugin made none
    uint32_t voice_count;  ///< the voices the plugin's patch uses, 1 to voice_capacity; 0 with no declaration
    uint32_t voice_capacity;  ///< the voices the plugin has allocated; 0 with no declaration
    uint64_t voice_flags;  ///< the declaration's CLAP_VOICE_INFO_* flags; 0 with no declaration

    // the MPE zones of the note input (ABI 9): a lower and an upper zone held together, read and written by the RT; the
    // control thread asks for a zone through omx_clap_host_set_mpe
    struct omx_clap_mpe_zones mpe_zones;  ///< fed by omx_clap_host_note_in

    // what the plugin tells the host through its clap.note-ports, clap.note-name and clap.voice-info (ABI 10): set on the
    // main thread by the plugin's call, taken by the control thread
    _Atomic uint32_t note_ports_rescan_flags;  ///< the CLAP_NOTE_PORTS_RESCAN_* flags of the rescans the tick has not seen
    _Atomic uint32_t note_ports_rescanned;  ///< the flags the tick saw since the last omx_clap_host_note_ports_rescanned
    uint32_t note_ports_stale;  ///< 1 after a rescan of every note port: the next activate reads the note input again
    _Atomic uint32_t note_names_changed;  ///< the plugin said its note names changed, until omx_clap_host_note_names_changed
    _Atomic uint32_t voice_info_changed;  ///< the plugin said its voice info changed: the tick asks it again while active

    // what a full host offers besides (ABI 11): timers and file descriptors the tick serves, the thread pool, the tail
    // and the sleep of an idle plugin
    const clap_plugin_timer_support_t *timer_support;  ///< the plugin's on_timer, read at open
    const clap_plugin_posix_fd_support_t *fd_support;  ///< the plugin's on_fd, read at open
    const clap_plugin_thread_pool_t *thread_pool;  ///< the plugin's exec, read at open
    int in_exec;                                   ///< 1 while the host runs the plugin's tasks: a request made inside is refused
    const clap_plugin_tail_t *tail;  ///< the plugin's tail, read at open
    struct omx_clap_timer timers[OMX_CLAP_TIMERS];  ///< the timers the plugin registered, served by the tick
    uint32_t timer_next_id;  ///< the id the next timer takes
    struct omx_clap_fd fds[OMX_CLAP_FDS];  ///< the descriptors the plugin registered, polled by the tick
    _Atomic uint32_t tail_frames;  ///< the plugin's tail, read at activate and after it says it changed; UINT32_MAX infinite
    _Atomic uint32_t tail_changed;  ///< the plugin said its tail changed: the tick reads it again
    struct omx_clap_sleep sleep;  ///< the RT's sleep of a plugin that said it needs no processing

    // the layout extensions (ABI 14): read at open, asked by describe at open for a surround or an ambisonic port; the
    // host's changed of either is a ports rescan, as clap.audio-ports rescan is, and as clap.audio-ports-config's rescan
    // (the configuration list changed) is while the plugin is inactive; the tick reports it and never turns it into a
    // restart
    const clap_plugin_surround_t *surround;  ///< the plugin's clap.surround/4 (or its draft id), NULL for none
    const clap_plugin_ambisonic_t *ambisonic;  ///< the plugin's clap.ambisonic/3 (or its draft id), NULL for none
    _Atomic uint32_t ports_rescanned;  ///< 1 once the tick saw a ports rescan, until omx_clap_host_ports_rescanned

    // the layout requests' extensions (ABI 14): read at open, asked only by omx_clap_host_request_layout, the plugin
    // inactive
    const clap_plugin_configurable_audio_ports_t *configurable_audio_ports;  ///< the plugin's clap.configurable-audio-ports/1
                                                                             ///< (or its draft id), NULL for none
    const clap_plugin_audio_ports_config_t *audio_ports_config;  ///< the plugin's clap.audio-ports-config, NULL for none

    // ABI 15: the pass-through placement (#270)
    uint32_t passthrough;       ///< 1: the plugin declares no audio port at all (a note tool, a controller). The main pair is
                                ///< the host's placement, stereo and named `in (passthrough: host)` / `out (passthrough: host)`:
                                ///< the strip's audio passes through the stage untouched, the plugin is handed no audio and
                                ///< runs for its notes and parameters alone, and its latency reads 0

    // ABI 16: the CPU idle hold (#296)
    uint32_t idle_held;         ///< 1 while this instance counts in the process's idle hold: set by a completed activate, cleared
                                ///< by deactivate, which releases its count

    // ABI 17: the plugin's org.openmixer.resource-hints.draft/0 (#323), omx-contract's RESOURCE_HINTS: read once at create
    // and fixed for the instance's life, so any thread reads them
    omx_resource_hints_t resource_hints;  ///< the hints as the plugin declared them, each member whose bit of `declared` is
                                          ///< clear 0 and unknown; declared 0 when it states none or they were refused
    const char *resource_hints_why;  ///< NULL, or the OMX_HOSTING_RESOURCE_HINTS_ code the declaration was refused by

    // ABI 18: the audio role's named slot (#104), a thread the carrier's own thread names wait-free
    // (omx_clap_host_name_slot)
    const _Atomic(pthread_t) *_Atomic audio_slot;  ///< the carrier's slot, NULL for none: the thread it holds has the role

    // ABI 19: the thread pool (#395): the caller's grant, set while inactive, and what the activate made of it
    uint32_t pool_use;          ///< 1: the instance is granted the process's thread pool (omx_clap_host_thread_pool_use)
    int32_t pool_priority;      ///< the audio thread's SCHED_FIFO priority the helpers run at, 0 the normal class
    uint32_t pool_held;         ///< 1 while the active instance holds the pool: its requests are handed to the helpers
    _Atomic uint32_t pool_late; ///< this instance's requests whose wait for the helpers ran past the block
};


/*
************************************************************************************************************************
*           FUNCTION PROTOTYPES
************************************************************************************************************************
*/

/** The defaults: clamp, scan and warm-up on, note inputs refused, the declared extensions only, the host named for the
 * library. Fills
 * `abi` and `size` too: start from it. Stamps the library's own current sizeof regardless of how large `config`
 * actually is (#301): a caller whose own struct is smaller, because it was compiled against an older header and not
 * rebuilt since, is overrun once that struct has grown past its size. omx_clap_host_config_default_sized is the call
 * that cannot do that; prefer it. */
OMX_CLAP_EXPORT void omx_clap_host_config_default(struct omx_clap_host_config *config);

/** Like omx_clap_host_config_default, but never touches a byte past caller_size (0.15.0, #301): pass sizeof(*config)
 * so the write is bounded to the struct as the CALLER compiled it, not to the library's own, possibly larger, current
 * one. config is filled up to whichever of caller_size and the library's own sizeof is smaller, and `size` is set to
 * that, not to the library's full size. caller_size too short to carry even `version` (the same floor
 * omx_clap_host_configure holds a config to) is refused: config is left untouched, not partly overwritten. */
OMX_CLAP_EXPORT void omx_clap_host_config_default_sized(struct omx_clap_host_config *config, uint32_t caller_size);

/** Set the process's configuration, once, before the first binary is opened. -1: the ABI differs, a binary is open
 * already or a configuration was set. */
OMX_CLAP_EXPORT int omx_clap_host_configure(const struct omx_clap_host_config *config);

/** Change the idle hold's target (ABI 16): off, 0 or a latency in microseconds. Written to the open descriptor at once
 * while held, applied at the next activate while not; off closes the descriptor. A target below off is refused, -1. */
OMX_CLAP_EXPORT int omx_clap_host_idle_hold_set(int32_t target_us);

/** The idle hold as it stands (ABI 16): the target, what is held, the device's aggregate, the active count, the code. */
OMX_CLAP_EXPORT void omx_clap_host_idle_hold(struct omx_clap_idle_hold *out);

/** Grant one instance the process's thread pool, or withdraw it (ABI 19, #395): `use` nonzero grants it, `rt_priority`
 * is the audio thread's SCHED_FIFO priority the helpers run at (1 to 99), 0 when the audio thread runs at the normal
 * class. The grant is the plugin's qualification's to give (docs/thread-pool.md): an instance never granted runs every
 * task serial on the audio thread. Read at the next activate; while the instance is active, or for a priority outside
 * 0..99, -1 and nothing changes; 0 otherwise. */
OMX_CLAP_EXPORT int omx_clap_host_thread_pool_use(struct omx_clap_instance *in, int use, int rt_priority);

/** The process's thread pool as it stands (ABI 19): helpers, holders, why it is serial, and the counts. Any thread. */
OMX_CLAP_EXPORT void omx_clap_host_thread_pool(struct omx_clap_thread_pool_info *out);

/** The instance's requests whose wait for the helpers ran past the block (ABI 19): an xrun-class count. Any thread. */
OMX_CLAP_EXPORT uint32_t omx_clap_host_thread_pool_late(const struct omx_clap_instance *in);

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

/**
 * The plugin object inside the bundle directory `dir`: the one regular `.clap` file directly inside it, else the one
 * `.so`, whose name is the packager's (nebula-cluster.clap/libnebula_cluster.so, #259). Returns the number of `.clap`
 * files directly inside when there is any, else the number of `.so` files (each name counted, a symlink beside its
 * target is two): exactly 1 and the object's path is in `object` (`cap` bytes); 0 (none, an unreadable directory, a
 * path that does not fit `cap`) or more than one, and `reason` says which. omx_clap_host_open given a directory
 * resolves it through this function.
 */
OMX_CLAP_EXPORT int omx_clap_host_bundle_object(const char *dir, char *object, size_t cap, char *reason, size_t reason_size);
/** Create one instance of the plugin `desc` describes, with no layout judged, into `*out`: 0, or -1 on a refusal. */
OMX_CLAP_EXPORT int omx_clap_host_create(struct omx_clap_binary *binary, const clap_plugin_descriptor_t *desc, struct omx_clap_instance **out);

/**
 * Load the .clap at `path`, create the plugin `id` (NULL: the factory's first descriptor), init it and read its
 * extensions, each step judged: the descriptor's feature list is not read (a plugin is admitted by its ports, #257),
 * init must succeed headless with only the declared host extensions offered, the audio ports must declare
 * one main output and at most one main input (none for a source), the two of any widths, every port of 1 to
 * CLAP_HOST_PORT_CHANNELS channels and at most CLAP_HOST_AUX_INPUTS and CLAP_HOST_AUX_OUTPUTS auxiliary ports, and no
 * note input unless the configuration admits one; every port is described in the instance (main_in, main_out, aux_in,
 * aux_out). An auxiliary output writes a scratch buffer until the caller binds its own with
 * omx_clap_host_bind_aux_output; an auxiliary input (a side chain) is fed silence until the caller binds its buffers
 * with omx_clap_host_bind_aux_input. How wide a main port the caller's lane carries is omx_clap_host_activate's to judge.
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
 * the stage, and, where the configuration warms up, warm it up in blocks of `max_block` on this thread holding the audio
 * role and restart the plugin after it; read latency.get() and publish it. The stage is left IDLE: publishing is
 * omx_clap_host_publish or omx_clap_host_arm. -1 with `why` (CLAP_HOST_CODE_WIDER_THAN_STRIP when a main port is wider
 * than the configuration's lane_channels, CLAP_HOST_CODE_HEADLESS_FAILED when activate refuses,
 * CLAP_HOST_CODE_OUTPUT_NON_FINITE when the warm-up did). A carrier whose cycles keep running shuts them out of the
 * instance across activate, deactivate and restart (omx-clap-host's cycle_gate.h): the active flag, the stage and the
 * bounce change under them, and the flag is set before the stage is bound.
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
/** A carrier thread known only once it runs (JACK's thread-init callback), named with no lock and no wait: the thread
 * `*slot` holds is on the audio role from the store that puts it there, whatever the role's thread and held flag say,
 * and its first cycle runs the plugin. The carrier's thread stores itself there (an atomic store, from any thread, at
 * any time); this library only reads it, and never writes the role's own fields from it, so the restart's saved role
 * never races it. The slot outlives the instance or is cleared first (NULL: none, the instance's start). Control
 * thread, before the carrier's cycles start or with them shut out of the instance. */
OMX_CLAP_EXPORT void omx_clap_host_name_slot(struct omx_clap_instance *in, const _Atomic(pthread_t) *slot);
/** Arm the published instance: its stage starts to run the plugin on the next cycle. */
OMX_CLAP_EXPORT void omx_clap_host_arm(struct omx_clap_instance *in);

/** Unpublish: ask the RT to stop and wait, off the RT, polling every `poll_us` for at most `timeout_us`, for it to say
 * STOPPED. 0 when it did, -1 on the timeout (the RT is not calling the stage; the caller must not deactivate). A stage
 * that never ran stops at once. */
OMX_CLAP_EXPORT int omx_clap_host_unpublish(struct omx_clap_instance *in, unsigned poll_us, unsigned timeout_us);

/** Control thread, once no cycle can run anymore: stop_processing when the stage was still running, the audio role
 * released, the stage IDLE. */
OMX_CLAP_EXPORT void omx_clap_host_stop(struct omx_clap_instance *in);

#define OMX_CLAP_RESTART_RESCANNED      1   ///< restart, set_rate, an aux bind: stopped inactive on a ports rescan, not a failure

/** While cycles keep running: stop the stage (taking the audio role when no cycle comes), deactivate, activate again at the
 * same rate and block, arm. The cycles pass the lane through until the plugin is back. CLAP's sequence for a layout
 * change is request_restart, deactivate, the plugin's ports changed (audio-ports rescan, surround or ambisonic changed),
 * the host reading the ports again, activate: when the plugin said its ports changed in that deactivate, the restart does
 * not activate it and returns OMX_CLAP_RESTART_RESCANNED with the instance inactive, its audio role kept and the rescan
 * reported (omx_clap_host_ports_rescanned); the caller reads the description again (omx_clap_host_request_layout, `n` 0
 * allowed) before omx_clap_host_activate and omx_clap_host_arm. 0 once restarted; -1 when it could not (the instance
 * not active, or the activate refused), a failure. The carrier shuts its cycles out of the instance across the call, as
 * for omx_clap_host_activate. */
OMX_CLAP_EXPORT int omx_clap_host_restart(struct omx_clap_instance *in);

/** Deactivate a STOPPED (or never published) instance and release the bounce and the ring; the carrier's cycles are shut
 * out of the instance across the call, as for omx_clap_host_activate. */
OMX_CLAP_EXPORT void omx_clap_host_deactivate(struct omx_clap_instance *in);

/** A rate change, off the RT: the instance must be unpublished; deactivate, activate at the new rate, re-read the latency.
 * 0, or -1 with `why`; OMX_CLAP_RESTART_RESCANNED when the plugin said its ports changed in that deactivate: inactive
 * with the rescan reported, and the caller reads the description again (omx_clap_host_request_layout, `n` 0 allowed)
 * before omx_clap_host_activate at the new rate, as after omx_clap_host_restart. A rescan already pending when the call
 * is made (raised by an earlier activate, not yet reported by the tick) is answered the same way: the call reports it and
 * does not activate. */
OMX_CLAP_EXPORT int omx_clap_host_set_rate(struct omx_clap_instance *in, double rate, uint32_t max_block, char why[OMX_CLAP_WHY_MAX]);

/** Destroy the instance (deactivating first if needed) and drop its binary reference. */
OMX_CLAP_EXPORT void omx_clap_host_close(struct omx_clap_instance *in);

/** The declared latency.get() as last published, in frames; 0 for a pass-through tool, whose latency applies to its notes
 * alone (the audio it does not touch has none). */
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

/** preset-load of a preset omx_clap_host_presets listed, by its location kind, location and load key: a FILE location
 * names its file (`load_key` NULL for a file that holds one preset, the key for one inside a container), a PLUGIN
 * location none (`location` is not read) and its key. -1 when the plugin has no such extension, refuses, or a FILE
 * location has no path. Control thread. */
OMX_CLAP_EXPORT int omx_clap_host_preset_load_key(struct omx_clap_instance *in, uint32_t location_kind, const char *location,
                                                  const char *load_key);

/** The plugin ids one listed preset names at most (CLAP ids only: a preset for another format's plugin names none). */
#define OMX_CLAP_PRESET_PLUGINS         8u
/** The presets one listing hands its callback at most; the rest of the file is not read. */
#define OMX_CLAP_PRESETS_MAX            65536u
/** The preset files one listing reads at most, across every location of every provider. */
#define OMX_CLAP_PRESET_FILES_MAX       65536u
/** How deep a FILE location that is a directory is walked: its files are depth 1. */
#define OMX_CLAP_PRESET_DEPTH           16u

/** One preset a plugin file lists through its preset-discovery factory. Every pointer is valid only for the callback. */
struct omx_clap_preset
{
    const char *provider_id;    ///< the provider that listed it
    const char *name;           ///< its name; for a file holding one preset that gives none, the file's name
    uint32_t location_kind;     ///< CLAP_PRESET_DISCOVERY_LOCATION_FILE or CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN
    const char *location;       ///< the file's path for FILE, NULL for PLUGIN
    const char *load_key;       ///< the key inside a container or a PLUGIN location; NULL for a file holding one preset
    uint32_t flags;             ///< CLAP_PRESET_DISCOVERY_IS_*: its own, else its location's
    const char *description;    ///< "" when it gives none
    uint32_t plugin_count;      ///< the CLAP plugin ids it applies to, at most OMX_CLAP_PRESET_PLUGINS
    const char *plugin_ids[OMX_CLAP_PRESET_PLUGINS];  ///< the first plugin_count are set
};

/** One preset of a listing; return nonzero to stop it. */
typedef int (*omx_clap_preset_fn)(void *ctx, const struct omx_clap_preset *preset);

/** Every preset a plugin file lists through its preset-discovery factory (clap.preset-discovery-factory/2, or its draft
 * id), in order: each provider is created and initialised, and each location it declares is read: a PLUGIN location
 * through get_metadata, a FILE location that is a file the same, and a directory walked in name order (at most
 * OMX_CLAP_PRESET_DEPTH deep, a link to a directory inside it not followed) for its files of a declared filetype. At most
 * OMX_CLAP_PRESETS_MAX presets and OMX_CLAP_PRESET_FILES_MAX files. Returns the presets listed, or -1 when the file
 * has no preset-discovery factory. Control thread. */
OMX_CLAP_EXPORT int omx_clap_host_presets(struct omx_clap_binary *binary, omx_clap_preset_fn fn, void *ctx);

/** The same for a linked entry, or an adapter's (omx_clap_lv2_entry): init once as omx_clap_host_open_entry does, kept
 * for the listing. */
OMX_CLAP_EXPORT int omx_clap_host_entry_presets(const clap_plugin_entry_t *entry, omx_clap_preset_fn fn, void *ctx);

/** clap.track-info, where the configuration offers it: store what the host's get answers from now on and call the
 * plugin's changed. `name` NULL or "" for none, `color` NULL for none; `flags` the CLAP_TRACK_INFO_IS_FOR_* bits, the
 * HAS_ bits are set here. The name is cut at CLAP_NAME_SIZE - 1 bytes. -1 when the configuration does not offer it. */
OMX_CLAP_EXPORT int omx_clap_host_track_info_set(struct omx_clap_instance *in, const char *name, const clap_color_t *color,
                                                 uint64_t flags);

/** Whether the plugin called the host's remote_controls.changed since the last call; reading clears it. */
OMX_CLAP_EXPORT int omx_clap_host_remote_controls_changed(struct omx_clap_instance *in);

/** Offer clap.gui to every plugin opened from now on (0.18.0, #180): a plugin that asks the host for CLAP_EXT_GUI is
 * handed `gui` itself, the caller's own object, besides the declared list; each call's `host->host_data` is the
 * instance. The library draws nothing and links no toolkit: showing the window, and keeping the calls thread-safe as
 * clap.gui requires, is the caller's. Called before the first binary is opened, as the configuration is; NULL offers
 * none, the default. 0, or -1 once a binary was opened. */
OMX_CLAP_EXPORT int omx_clap_host_offer_gui(const clap_host_gui_t *gui);

/** The meters the plugin declared through clap.meters/1, read at open: 0 when it has none, or none read whole. Any thread. */
OMX_CLAP_EXPORT uint32_t omx_clap_host_meter_count(const struct omx_clap_instance *in);

/** The `index`-th meter's declaration into `*meter`: 0, or -1 for an index past omx_clap_host_meter_count. Any thread. */
OMX_CLAP_EXPORT int omx_clap_host_meter_info(const struct omx_clap_instance *in, uint32_t index, struct omx_clap_meter *meter);

/** The latest values of meter `id`, one per channel, into `values`: how many were written, at most `capacity` and the
 * meter's channel count; 0 for an id the plugin did not declare, or a plugin that has not processed since it was
 * activated. Calls the plugin's thread-safe get_values(), so any thread but the audio role reads it: a telemetry
 * thread, at its own rate. The caller keeps the instance open while it reads. */
OMX_CLAP_EXPORT uint32_t omx_clap_host_meter_values(struct omx_clap_instance *in, clap_id id, double *values, uint32_t capacity);

/** The plugin's clap.voice-info, as it answered at the last omx_clap_host_activate: 1 with `*count` the voices its patch
 * uses, `*capacity` the voices it has allocated (1 <= count <= capacity, the ceiling a host prices it at) and `*flags`
 * its CLAP_VOICE_INFO_* flags (CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES: two notes on the same key at once, told
 * apart by note id). 0 when it declares none: no extension, a get that fails, an answer outside 1 <= count <= capacity,
 * or an instance never activated; each output then reads 0, which is never a voice count. Any pointer may be NULL.
 * Control thread. */
OMX_CLAP_EXPORT int omx_clap_host_voice_info(const struct omx_clap_instance *in, uint32_t *count, uint32_t *capacity, uint64_t *flags);

/** The resource hints the plugin declares about itself (org.openmixer.resource-hints.draft/0, omx-contract's
 * RESOURCE_HINTS), as its get_extension answered at create: whole or refused. 1 with `*hints` the declaration, its
 * `declared` the OMX_RESOURCE_HINT_ bits of the hints it states; a member whose bit is clear is unknown, never zero,
 * and reads 0. 0 when it states none (no extension, a get that answers false, or no bit set): `declared` 0. -1 when the
 * declaration is refused: `declared` 0 and `*why` the code, OMX_HOSTING_RESOURCE_HINTS_UNKNOWN_HINT for a bit
 * RESOURCE_HINTS has no hint for, OMX_HOSTING_RESOURCE_HINTS_WRONG_UNIT for a declared flag that is neither 0 nor 1.
 * `*why` is NULL otherwise. Any pointer may be NULL. Any thread, while the instance is open. */
OMX_CLAP_EXPORT int omx_clap_host_resource_hints(const struct omx_clap_instance *in, omx_resource_hints_t *hints, const char **why);

/** The word of hint `index` (bit 1u << index of `declared`), as RESOURCE_HINTS spells it ("footprint-at-rest"); NULL
 * from OMX_RESOURCE_HINT_COUNT on. */
OMX_CLAP_EXPORT const char *omx_clap_resource_hint_word(uint32_t index);

/** The unit of hint `index`'s figure, a word of RESOURCE_UNITS ("bytes", "count", "flag", "ms"); NULL from
 * OMX_RESOURCE_HINT_COUNT on. */
OMX_CLAP_EXPORT const char *omx_clap_resource_hint_unit(uint32_t index);

/** The figure of hint `index` in its unit, a flag 0 or 1: 1 with `*figure` set when `hints` declares it, 0 when it is
 * unknown (`*figure` 0, never a figure), -1 from OMX_RESOURCE_HINT_COUNT on or with no `hints`. `figure` may be NULL. */
OMX_CLAP_EXPORT int omx_clap_resource_hint_figure(const omx_resource_hints_t *hints, uint32_t index, uint64_t *figure);

/** state-context's save (clap.state-context): the plugin's state for `context` (CLAP_STATE_CONTEXT_FOR_PRESET, _DUPLICATE
 * or _PROJECT) into `buf`, as omx_clap_host_state_save does; a plugin without the extension saves its plain state for
 * every context, which is what the extension says it stands for. -1 for another context, or as omx_clap_host_state_save.
 * Control thread. */
OMX_CLAP_EXPORT int omx_clap_host_state_save_context(struct omx_clap_instance *in, uint32_t context, void *buf, size_t cap, size_t *len);

/** state-context's load, the same way: the plain state load for a plugin without the extension. Control thread. */
OMX_CLAP_EXPORT int omx_clap_host_state_load_context(struct omx_clap_instance *in, uint32_t context, const void *buf, size_t len);

/** Tell the plugin how it is rendered (clap.render): CLAP_RENDER_REALTIME, or CLAP_RENDER_OFFLINE before a render that is
 * not bound to the clock (a bounce, a freeze), so it may take its best quality. 0, or -1 when it has no extension,
 * refuses, or has a hard real-time requirement and OFFLINE was asked. Control thread. */
OMX_CLAP_EXPORT int omx_clap_host_set_render(struct omx_clap_instance *in, int32_t mode);

/** The plugin's tail (clap.tail) in frames, as read at the last activate or after it said its tail changed: 0 with no
 * extension, UINT32_MAX for an infinite tail. A plugin that answers CLAP_PROCESS_TAIL is processed for this long after
 * its input goes silent, then sleeps (struct omx_clap_sleep). Any thread. */
OMX_CLAP_EXPORT uint32_t omx_clap_host_tail(const struct omx_clap_instance *in);

/** The note dialects the host's clap.note-ports answers a plugin that asks (supported_dialects): every dialect a note
 * input can be fed in, CLAP, MIDI, MIDI with MPE and MIDI 2.0 (translated from the MIDI 1.0 the host reads), when the
 * configuration admits note inputs; none when it does not. */
#define OMX_CLAP_HOST_NOTE_DIALECTS     (CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE | CLAP_NOTE_DIALECT_MIDI2)

/** The note names the plugin declares through clap.note-name (a drum machine naming its pads), asked now: 0 when it offers
 * none. Control thread. */
OMX_CLAP_EXPORT uint32_t omx_clap_host_note_name_count(struct omx_clap_instance *in);

/** Note name `index` (below omx_clap_host_note_name_count) into `*name`: its text, always terminated, and the port, key
 * and channel it names, each -1 for every one. 0, or -1 for an index the plugin does not answer. Control thread. */
OMX_CLAP_EXPORT int omx_clap_host_note_name(struct omx_clap_instance *in, uint32_t index, clap_note_name_t *name);

/** 1 once after the plugin said its note names changed (the host's clap.note-name), then 0 until it says so again: the
 * names are worth reading again. Control thread. */
OMX_CLAP_EXPORT int omx_clap_host_note_names_changed(struct omx_clap_instance *in);

/** The CLAP_NOTE_PORTS_RESCAN_* flags of every note-ports rescan the plugin made since the last call, as the tick saw
 * them, then none. A rescan of CLAP_NOTE_PORTS_RESCAN_ALL is only the plugin's to make while inactive: made while
 * active, the tick asks for a restart; either way the next omx_clap_host_activate reads the note input again, and
 * refuses with hosting.clap.note-input when the new ports are ones open would have refused. Control thread. */
OMX_CLAP_EXPORT uint32_t omx_clap_host_note_ports_rescanned(struct omx_clap_instance *in);

/** 1 once after the tick saw a ports rescan the plugin made (clap.audio-ports rescan, any flags, clap.surround's or
 * clap.ambisonic's changed, or, while it was inactive, clap.audio-ports-config's rescan: its configuration list
 * changed), then 0 until it makes another: the description held from open, or the configuration list request_layout
 * chose from, may no longer be the plugin's. Nothing is restarted or read again for it: omx_clap_host_request_layout reads it again and clears the
 * report, an activate never does. The warm-up's own deactivate is not reported; a rescan made in a restart's deactivate
 * is (the restart then leaves the instance inactive), and one made inside activate() is. Control thread. */
OMX_CLAP_EXPORT int omx_clap_host_ports_rescanned(struct omx_clap_instance *in);

/**
 * Ask the plugin, inactive, for the carrier's layout: `reqs`, `n` of them, each a port by side and index (the main
 * port is index 0), its channels and its port type, port_details the carrier's channel map for a surround port and its
 * clap_ambisonic_config_t for an ambisonic one. First a surround request's mask is asked of the plugin's clap.surround
 * (is_channel_mask_supported) and an ambisonic request's configuration of its clap.ambisonic (is_config_supported): a
 * refusal, or a map naming an id past CLAP_SURROUND_TSR, is -1 with CLAP_HOST_CODE_CHANNEL_MAP_UNKNOWN or
 * CLAP_HOST_CODE_AMBISONIC_CONFIG, nothing applied and the description read again all the same (below; its own
 * refusal's code wins when that re-read fails). Then clap.configurable-audio-ports (can_apply_configuration,
 * apply_configuration), and when it is not offered, cannot apply the requests or its apply answers false, the
 * clap.audio-ports-config list: a config matches when its main ports are the requests' mains, a main on each side
 * asked and none on a side not asked, of the requests' channels and port types (a config naming no type matches a mono
 * request of one channel or a stereo one of two), whatever auxiliary ports it also declares (a side chain is described
 * after); requests past the main ports are configurable-audio-ports' only. The first match select accepts is selected,
 * a match it refuses passed over. Returns 0 once one applied; OMX_CLAP_LAYOUT_DECLARED when neither extension offers
 * the layout, or select refused every match: the plugin keeps its layout and the carrier judges it. Either way the description is read again, as open reads it (a
 * side's auxiliary bindings kept while its auxiliary ports keep their count and widths, all of that side's dropped
 * otherwise), which answers every ports rescan the plugin made up to then: `n` 0 asks nothing and only reads it again,
 * the answer to a reported rescan (omx_clap_host_ports_rescanned, omx_clap_host_restart). A description open would
 * refuse is -1 with that code, the instance left with no main output: omx_clap_host_activate refuses it
 * (CLAP_HOST_CODE_NO_AUDIO_OUTPUT) and the caller closes it. -1 with nothing asked and `why` empty (no hosting code
 * names it) for no instance, an active one, or `n` above 0 with `reqs` NULL. Control thread, never the RT.
 */
OMX_CLAP_EXPORT int omx_clap_host_request_layout(struct omx_clap_instance *in, const clap_audio_port_configuration_request_t *reqs,
                                                 uint32_t n, char why[OMX_CLAP_WHY_MAX]);

/** The events the plugin pushed about its parameters since the last take, in order, at most `capacity` of them into
 * `events`: a value it moved by itself, and the gestures that bracket the user's hold of one of its controls. The rest
 * waits for the next take. Control thread. */
OMX_CLAP_EXPORT uint32_t omx_clap_host_param_events_take(struct omx_clap_instance *in, struct omx_clap_param_event *events,
                                                         uint32_t capacity);

/** The plugin's parameter events dropped since open, for want of room between two takes: when it grows, a value the
 * plugin moved may have gone unseen, and its parameters are worth reading again. Any thread. */
OMX_CLAP_EXPORT uint32_t omx_clap_host_param_events_dropped(const struct omx_clap_instance *in);

/** Modulate row `id` by `amount` (CLAP_EVENT_PARAM_MOD, in the parameter's own units) around its set value, which stays
 * what it was: queued for the next block like a write. -1 for an id that is no row or is not CLAP_PARAM_IS_MODULATABLE,
 * an inactive instance, or a full ring. Control thread. */
OMX_CLAP_EXPORT int omx_clap_host_param_modulate(struct omx_clap_instance *in, clap_id id, double amount);

/** The host's own gesture on row `id`: `begin` nonzero when a control of the host is taken hold of, 0 when it is let go
 * (CLAP_EVENT_PARAM_GESTURE_BEGIN or _END), queued for the next block in order with the writes between them. -1 as for a
 * write. Control thread. */
OMX_CLAP_EXPORT int omx_clap_host_param_gesture(struct omx_clap_instance *in, clap_id id, int begin);

/** The CLAP_PARAM_RESCAN_* flags of every params.rescan the plugin made since the last call, then none. A rescan of
 * CLAP_PARAM_RESCAN_INFO changes what the rows answer at once (they are read from get_info); a RESCAN_ALL while
 * active makes omx_clap_host_tick ask for a restart. Control thread. */
OMX_CLAP_EXPORT uint32_t omx_clap_host_params_rescanned(struct omx_clap_instance *in);

/** The plugin's own text for `value` of parameter `id` (params.value_to_text) into `out`: 0, or -1 when it has none for
 * it. Control thread. */
OMX_CLAP_EXPORT int omx_clap_host_param_text(struct omx_clap_instance *in, clap_id id, double value, char *out, uint32_t size);

/** The value the plugin reads in `text` for parameter `id` (params.text_to_value): 0 with `*value`, or -1. Control
 * thread. */
OMX_CLAP_EXPORT int omx_clap_host_param_parse(struct omx_clap_instance *in, clap_id id, const char *text, double *value);

/** Whether the plugin's org.openmixer.param-contract/1 holds, as checked at open (rules R1, R2, R4 and R5 of openmixer's
 * contract's rules, see omx_clap_ext.h) and again by the tick after each params.rescan (R3: the contract read again is
 * the same one): 1 with `*version`; 0 for an uncontracted plugin, `*why` "absent" when it offers no contract, else the
 * rule it broke and how (a contract that breaks one rule is not trusted at all, and one a rescan changed is not trusted
 * again for the instance's life). Either pointer may be NULL. Control thread. */
OMX_CLAP_EXPORT int omx_clap_host_param_contract(struct omx_clap_instance *in, uint32_t *version, const char **why);

/** What parameter `id` means: 0 with `*meaning` filled from the contract and from get_info; -1 for an uncontracted
 * plugin or an id clap.params does not list. Control thread. */
OMX_CLAP_EXPORT int omx_clap_host_param_meaning(struct omx_clap_instance *in, clap_id id, struct omx_clap_param_meaning *meaning);

/** The name of an OMX_CLAP_PARAM_UNITS code ("db", "hz", "semitone12TET"), NULL for a code the table does not hold. */
OMX_CLAP_EXPORT const char *omx_clap_param_unit_name(uint32_t unit);

/** The name of an OMX_CLAP_PARAM_SCALES code ("linear", "log", "stepped"), NULL for any other. */
OMX_CLAP_EXPORT const char *omx_clap_param_scale_name(uint32_t scale);

/** `value` as the shortest decimal that reads back to the same double, in ECMAScript's Number.prototype.toString form
 * (-0 written 0): the length written, or -1 for a value that is not finite or does not fit in `size`. */
OMX_CLAP_EXPORT int omx_clap_param_number(double value, char *out, size_t size);

/** The control thread's tick: run on_main_thread if the plugin asked, drain the log, republish a changed latency, take
 * the plugin's parameter, note-port and audio-port rescans and ask its voice info again when it said it changed. A ports
 * rescan (clap.audio-ports rescan, or clap.surround's or clap.ambisonic's changed) is reported through
 * omx_clap_host_ports_rescanned and asks for no restart. Returns nonzero when the plugin asked for a restart (the
 * caller performs it). */
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

/** Control thread, before publish: the host's published transport omx_clap_host_run hands the plugin from now on (NULL:
 * none, and the tempo word decides again). It carries the position, the playing state, the bar and the time signature as
 * well as the tempo, so it supersedes the tempo word while set. The storage must outlive the instance's publication. */
OMX_CLAP_EXPORT void omx_clap_host_set_transport(struct omx_clap_instance *in, const struct omx_clap_transport_src *src);

/**
 * Control thread, while the instance is not running (never published, or unpublished): bind the buffers auxiliary input
 * `port` reads, `bufs[c]` for its channel c, `channels` of them (the port's own count), each `frames` long. `bufs` NULL
 * unbinds it, back to the silence every unbound port reads; a NULL `bufs[c]` leaves that channel on the silence. The
 * buffers are not copied: a block never reads past `frames`, since the binding is refused while the instance is active
 * at a larger block, and omx_clap_host_activate (and so omx_clap_host_set_rate) refuses a larger block while it is
 * bound. The caller keeps them alive until it unbinds or closes the instance, and fills them on the audio role before
 * each block that runs the instance. The binding outlives a deactivate and activate. Returns 0, or -1 when `port` is not
 * an auxiliary input, `channels` is not the port's (an unbind, `bufs` NULL, names none), `frames` is shorter than the
 * active instance's largest block, the instance is running, or the activation below refuses. On an active instance
 * whose plugin has clap.audio-ports-activation, a port that changes between bound and unbound is set active or inactive
 * at once: by set_active holding the audio role when the plugin can activate a port while processing, else by a
 * deactivate and an activate (the stage is then IDLE, to publish again; a refused activate leaves it inactive, -1).
 * When the plugin said its ports changed in that deactivate it is left inactive with the rescan reported and this
 * returns OMX_CLAP_RESTART_RESCANNED: the caller reads the description again (omx_clap_host_request_layout, `n` 0
 * allowed) and activates, as after omx_clap_host_restart.
 */
OMX_CLAP_EXPORT int omx_clap_host_bind_aux_input(struct omx_clap_instance *in, uint32_t port, float *const *bufs,
                                                 uint32_t channels, uint32_t frames);

/**
 * Control thread, while the instance is not running: bind the buffers auxiliary output `port` writes, `bufs[c]` for its
 * channel c, `channels` of them (the port's own count), each `frames` long; the plugin writes them directly while they
 * are bound, and its scratch otherwise. `bufs` NULL unbinds it; a NULL `bufs[c]` leaves that channel on its scratch.
 * The same rules as omx_clap_host_bind_aux_input, the activation of a port included: the caller reads them on the
 * audio role after each block that runs the instance. Returns 0, or -1 when `port` is not an auxiliary output, `channels` is not the port's, `frames` is
 * shorter than the active instance's largest block, or the instance is running; OMX_CLAP_RESTART_RESCANNED as there,
 * a rescan already pending at the call included.
 */
OMX_CLAP_EXPORT int omx_clap_host_bind_aux_output(struct omx_clap_instance *in, uint32_t port, float *const *bufs,
                                                  uint32_t channels, uint32_t frames);


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

/** 1 when `now` is not the transport `was` moved on by one block of `n` frames at `rate`: a flag, a tempo or a meter changed,
 * or the playhead jumped (a locate, a loop wrap) rather than advanced by about the block's length. */
static inline int omx_clap_transport_changed(const struct omx_clap_transport *was, const struct omx_clap_transport *now,
                                             uint32_t n, double rate)
{
    const double dt = rate > 0.0 ? (double)n / rate : 0.0;
    const double tol = dt + 0.001;      // a block's length and a millisecond of the host's rounding

    if (was->flags != now->flags || was->tempo != now->tempo || was->tsig_num != now->tsig_num || was->tsig_denom != now->tsig_denom)
        return 1;
    if (!(now->flags & CLAP_TRANSPORT_IS_PLAYING))
        return (now->flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE) && was->song_pos_seconds != now->song_pos_seconds;
    if (now->flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE)
    {
        const double d = (double)(now->song_pos_seconds - was->song_pos_seconds) / (double)CLAP_SECTIME_FACTOR;

        if (d < 0.0 || d > dt + tol)
            return 1;
    }
    if ((now->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) && (now->flags & CLAP_TRANSPORT_HAS_TEMPO))
    {
        const double d = (double)(now->song_pos_beats - was->song_pos_beats) / (double)CLAP_BEATTIME_FACTOR;

        if (d < 0.0 || d > (dt + tol) * now->tempo / 60.0)
            return 1;
    }
    return 0;
}

/** The audio role: the transport record a block hands the plugin into `rec`, the host's published transport when one is
 * set (read whole, a read that does not come whole keeping the last record read), else the tempo the instance was given,
 * else no flags. */
static inline void omx_clap_host_transport_rec(struct omx_clap_instance *in, struct omx_clap_transport *rec)
{
    if (in->transport_src)
    {
        omx_clap_transport_read(in->transport_src, &in->transport_rec);
        *rec = in->transport_rec;
    }
    else
    {
        const double bpm = in->tempo ? omx_clap_tempo_read(in->tempo) : 0.0;

        memset(rec, 0, sizeof *rec);
        if (bpm > 0.0)
        {
            rec->flags = CLAP_TRANSPORT_HAS_TEMPO;
            rec->tempo = bpm;
        }
    }
}

/** The audio role, one block: the instance's stage over `l` (and `r`, NULL for a mono lane) for `n` frames, the plugin's
 * transport carrying the host's published transport when one is set, else the tempo the instance was given (NULL when
 * the record holds no flags). The published transport is read whole, once per block, and a block whose read does not
 * come whole keeps the last record it read. A change is seen by the next block. The transport record is written only
 * inside the stage's cycle, on a processing block (omx_clap_run_transport); this function itself writes nothing of the
 * instance's stage. */
static inline void omx_clap_host_run(struct omx_clap_instance *in, float *l, float *r, uint32_t n)
{
    struct omx_clap_transport rec;
    struct omx_clap_sleep *z = &in->sleep;
    clap_process_status said;
    int silent, quiet_out, moved;

    omx_clap_host_transport_rec(in, &rec);
    silent = l && omx_clap_block_silent(l, r, n);
    moved = z->have_last && omx_clap_transport_changed(&z->last, &rec, n, in->rate);
    z->last = rec;
    z->have_last = 1;
    if (z->asleep)
    {
        if (silent && !moved && !atomic_exchange_explicit(&z->wake, 0u, memory_order_relaxed) && in->stage.n_notes == 0 &&
            in->stage.n_sysex == 0 &&
            !omx_clap_queue_pending(&in->stage.queue) &&
            atomic_load_explicit(&in->stage.state, memory_order_acquire) == OMX_CLAP_PROCESSING &&
            atomic_load_explicit(&in->stage.h.bypass, memory_order_relaxed) == 0u &&
            atomic_load_explicit(&in->stage.h.rendered_wet, memory_order_relaxed))
        {
            in->stage.steady_time += (int64_t)n;    // the plugin's clock moves on while it sleeps
            atomic_fetch_add_explicit(&z->skipped, 1u, memory_order_relaxed);
            omx_clap_aux_out_silence(&in->stage, n);    // a sleeping plugin writes nothing: its auxiliary outputs are silent
            return;
        }
        z->asleep = 0;
        z->quiet_frames = 0;
        z->quiet_out_frames = 0;
    }
    omx_clap_run_transport_status(&in->stage, l, r, n, &in->transport, &rec, &said);
    if (said < 0)
        return;
    for (uint32_t i = 0; i < in->aux_inputs && i < CLAP_HOST_AUX_INPUTS; i++)
        for (uint32_t c = 0; c < in->aux_in[i].channels; c++)
            if (in->aux_in_bound[in->aux_in_base[i] + c])
                return;     // a side chain the host does not read for silence: never asleep
    if (!silent)
        z->quiet_frames = 0;
    else
        z->quiet_frames += n;
    // the lane after the block is the plugin's output: CONTINUE_IF_NOT_QUIET and TAIL sleep only once that is silent
    // too, so an instrument that sounds on silent input, or a reverb still ringing, is never put to sleep
    quiet_out = omx_clap_block_silent(l, r, n);
    z->quiet_out_frames = quiet_out ? z->quiet_out_frames + n : 0;
    if (z->quiet_out_frames < z->quiet_min_frames || omx_clap_note_ids_live(&in->note_ids, in->stage.steady_time))
        return;
    if (said == CLAP_PROCESS_SLEEP || (said == CLAP_PROCESS_CONTINUE_IF_NOT_QUIET && silent && quiet_out) ||
        (said == CLAP_PROCESS_TAIL && silent && quiet_out && atomic_load_explicit(&in->tail_frames, memory_order_relaxed) != UINT32_MAX &&
         z->quiet_frames >= atomic_load_explicit(&in->tail_frames, memory_order_relaxed)))
    {
        z->asleep = 1;
        atomic_fetch_add_explicit(&z->sleeps, 1u, memory_order_relaxed);
    }
}

/** The audio role, a cycle as a client with separate input and output buffers has it (JACK): the lane is the output pair,
 * an effect's input copied onto it and an instrument's starting silent (omx_clap_io_lane), then omx_clap_host_run on it,
 * the host's transport or tempo handed to the plugin as there. */
static inline void omx_clap_host_run_io(struct omx_clap_instance *in, const float *const *inputs, float *const *outputs, uint32_t n)
{
    omx_clap_io_lane(&in->stage, inputs, outputs, n);
    omx_clap_host_run(in, outputs[0], in->stage.h.n_out == 2 ? outputs[1] : NULL, n);
}

/** The audio role, a cycle with one buffer per main channel (JACK): `ins` one per main input channel (not read for a
 * source), `outs` one per main output channel, main_in.channels and main_out.channels of them. An instance whose main
 * ports fit the two legs runs omx_clap_host_run_io, the two-leg path with its transport and its sleep, a stereo input
 * to a mono output with its right leg from omx_clap_ports_right; a wider one runs the wide path
 * (omx_clap_run_ports_transport), the host's transport or tempo handed to the plugin as there, and never sleeps. */
static inline void omx_clap_host_run_ports(struct omx_clap_instance *in, const float *const *ins, float *const *outs, uint32_t n)
{
    struct omx_clap_transport rec;

    if (!in->stage.wide)
    {
        float *r = in->stage.plugin && outs && n ? omx_clap_ports_right(&in->stage, ins, n) : NULL;

        if (r)
        {
            omx_clap_io_lane(&in->stage, ins, outs, n);
            omx_clap_host_run(in, outs[0], r, n);
            return;
        }
        omx_clap_host_run_io(in, ins, outs, n);
        return;
    }
    omx_clap_host_transport_rec(in, &rec);
    omx_clap_run_ports_transport(&in->stage, ins, outs, n, &in->transport, &rec);
}

/** RT, ahead of the block it belongs to: one MIDI message of the instance's note input at frame `time`, its CLAP notes
 * carrying the instance's note ids and its MPE zone read (omx_clap_note_in_ids). */
static inline void omx_clap_host_note_in(struct omx_clap_instance *in, uint32_t time, const uint8_t *data, size_t size)
{
    omx_clap_note_in_ids(&in->stage, &in->note_ids, &in->mpe_zones, time, data, size);
}

/** RT right after omx_clap_host_run, in the same cycle, or one other thread that is the ring's only reader: take up to
 * `capacity` of the notes and MIDI the plugin sent out on its first note output, oldest first, each as its MIDI message
 * with its frame in its block and since activate (omx_clap_note_out_take). The ring holds OMX_CLAP_NOTE_OUT_DEPTH
 * events; in->note_out.dropped counts those it had no room for and in->note_out.unmapped those with no MIDI form. */
static inline uint32_t omx_clap_host_notes_out(struct omx_clap_instance *in, struct omx_clap_note_out_event *events,
                                               uint32_t capacity)
{
    return omx_clap_note_out_take(&in->note_out, events, capacity);
}

/** Any thread: read the instance's note input as MPE zone `zone` (OMX_CLAP_MPE_LOWER or OMX_CLAP_MPE_UPPER) with
 * `members` member channels, 1 to 15, from the next message on, as the MPE configuration message would; the other zone
 * stays, but yields the channels this one takes. `members` 0 ends that zone alone; OMX_CLAP_MPE_OFF ends both. The zones
 * last, across a deactivate and a reset too, until a configuration message or another call changes them. */
static inline void omx_clap_host_set_mpe(struct omx_clap_instance *in, uint32_t zone, uint32_t members)
{
    omx_clap_mpe_zones_want(&in->mpe_zones, zone, members);
}

#endif
