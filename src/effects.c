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
************************************************************************************************************************
*/


/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <jack/jack.h>
#include <jack/midiport.h>
#include <jack/thread.h>
#include <clap/ext/draft/gain-adjustment-metering.h>
#include <clap/factory/preset-discovery.h>

#include "effects.h"
#include "clap_host.h"
#include "clap_chain.h"
#include "clap_meters.h"
#include "cycle_gate.h"
#include "hosted_stage_limits.h"
#include "host_transport.h"
#include "host_verbs.h"
#include "layout_pin.h"
#include "omx_clap_core_vocabulary.h"
#include "lv2_locate.h"
#include "window.h"
#include "xrun_report.h"
#include "omx_msgring.h"
#include "preset_file.h"
#include "omx_clap_lv2.h"
#include "host-dispatch.h"
#include "socket.h"


/*
************************************************************************************************************************
*           LOCAL DEFINES
************************************************************************************************************************
*/

#define REMOVE_ALL              (-1)
#define URI_SCHEME              "clap:"
#define LV2_URI_SCHEME          "lv2:"
#define BYPASS_PORT_SYMBOL      ":bypass"
#define MIDI_PORT_NAME          "midi_in"
/* the bytes of the ring the note verbs fill for the audio thread: 512 messages of up to three bytes, a header each */
#define NOTE_RING_BYTES         4096u
#define MIDI_OUT_PORT_NAME      "midi_out"
// the note output events one take reads off the library's ring on the audio thread
#define NOTES_OUT_CHUNK         64u
#define STATE_FILE_SUFFIX       ".clapstate"
#define INSTANCE_IS_VALID(id)   ((id) >= 0 && (id) < MAX_INSTANCES)
#define REQUESTED_CLIENT_NAME_BUF_SIZE  256

// a write waits for the next cycle this long before the control thread delivers it itself
#define QUEUE_SETTLE_US         50000
#define RELAYOUT_UNCHANGED_MAX  8       // relayouts in a row with the main layout unchanged before the reports go unanswered

// the chains plugin-hostd's chain line declares, each run in its head's JACK client (#104, docs/chain.md); plugin-hostd
// gives every chain a worker of its own, so a host runs one, and holds this many
#define CHAINS_MAX              64

// the output the gain adjustment of clap.gain-adjustment-metering answers to
#define GAIN_ADJUSTMENT_SYMBOL  "gain_adjustment_metering"
#define METER_CHANNELS_MAX      64
#define OUTPUT_SYMBOL_SIZE      (CLAP_NAME_SIZE + 16)

// the LV2 adapter's numbers: its worker's poll and ring are the hosted stage's declared ones (data/hosting.json), the
// same the console's in-process adapter is handed; the smallest atom buffer (0: the adapter's own); the latency hold
#define LV2_ATOM_BUFFER_BYTES   0u


/*
************************************************************************************************************************
*           LOCAL DATA TYPES
************************************************************************************************************************
*/

/* one value a plugin reports, answered to by its symbol: a channel of a clap.meters/1 meter, or the gain adjustment of
 * clap.gain-adjustment-metering/0 */
typedef struct OUTPUT_T {
    char symbol[OUTPUT_SYMBOL_SIZE];
    char name[CLAP_NAME_SIZE];  // the meter's, for a refusal to name
    int gain_adjustment;        // the standard value, not a meter
    int param;                  // a read-only parameter, an LV2 output control port: `meter` is its id
    clap_id meter;
    uint32_t channel;
    uint32_t channels;          // the meter's: what get_values fills
    int monitored;
    int notified;               // a value went out since monitor_output
    float value;                // the last value that went out
} output_t;

/* an LV2 plugin's control port, as param_set, param_get and monitor_output name it: its lv2:symbol, its CLAP id (the port
 * index), its flags, and, for the plugin's own bypass, the value that keeps it processing */
typedef struct LV2_PARAM_T {
    const char *symbol;         // the adapter's, alive as long as the plugin
    clap_id id;
    uint32_t flags;
    double hold;
} lv2_param_t;

struct CHAIN_T;

typedef struct EFFECT_T {
    int instance;
    jack_client_t *jack_client;             // a chain member's is its head's, borrowed: the frame clock, the rate, the block
    char client_name[REQUESTED_CLIENT_NAME_BUF_SIZE];   // its own client's name as JACK gave it, for a member that leaves
    /* the chain (docs/chain.md): the one this instance is in, the head too, and its id in the library's chain, NULL
     * for an instance in a client of its own (the control thread's); the one a head's cycle runs instead of the
     * instance alone, changed only with chain_gate closed, which the cycle enters around reading it */
    struct CHAIN_T *chain;
    uint32_t chain_member;
    struct CHAIN_T *_Atomic chain_run;
    struct omx_cycle_gate chain_gate;
    jack_port_t *input_ports[CLAP_HOST_PORT_CHANNELS];
    jack_port_t *output_ports[CLAP_HOST_PORT_CHANNELS];
    uint32_t in_ports, out_ports;           // the main ports registered at add: what each cycle reads and writes
    /* a JACK port per auxiliary channel, registered at add in the description's order (port by port, channel by
     * channel), and the block each is bound to: one aux_frames long per channel, the JACK buffer size, made again when
     * that changes. Each cycle copies an input's JACK buffer into its block and a block onto an output's JACK buffer:
     * JACK's buffers move, a binding does not. */
    jack_port_t **aux_in_ports, **aux_out_ports;
    float **aux_in_bufs, **aux_out_bufs;
    float *aux_in_buf, *aux_out_buf;
    uint32_t aux_in_count, aux_out_count;   // the auxiliary channels registered, each side
    uint32_t *aux_in_widths, *aux_out_widths;   // each auxiliary port's channels as registered
    uint32_t aux_in_n, aux_out_n;           // the auxiliary ports registered, each side
    uint32_t aux_frames;                    // the frames of each block: 0 is none made
    /* a relayout whose activation waits for the buffer-size callback: its blocks were shorter than the new block, and
     * the callback makes them and activates with the audio role the relayout saved */
    struct omx_cycle_gate_pending pending;
    int relayout_pending;                   // a reported ports rescan not yet answered (ports_relayout)
    uint32_t relayout_tick;                 // the idle tick of the last relayout
    uint32_t relayouts_unchanged;           // relayouts on consecutive ticks that found the main layout unchanged
    struct omx_cycle_gate gate;             // the cycles shut out of the instance while a restart or relayout deactivates it
    /* the audio thread its client's thread-init callback named: the slot the core reads the audio role from
     * (omx_clap_host_name_slot), its own instance's and, as a head, its members', stored with no lock, so every cycle
     * from the first runs the plugin on the audio role */
    _Atomic(pthread_t) thread_named;
    /* the block size a head's buffer-size callback could not walk its members with while g_chain_members was held,
     * which it never waits on (0: none), for the control thread to apply under the lock (chain_members_reconcile) */
    _Atomic uint32_t resize_named;
    jack_port_t *midi_port;
    struct omx_msgring note_ring;           // control thread -> audio thread: the note verbs' MIDI messages
    void *note_ring_buf;                    // its NOTE_RING_BYTES, NULL for a plugin that takes no notes
    jack_port_t *midi_out_port;             // the plugin's note output, when it has one
    _Atomic uint32_t midi_out_full;         // messages midi_out had no room for
    struct omx_clap_instance *clap;
    uint32_t latency_published;
    _Atomic uint32_t latency_run;           // between its client's ports: its own, or as a head the chain's sum
    const clap_plugin_meters_t *meters;
    const clap_plugin_gain_adjustment_metering_t *gain_adjustment;
    _Atomic uint32_t gain_adjustment_bits;  // the float the audio thread published after its last cycle
    output_t *outputs;
    uint32_t outputs_count;
    const clap_plugin_entry_t *lv2_entry;   // an LV2 plugin: the adapter's entry of its bundle; NULL for a CLAP plugin
    lv2_param_t *lv2_params;
    uint32_t lv2_params_count;
    char **preset_keys, **preset_names;     // the last preset_list, the rows preset_get answers, encoded keys
    uint32_t presets_count;
    uint32_t param_events_dropped;          // the library's count of dropped parameter events, as last seen
    clap_id *gestures_open;                 // the parameters whose gesture begin went out on the feedback port, no end yet
    uint32_t gestures_open_count, gestures_open_cap;
    double rate;                            // the graph's rate when the instance was added
    struct host_transport_intent intent;    // the host's transport as this instance's audio thread last read it whole
    struct omx_clap_transport_src transport_src;  // the record the audio thread works out each block and the plugin reads
} effect_t;

static void presets_forget(effect_t *effect);

/* a chain the chain line declared: the library's chain the head's cycle runs, and its members in order, the head first,
 * changed by the control thread and read by the head's buffer-size callback on JACK's thread, both under
 * g_chain_members */
typedef struct CHAIN_T {
    int id;                                 // plugin-hostd's number of the chain; -1 for a free slot
    struct omx_clap_chain *walk;
    effect_t *head;
    effect_t *_Atomic members[OMX_CLAP_CHAIN_MEMBERS_MAX];
    _Atomic uint32_t count;
} chain_t;

/* a client of its own made ready for a member that leaves its chain, before the chain changes (client_ready) */
typedef struct {
    jack_client_t *client;
    jack_port_t *inputs[CLAP_HOST_PORT_CHANNELS];
    jack_port_t *outputs[CLAP_HOST_PORT_CHANNELS];
    jack_port_t *midi_in, *midi_out;    // the note ports of a plugin that has them
} own_client_t;

/* a client of its own a chain line stopped (members_stop): the instance and its main ports' links as they were, pairs of
 * an output and an input, for members_start to make again, since a client JACK stops loses every link of its ports */
typedef struct {
    effect_t *effect;
    char **links;
    uint32_t n_links;
} stopped_t;


/*
************************************************************************************************************************
*           LOCAL GLOBAL VARIABLES
************************************************************************************************************************
*/

static effect_t g_effects[MAX_INSTANCES];
// the layout digest pin_expect named for the next add of an instance, empty when none
static char g_layout_pins[MAX_INSTANCES][PHD_SHA256_HEX_LEN + 1];
static jack_client_t *g_jack_global_client;
// the xruns JACK reported to this worker, and what of them the feedback port has been told (#371)
static xrun_report_t g_xruns;
// the hosting.* code of the last add that failed with one, "" otherwise: the add reply carries it
static char g_add_reason[OMX_CLAP_LV2_WHY_MAX > OMX_CLAP_WHY_MAX ? OMX_CLAP_LV2_WHY_MAX : OMX_CLAP_WHY_MAX];
// the LV2 adapter took its configuration: an LV2 add can be tried
static int g_lv2_ready;
/* the state root the LV2 adapter makes each instance's state:makePath directory in: -s, else the XDG default */
static const char *g_state_root_given;
/* the host's one transport: what set_bpm, set_bpb and transport asked for, the control thread's copy and the published
 * one every audio thread reads; and -t, follow the JACK transport instead, set before the first add */
static struct host_transport_intent g_intent = { 0, 0, 0.0, HOST_TRANSPORT_BPB_DEFAULT, 0, 0.0, 0.0 };
static struct host_transport g_transport;
static int g_follow_jack;
/* -c: the lane's width, 1 or 2, asked of a plugin whose main ports differ */
static uint32_t g_lane_channels = 2;
static int32_t g_idle_hold = INT32_MIN;      // --idle-hold; INT32_MIN until given, the core's default then
static int g_idle_hold_said;                 // the refusal, said on stderr once
static int g_show_ui;                        // -S: every plugin's own window shown at its add (#180)
static int g_thread_pool = EFFECTS_THREAD_POOL_QUALIFIED;  // --thread-pool: which plugins get the thread pool (#395)
static uint64_t g_pool_late_seen;             // the pool's late count the last tick added to the xruns
/* the control thread's idle ticks, counted by effects_idle */
static uint32_t g_idle_ticks;
/* the chains the chain line declared; a slot whose id is -1 is free (effects_init) */
static chain_t g_chains[CHAINS_MAX];


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS
************************************************************************************************************************
*/

static int instance_exist(int effect_id)
{
    return INSTANCE_IS_VALID(effect_id) && g_effects[effect_id].jack_client != NULL;
}

/* "clap:<absolute path>#<plugin id>" */
static int parse_uri(const char *uri, char *path, size_t path_size, const char **id)
{
    const char *rest, *hash;
    size_t len;

    if (!uri || strncmp(uri, URI_SCHEME, strlen(URI_SCHEME)) != 0)
        return -1;

    rest = uri + strlen(URI_SCHEME);
    hash = strchr(rest, '#');
    if (rest[0] != '/' || !hash || hash[1] == '\0')
        return -1;

    len = (size_t)(hash - rest);
    if (len >= path_size)
        return -1;

    memcpy(path, rest, len);
    path[len] = '\0';
    *id = hash + 1;
    return 0;
}

static void sanitize_client_name(char *dst, size_t dst_size, const char *src, size_t max_len)
{
    size_t i, n = 0;

    for (i = 0; src[i] != '\0' && n < max_len && n + 1 < dst_size; i++)
    {
        char c = src[i];
        if (c == ':')
            c = '_';
        dst[n++] = c;
    }
    dst[n] = '\0';
}

/* a client opened under name, and the name it has copied to given (when not NULL) at the open: the one asked for, or the
 * one JACK made unique when it says so (JackNameNotUnique, never under JackUseExactName). The name is never read from
 * the client later: PipeWire's libjack rewrites the client's name on its own thread when the client's node appears,
 * which can be after jack_client_open returns, so a later jack_get_client_name races that write. */
static jack_client_t *open_as(const char *name, jack_options_t options, jack_status_t *status, char *given, size_t given_size)
{
    jack_client_t *jack_client = jack_client_open(name, options, status);

    if (jack_client && given)
        snprintf(given, given_size, "%s", (*status & JackNameNotUnique) ? jack_get_client_name(jack_client) : name);
    return jack_client;
}

static jack_client_t *open_jack_client(int instance, const char *client_name, char *given, size_t given_size)
{
    char effect_name[32];
    char requested_name[REQUESTED_CLIENT_NAME_BUF_SIZE];
    jack_client_t *jack_client;
    jack_status_t jack_status;
    size_t name_limit;

    snprintf(effect_name, sizeof(effect_name), "effect_%i", instance);

    if (!client_name || client_name[0] == '\0')
        return open_as(effect_name, JackNoStartServer, &jack_status, given, given_size);

    name_limit = (size_t)jack_client_name_size() - 1;
    sanitize_client_name(requested_name, sizeof(requested_name), client_name, name_limit);
    if (requested_name[0] == '\0')
        return open_as(effect_name, JackNoStartServer, &jack_status, given, given_size);

    jack_client = open_as(requested_name, JackNoStartServer | JackUseExactName, &jack_status, given, given_size);
    if (!jack_client && (jack_status & JackNameNotUnique))
    {
        char suffixed_name[REQUESTED_CLIENT_NAME_BUF_SIZE];
        char id_suffix[16];
        size_t base_len, suffix_len, keep;

        snprintf(id_suffix, sizeof(id_suffix), "_%i", instance);
        base_len = strlen(requested_name);
        suffix_len = strlen(id_suffix);
        keep = (base_len + suffix_len <= name_limit) ? base_len
             : (name_limit > suffix_len ? name_limit - suffix_len : 0);
        memcpy(suffixed_name, requested_name, keep);
        memcpy(suffixed_name + keep, id_suffix, suffix_len + 1);

        jack_client = open_as(suffixed_name, JackNoStartServer | JackUseExactName, &jack_status, given, given_size);
    }
    if (!jack_client)
        jack_client = open_as(effect_name, JackNoStartServer, &jack_status, given, given_size);

    return jack_client;
}

/* the chains' members as JACK's buffer-size callback walks them: the control thread changes a chain's members, and
 * frees a member's instance, holding this; the callback is not the process callback, which never takes it. A member
 * that has just left has a client of its own, whose callback does the same for it. JACK's thread never waits on it: the
 * callback tries it, and a block size it records while the control thread holds it (resize_named) the control thread
 * applies once it has let it go (chain_members_reconcile). No JACK thread writes an audio role: the thread-init
 * callback names its thread in its instance's slot (thread_named), which the core reads with no lock. */
static pthread_mutex_t g_chain_members = PTHREAD_MUTEX_INITIALIZER;
// an instance's resize_named waits for chain_members_reconcile
static _Atomic uint32_t g_named_pending;

/* under g_chain_members: `fn` on each member after the head of the chain `effect` heads; nothing for an instance that
 * heads none */
static void chain_followers_walk(const effect_t *effect, void (*fn)(effect_t *member, void *arg), void *arg)
{
    const chain_t *chain = atomic_load_explicit(&effect->chain_run, memory_order_relaxed);
    uint32_t count, i;

    if (!chain)
        return;
    count = atomic_load_explicit(&chain->count, memory_order_relaxed);
    for (i = 1; i < count && i < OMX_CLAP_CHAIN_MEMBERS_MAX; i++)
    {
        effect_t *member = atomic_load_explicit(&chain->members[i], memory_order_relaxed);

        if (member && member->clap)
            fn(member, arg);
    }
}

static void follower_resize(effect_t *member, void *arg)
{
    omx_cycle_gate_resize_record(&member->gate, *(const jack_nframes_t *)arg);
}

/* under g_chain_members: the block size JACK's thread named for `effect` and has not applied, recorded for its chain's
 * members, which run at its head's block */
static void named_apply(effect_t *effect)
{
    jack_nframes_t nframes;

    nframes = atomic_exchange_explicit(&effect->resize_named, 0u, memory_order_acquire);
    if (nframes)
        chain_followers_walk(effect, follower_resize, &nframes);
}

/* the control thread, each tick and after each instance's restart, resize and relayout: what JACK's threads named while
 * g_chain_members was held, applied under it. A name published after the flag is taken sets it again, for the next. */
static void chain_members_reconcile(void)
{
    int i;

    if (!atomic_exchange(&g_named_pending, 0u))
        return;
    pthread_mutex_lock(&g_chain_members);
    for (i = 0; i < MAX_INSTANCES; i++)
        named_apply(&g_effects[i]);
    pthread_mutex_unlock(&g_chain_members);
}

/* JACK's thread, its buffer-size callback: the chain's members, when `effect` heads one, run at its block. The size is
 * published first and the members walked under g_chain_members when it is free; when it is not, the control thread
 * walks them (chain_members_reconcile) */
static void chain_followers_each(effect_t *effect, jack_nframes_t nframes)
{
    if (!effect || !atomic_load_explicit(&effect->chain_run, memory_order_relaxed))
        return;
    atomic_store_explicit(&effect->resize_named, nframes, memory_order_release);
    atomic_store(&g_named_pending, 1u);
    if (pthread_mutex_trylock(&g_chain_members) == 0)
    {
        named_apply(effect);
        pthread_mutex_unlock(&g_chain_members);
    }
}

/* JACK's thread, before its first cycle (PipeWire's JACK calls it inside that cycle): its own instance's audio thread,
 * and its chain's members' when it heads one, named in one atomic store, the slot their roles read (callbacks_set,
 * chain_insert). No lock and no wait, so the first cycle runs the plugin on the audio role, whatever the control thread
 * holds meanwhile, and no role the control thread saves is written behind it. */
static void jack_thread_init(void *arg)
{
    effect_t *effect = arg;

    omx_denormals_off();
    if (!effect)
        return;
    atomic_store_explicit(&effect->thread_named, pthread_self(), memory_order_release);
}

/* audio thread, right after the cycle: get() is [audio-thread] and reads the block process() just ran. A cycle that did
 * not call process() publishes 0, what the extension answers for a plugin that is not processing. */
static void publish_gain_adjustment(effect_t *effect, uint32_t runs_before)
{
    float value = 0.0f;
    uint32_t bits;

    if (atomic_load_explicit(&effect->clap->stage.h.runs, memory_order_relaxed) != runs_before)
        value = (float)effect->gain_adjustment->get(effect->clap->plugin);
    memcpy(&bits, &value, sizeof(bits));
    atomic_store_explicit(&effect->gain_adjustment_bits, bits, memory_order_relaxed);
}

/* audio thread, ahead of the block: the transport the plugin reads in it, worked out of the host's transport at the
 * block's first frame, or out of the JACK transport with -t, and published for the instance's run on this same thread */
static void transport_block(effect_t *effect)
{
    struct omx_clap_transport rec;

    host_transport_read(&g_transport, &effect->intent);
    if (g_follow_jack)
    {
        jack_position_t pos;
        const jack_transport_state_t state = jack_transport_query(effect->jack_client, &pos);
        const struct host_jack_position at = {
            state == JackTransportRolling, pos.frame, (pos.valid & JackPositionBBT) != 0, pos.bar, pos.beat, pos.tick,
            pos.ticks_per_beat, pos.beats_per_minute, pos.beats_per_bar, pos.beat_type
        };

        host_transport_follow(&at, &effect->intent, pos.frame_rate ? (double)pos.frame_rate : effect->rate, &rec);
    }
    else
    {
        host_transport_own(&effect->intent, jack_last_frame_time(effect->jack_client), effect->rate, &rec);
    }
    omx_clap_transport_publish(&effect->transport_src, &rec);
}

/* audio thread, right after the cycle: what the plugin sent out in it, on midi_out at its frame. A time past the cycle
 * is written on its last frame, a time before the previous message's on that message's frame, since a jack MIDI buffer
 * holds its events in order; a message the buffer has no room for is counted. */
static void notes_out(effect_t *effect, jack_nframes_t nframes)
{
    struct omx_clap_note_out_event events[NOTES_OUT_CHUNK];
    void *midi = jack_port_get_buffer(effect->midi_out_port, nframes);
    jack_nframes_t last = 0;
    uint32_t n, i;

    jack_midi_clear_buffer(midi);
    while ((n = omx_clap_host_notes_out(effect->clap, events, NOTES_OUT_CHUNK)) > 0)
    {
        for (i = 0; i < n; i++)
        {
            jack_nframes_t time = events[i].time < nframes ? events[i].time : nframes - 1;

            if (time < last)
                time = last;
            if (jack_midi_event_write(midi, time, events[i].data, events[i].size) != 0)
                atomic_fetch_add_explicit(&effect->midi_out_full, 1u, memory_order_relaxed);
            last = time;
        }
    }
}

/* one cycle inside the gate, from `inputs` to `outputs` (the main ports' channels: the JACK buffers of its own client,
 * a chain's blocks for a member and for an instance a member follows): `open` zero when the control thread shut it, and
 * the instance is then not read at all */
static void process_gated(effect_t *effect, const float *const *inputs, float *const *outputs, jack_nframes_t nframes, int open)
{
    uint32_t c, runs = 0;

    if (open && effect->note_ring_buf)
    {
        // the note verbs first, at the cycle's first frame, ahead of what midi_in carries in it
        const void *m;
        uint32_t size;

        while ((m = omx_msgring_peek(&effect->note_ring, &size)) != NULL)
        {
            omx_clap_host_note_in(effect->clap, 0, m, size);
            omx_msgring_pop(&effect->note_ring);
        }
    }
    if (open && effect->midi_port)
    {
        void *midi = jack_port_get_buffer(effect->midi_port, nframes);
        jack_nframes_t count = jack_midi_get_event_count(midi);
        jack_midi_event_t event;
        jack_nframes_t e;

        for (e = 0; e < count; e++)
            if (jack_midi_event_get(&event, midi, e) == 0)
                omx_clap_host_note_in(effect->clap, event.time, event.buffer, event.size);
    }

    // a cycle with the gate shut (a restart, a relayout or a resize deactivating the instance), the instance deactivated
    // (a relayout or a resize that left it inactive), a block size JACK changed to that the tick has not applied yet
    // (the instance is activated for the old one), or a cycle longer than the auxiliary blocks: the audio passes dry, a
    // source's output silent, and the plugin is not run. The flag and the blocks are read inside the open gate, where no
    // control thread writes them
    if (!open || !__atomic_load_n(&effect->clap->active, __ATOMIC_ACQUIRE) || omx_cycle_gate_resizing(&effect->gate)
        || ((effect->aux_in_count || effect->aux_out_count) && nframes > effect->aux_frames))
    {
        for (c = 0; c < effect->out_ports; c++)
        {
            if (effect->in_ports == 0)
                memset(outputs[c], 0, nframes * sizeof(float));
            else if (outputs[c] != inputs[c < effect->in_ports ? c : effect->in_ports - 1])
                memcpy(outputs[c], inputs[c < effect->in_ports ? c : effect->in_ports - 1], nframes * sizeof(float));
        }
        for (c = 0; c < effect->aux_out_count; c++)
            if (effect->aux_out_ports[c])
                memset(jack_port_get_buffer(effect->aux_out_ports[c], nframes), 0, nframes * sizeof(float));
        // the note output says nothing this cycle: JACK hands the buffer back as the last cycle left it
        if (effect->midi_out_port)
            jack_midi_clear_buffer(jack_port_get_buffer(effect->midi_out_port, nframes));
        return;
    }
    // a chain member's auxiliary inputs have no port: they read silence, as an unconnected port of its own would
    for (c = 0; c < effect->aux_in_count; c++)
        if (effect->aux_in_ports[c])
            memcpy(effect->aux_in_bufs[c], jack_port_get_buffer(effect->aux_in_ports[c], nframes), nframes * sizeof(float));
        else
            memset(effect->aux_in_bufs[c], 0, nframes * sizeof(float));

    if (effect->gain_adjustment)
        runs = atomic_load_explicit(&effect->clap->stage.h.runs, memory_order_relaxed);
    transport_block(effect);
    omx_clap_host_run_ports(effect->clap, inputs, outputs, nframes);
    for (c = 0; c < effect->aux_out_count; c++)
        if (effect->aux_out_ports[c])
            memcpy(jack_port_get_buffer(effect->aux_out_ports[c], nframes), effect->aux_out_bufs[c], nframes * sizeof(float));
    if (effect->gain_adjustment)
        publish_gain_adjustment(effect, runs);
    if (effect->midi_out_port)
        notes_out(effect, nframes);
    else if (effect->clap->note_outputs)
    {
        // a chain member's note output has no port: what the plugin sent out is dropped
        struct omx_clap_note_out_event dropped[NOTES_OUT_CHUNK];

        while (omx_clap_host_notes_out(effect->clap, dropped, NOTES_OUT_CHUNK) > 0)
            ;
    }
}

/* a chain's member, in the head's cycle: the instance inside its own gate, as its own client's cycle runs it */
static void chain_stage(void *ctx, const float *const *inputs, float *const *outputs, uint32_t frames)
{
    effect_t *effect = ctx;

    process_gated(effect, inputs, outputs, frames, omx_cycle_gate_enter(&effect->gate));
    omx_cycle_gate_leave(&effect->gate);
}

/* the client's cycle: the instance from its JACK input ports to its output ports, or, when it heads a chain, the
 * library's chain between those ports, every member a chain_stage. The chain is read inside the head's chain gate,
 * which the control thread closes to change it: a cycle that finds it closed runs the head alone, what the chain is
 * then (a chain of the head alone, before its first member joins and after its last one leaves) */
static int process(jack_nframes_t nframes, void *arg)
{
    effect_t *effect = arg;
    const float *inputs[CLAP_HOST_PORT_CHANNELS];
    float *outputs[CLAP_HOST_PORT_CHANNELS];
    const chain_t *chain;
    uint32_t c;

    if (!effect || !effect->clap)
        return 0;
    // the ports registered at add, never the description: a relayout reads that again while this cycle runs, and the
    // instance is active only while its main ports are these
    for (c = 0; c < effect->in_ports; c++)
        inputs[c] = jack_port_get_buffer(effect->input_ports[c], nframes);
    for (c = 0; c < effect->out_ports; c++)
        outputs[c] = jack_port_get_buffer(effect->output_ports[c], nframes);
    if (omx_cycle_gate_enter(&effect->chain_gate) && (chain = atomic_load_explicit(&effect->chain_run, memory_order_relaxed)))
        omx_clap_chain_process(chain->walk, inputs, outputs, nframes);
    else
        chain_stage(effect, inputs, outputs, nframes);
    omx_cycle_gate_leave(&effect->chain_gate);
    return 0;
}

/* the plugin's latency on top of what reaches the ports of the other direction */
static void latency(jack_latency_callback_mode_t mode, void *arg)
{
    effect_t *effect = arg;
    jack_port_t **from, **to;
    jack_latency_range_t range, total;
    uint32_t c, from_count, to_count, own;

    if (!effect->clap)
        return;

    if (mode == JackCaptureLatency)
    {
        from = effect->input_ports;
        from_count = effect->in_ports;
        to = effect->output_ports;
        to_count = effect->out_ports;
    }
    else
    {
        from = effect->output_ports;
        from_count = effect->out_ports;
        to = effect->input_ports;
        to_count = effect->in_ports;
    }

    total.min = UINT32_MAX;
    total.max = 0;
    for (c = 0; c < from_count; c++)
    {
        jack_port_get_latency_range(from[c], mode, &range);
        if (range.min < total.min)
            total.min = range.min;
        if (range.max > total.max)
            total.max = range.max;
    }
    if (total.min == UINT32_MAX)
        total.min = 0;

    // a chain's head answers for every member, itself too: the library's sum of their latencies, between its ports
    own = atomic_load_explicit(&effect->latency_run, memory_order_relaxed);
    total.min += own;
    total.max += own;
    for (c = 0; c < to_count; c++)
        jack_port_set_latency_range(to[c], mode, &total);
}

/* control thread: the figure the latency callback answers with, and a recompute so the graph asks. A chain's member
 * states it to the chain instead, whose sum the head's ports answer with (chain_latency_changed) */
static void publish_latency(effect_t *effect)
{
    effect->latency_published = omx_clap_host_latency(effect->clap);
    fprintf(stderr, "effect_%i: latency %u frames\n", effect->instance, effect->latency_published);
    if (effect->chain)
    {
        omx_clap_chain_member_latency_set(effect->chain->walk, effect->chain_member, effect->latency_published);
        return;
    }
    atomic_store_explicit(&effect->latency_run, effect->latency_published, memory_order_relaxed);
    jack_recompute_total_latencies(effect->jack_client);
}

/* one side's auxiliary ports bound to its blocks, `frames` long each, or unbound with `frames` 0: 0, or -1 when a
 * binding is refused, the side then unbound (its ports read silence and write their scratch). Nothing is allocated or
 * freed. Control thread or the buffer-size callback, the instance not running. */
static int aux_side_bind(effect_t *effect, bool is_input, uint32_t frames)
{
    struct omx_clap_instance *clap = effect->clap;
    const uint32_t ports = is_input ? effect->aux_in_n : effect->aux_out_n;
    const uint32_t *widths = is_input ? effect->aux_in_widths : effect->aux_out_widths;
    float **bufs = is_input ? effect->aux_in_bufs : effect->aux_out_bufs;
    uint32_t p, base;

    for (p = 0, base = 0; p < ports; base += widths[p], p++)
    {
        float *const *b = frames ? &bufs[base] : NULL;
        const int bound = is_input ? omx_clap_host_bind_aux_input(clap, p, b, widths[p], frames)
                                   : omx_clap_host_bind_aux_output(clap, p, b, widths[p], frames);

        if (bound != 0)
        {
            if (frames)
                aux_side_bind(effect, is_input, 0);
            return -1;
        }
    }
    return 0;
}

/* one side's blocks made again `frames` long each and bound to its auxiliary ports, the old ones unbound and freed
 * first: 0, or -1 when memory runs out or a binding is refused, the side then unbound and holding no block */
static int aux_side_blocks(effect_t *effect, bool is_input, uint32_t frames)
{
    const uint32_t count = is_input ? effect->aux_in_count : effect->aux_out_count;
    float **bufs = is_input ? effect->aux_in_bufs : effect->aux_out_bufs;
    float **block = is_input ? &effect->aux_in_buf : &effect->aux_out_buf;
    uint32_t c;

    aux_side_bind(effect, is_input, 0);
    free(*block);
    *block = NULL;
    if (count == 0 || frames == 0)
        return 0;
    *block = calloc((size_t)count * frames, sizeof(float));
    if (!*block)
        return -1;
    for (c = 0; c < count; c++)
        bufs[c] = *block + (size_t)c * frames;
    if (aux_side_bind(effect, is_input, frames) != 0)
    {
        free(*block);
        *block = NULL;
        return -1;
    }
    return 0;
}

/* both sides' blocks `frames` long, bound (aux_side_blocks); aux_frames is `frames` when both are, 0 otherwise, and a
 * cycle longer than aux_frames passes dry */
static int aux_blocks(effect_t *effect, uint32_t frames)
{
    effect->aux_frames = 0;
    if (aux_side_blocks(effect, true, frames) != 0 || aux_side_blocks(effect, false, frames) != 0)
    {
        aux_side_blocks(effect, true, 0);
        aux_side_blocks(effect, false, 0);
        return -1;
    }
    effect->aux_frames = frames;
    return 0;
}

/* one side's auxiliary ports as aux_register kept them registered on the instance's client, a JACK port per channel
 * named by layout_port_name: SUCCESS or ERR_JACK_PORT_REGISTER. Before jack_activate: no cycle runs. */
static int aux_ports_register(effect_t *effect, bool is_input)
{
    const uint32_t n = is_input ? effect->aux_in_n : effect->aux_out_n;
    const uint32_t *widths = is_input ? effect->aux_in_widths : effect->aux_out_widths;
    jack_port_t **jack = is_input ? effect->aux_in_ports : effect->aux_out_ports;
    uint32_t p, k, c = 0;
    char name[LAYOUT_PORT_NAME_SIZE];

    for (p = 0; p < n; p++)
        for (k = 0; k < widths[p]; k++)
        {
            layout_port_name(name, is_input, p, k);
            jack[c] = jack_port_register(effect->jack_client, name, JACK_DEFAULT_AUDIO_TYPE,
                                         is_input ? JackPortIsInput : JackPortIsOutput, 0);
            if (!jack[c++])
                return ERR_JACK_PORT_REGISTER;
        }
    return SUCCESS;
}

/* one side's auxiliary ports of the description kept, their widths and a JACK port each, then registered
 * (aux_ports_register): SUCCESS, ERR_MEMORY_ALLOCATION or ERR_JACK_PORT_REGISTER. Before jack_activate: no cycle runs. */
static int aux_register(effect_t *effect, bool is_input)
{
    const struct omx_clap_instance *clap = effect->clap;
    const uint32_t n = is_input ? clap->aux_inputs : clap->aux_outputs;
    const struct omx_clap_port *ports = is_input ? clap->aux_in : clap->aux_out;
    jack_port_t **jack;
    uint32_t *widths;
    uint32_t p, count = 0;

    for (p = 0; p < n; p++)
        count += ports[p].channels;
    jack = calloc(count ? count : 1, sizeof(*jack));
    widths = calloc(n ? n : 1, sizeof(*widths));
    if (is_input)
    {
        effect->aux_in_ports = jack;
        effect->aux_in_widths = widths;
        effect->aux_in_bufs = calloc(count ? count : 1, sizeof(float *));
    }
    else
    {
        effect->aux_out_ports = jack;
        effect->aux_out_widths = widths;
        effect->aux_out_bufs = calloc(count ? count : 1, sizeof(float *));
    }
    if (!jack || !widths || !(is_input ? effect->aux_in_bufs : effect->aux_out_bufs))
        return ERR_MEMORY_ALLOCATION;
    for (p = 0; p < n; p++)
        widths[p] = ports[p].channels;
    if (is_input)
        effect->aux_in_count = count, effect->aux_in_n = n;
    else
        effect->aux_out_count = count, effect->aux_out_n = n;
    return aux_ports_register(effect, is_input);
}

/* whether the description's auxiliary ports are still the ones registered at add, port by port and width by width */
static int aux_carried(const effect_t *effect)
{
    const struct omx_clap_instance *clap = effect->clap;
    uint32_t p;

    if (clap->aux_inputs != effect->aux_in_n || clap->aux_outputs != effect->aux_out_n)
        return 0;
    for (p = 0; p < effect->aux_in_n; p++)
        if (clap->aux_in[p].channels != effect->aux_in_widths[p])
            return 0;
    for (p = 0; p < effect->aux_out_n; p++)
        if (clap->aux_out[p].channels != effect->aux_out_widths[p])
            return 0;
    return 1;
}

/* A re-activation (a resize, a relayout, a restart) is a main-thread call of the plugin (CLAP's own rule) and, for an LV2
 * plugin, a lilv call on the adapter's one world (the default state is loaded at activate), and neither bears two
 * threads at a time: two effects re-activating together crashed in sord's node table (jack_lv2_e2e, the shrink). All of
 * them run on the control thread under this lock, which audio_ports takes too to read what they write. */
static pthread_mutex_t g_reactivate = PTHREAD_MUTEX_INITIALIZER;

/* omx_cycle_gate_resize's view of the auxiliary blocks: stale when an instance with auxiliary ports has them at another
 * length, made again by aux_blocks */
static int aux_stale(void *ctx, uint32_t nframes)
{
    const effect_t *effect = ctx;

    return (effect->aux_in_count || effect->aux_out_count) && nframes != effect->aux_frames;
}

static int aux_reblock(void *ctx, uint32_t nframes)
{
    return aux_blocks(ctx, nframes);
}

/* JACK's thread, its process cycle stopped while this runs: the new size recorded for the control thread's next tick
 * (resize_apply), and nothing of the plugin done here, since a deactivate and an activate are main-thread calls. Until
 * the tick applies it the cycles pass dry (process_gated): the instance is still activated for the old size. */
static int buffer_size(jack_nframes_t nframes, void *arg)
{
    effect_t *effect = arg;

    if (effect && effect->clap)
        omx_cycle_gate_resize_record(&effect->gate, nframes);
    // a chain's members run at its head's block
    chain_followers_each(effect, nframes);
    return 0;
}

/* the control thread's tick, before the relayout: a size buffer_size recorded applied by the gate's resize, under the
 * lock every restart and relayout holds, with the cycles shut out. Any change, a shrink too: the adapter promised the
 * plugin the activation's length (bufsz:fixedBlockLength) and refuses a cycle of another one. The auxiliary blocks,
 * which a cycle copies, are made again at the new size; an active instance, or one whose relayout waits for these blocks
 * (pending), is activated at it; one a relayout left inactive otherwise is the relayout's to activate; a plugin that
 * rescans its ports in the deactivate stays inactive, the rescan reported, for the relayout that follows. */
static void resize_apply(effect_t *effect)
{
    const struct omx_cycle_gate_blocks blocks = { aux_stale, aux_reblock, effect };
    char why[OMX_CLAP_WHY_MAX] = "";
    uint32_t nframes;

    switch (omx_cycle_gate_resize_tick(&effect->gate, effect->clap, &g_reactivate, jack_get_sample_rate(effect->jack_client),
                                       CLAP_HOST_ROLE_POLL_US, &blocks, &effect->pending, why, &nframes))
    {
    case OMX_CYCLE_GATE_REBLOCK_FAILED:
        fprintf(stderr, "effect_%i: its auxiliary blocks cannot be made for %u frames; it passes dry until %s\n", effect->instance,
                nframes, effect->pending.set ? "the next change of the buffer size" : "it is added again");
        break;
    case OMX_CYCLE_GATE_RESCANNED:
        // its ports changed in the deactivate: the rescan is reported (omx_clap_host_ports_rescanned), and the relayout
        // right after reads the description again and activates
        fprintf(stderr, "%s: its ports changed in the deactivate at %u frames; its relayout answers it\n", effect->clap->desc->id,
                nframes);
        break;
    case OMX_CYCLE_GATE_REFUSED:
        fprintf(stderr, "%s: it does not activate again at %u frames: %s; it passes dry\n", effect->clap->desc->id, nframes, why);
        break;
    default:
        break;
    }
}

/* a chain's member other than its head runs in the head's client, which it does not own */
static int client_borrowed(const effect_t *effect)
{
    return effect->chain && effect->chain->head != effect;
}

static void instance_free(effect_t *effect)
{
    // the client goes first: no cycle can run the stage while the plugin is stopped and destroyed. A chain member's is
    // its head's: it left the chain's walk first (chain_drop)
    if (effect->jack_client && !client_borrowed(effect))
        jack_client_close(effect->jack_client);
    if (effect->clap)
    {
        // the plugin's window goes before the plugin: clap.gui's destroy, then the host's window
        window_close(effect->clap);
        omx_clap_host_stop(effect->clap);
        omx_clap_host_close(effect->clap);
    }
    // the entry outlives the plugin made from it
    if (effect->lv2_entry)
        omx_clap_lv2_entry_release(effect->lv2_entry);
    presets_forget(effect);
    free(effect->aux_in_ports);
    free(effect->aux_out_ports);
    free(effect->aux_in_bufs);
    free(effect->aux_out_bufs);
    free(effect->aux_in_buf);
    free(effect->aux_out_buf);
    free(effect->aux_in_widths);
    free(effect->aux_out_widths);
    free(effect->outputs);
    free(effect->lv2_params);
    free(effect->gestures_open);
    free(effect->note_ring_buf);
    memset(effect, 0, sizeof(effect_t));
}

/* the file and the plugin in it, then the core's judgment of the layout: a file or an id that isn't there is an invalid
 * URI, anything the core refuses an instantiation error, with its hosting code on stderr */
static int open_clap(const char *path, const char *id, struct omx_clap_instance **clap)
{
    struct omx_clap_binary *binary;
    char reason[256], why[OMX_CLAP_WHY_MAX];
    uint32_t count, i;
    int ret;

    binary = omx_clap_host_binary_open(path, reason, sizeof(reason));
    if (!binary)
    {
        fprintf(stderr, "%s\n", reason);
        return ERR_LV2_INVALID_URI;
    }
    count = omx_clap_host_binary_count(binary);
    for (i = 0; i < count; i++)
    {
        const clap_plugin_descriptor_t *desc = omx_clap_host_binary_descriptor(binary, i);

        if (desc && desc->id && strcmp(desc->id, id) == 0)
            break;
    }
    if (i == count)
    {
        fprintf(stderr, "no plugin %s in %s\n", id, path);
        omx_clap_host_binary_close(binary);
        return ERR_LV2_INVALID_URI;
    }
    ret = omx_clap_host_open(path, id, clap, why);
    omx_clap_host_binary_close(binary);
    if (ret != 0)
    {
        fprintf(stderr, "%s: %s\n", id, why);
        snprintf(g_add_reason, sizeof(g_add_reason), "%s", why);
        return ERR_LV2_INSTANTIATION;
    }
    return SUCCESS;
}

/* what an LV2 add looks for on the LV2 path: the bundle whose entry lists the URI, and the refusal of a bundle the
 * adapter could not read */
struct lv2_search {
    const char *uri;
    const clap_plugin_entry_t *entry;
    char why[OMX_CLAP_LV2_WHY_MAX];
};

/* a candidate bundle: its entry, kept when its factory lists the URI and released otherwise */
static int lv2_candidate(const char *bundle, void *ctx)
{
    struct lv2_search *search = ctx;
    const clap_plugin_factory_t *factory;
    const clap_plugin_entry_t *entry;
    char why[OMX_CLAP_LV2_WHY_MAX];
    uint32_t i, count;

    entry = omx_clap_lv2_entry(bundle, why);
    if (!entry)
    {
        fprintf(stderr, "%s: %s\n", bundle, why);
        snprintf(search->why, sizeof(search->why), "%s", why);
        return 0;
    }
    factory = entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    count = factory ? factory->get_plugin_count(factory) : 0;
    for (i = 0; i < count; i++)
    {
        const clap_plugin_descriptor_t *desc = factory->get_plugin_descriptor(factory, i);

        if (desc && desc->id && strcmp(desc->id, search->uri) == 0)
        {
            search->entry = entry;
            return 1;
        }
    }
    omx_clap_lv2_entry_release(entry);
    return 0;
}

/* the reason a refusal of the core names, or the adapter's own when the core saw only the plugin fail */
static void lv2_reason(char why[OMX_CLAP_WHY_MAX])
{
    const char *adapter = omx_clap_lv2_last_refusal();

    if (strcmp(why, CLAP_HOST_CODE_HEADLESS_FAILED) == 0 && adapter[0])
        snprintf(why, OMX_CLAP_WHY_MAX, "%s", adapter);
}

/* the bundle on the LV2 path that holds `uri`, and the plugin in it through the adapter, judged as a CLAP plugin is: a
 * URI no bundle holds is an invalid URI, anything the adapter or the core refuses an instantiation error, with its
 * hosting code on stderr and in the reply */
static int open_lv2(const char *uri, effect_t *effect)
{
    struct lv2_search search;
    char why[OMX_CLAP_WHY_MAX];

    memset(&search, 0, sizeof(search));
    search.uri = uri;
    if (!g_lv2_ready)
    {
        fprintf(stderr, "%s: %s: the LV2 adapter is not configured\n", uri, OMX_HOSTING_NO_REALISATION);
        snprintf(g_add_reason, sizeof(g_add_reason), "%s", OMX_HOSTING_NO_REALISATION);
        return ERR_LV2_INSTANTIATION;
    }
    switch (lv2_locate(NULL, uri, lv2_candidate, &search))
    {
    case LV2_LOCATE_READ_FAILED:
        // a manifest's own allocation failed: a read failure, never that no bundle holds the URI
        fprintf(stderr, "%s: a manifest on the LV2 path could not be read\n", uri);
        snprintf(g_add_reason, sizeof(g_add_reason), "a manifest on the LV2 path could not be read");
        return ERR_LV2_INSTANTIATION;
    case 0:
        if (search.why[0])
        {
            // a bundle that may hold it could not be read: lilv missing, or the bundle broken
            fprintf(stderr, "%s: %s\n", uri, search.why);
            snprintf(g_add_reason, sizeof(g_add_reason), "%s", search.why);
            return ERR_LV2_INSTANTIATION;
        }
        fprintf(stderr, "%s: no bundle on the LV2 path holds it\n", uri);
        return ERR_LV2_INVALID_URI;
    default:
        break;
    }
    effect->lv2_entry = search.entry;
    if (omx_clap_host_open_entry(search.entry, uri, &effect->clap, why) != 0)
    {
        lv2_reason(why);
        fprintf(stderr, "%s: %s\n", uri, why);
        snprintf(g_add_reason, sizeof(g_add_reason), "%s", why);
        return ERR_LV2_INSTANTIATION;
    }
    return SUCCESS;
}

/* an LV2 plugin's control ports by their symbols, read once at the add */
static int lv2_params_build(effect_t *effect)
{
    struct omx_clap_instance *clap = effect->clap;
    uint32_t i, count = clap->params ? clap->params->count(clap->plugin) : 0;

    effect->lv2_params = count ? calloc(count, sizeof(lv2_param_t)) : NULL;
    if (count && !effect->lv2_params)
        return -1;
    for (i = 0; i < count; i++)
    {
        clap_param_info_t info;
        const char *symbol;
        lv2_param_t *param;

        memset(&info, 0, sizeof(info));
        if (!clap->params->get_info(clap->plugin, i, &info))
            return -1;
        symbol = omx_clap_lv2_param_symbol(clap->plugin, info.id);
        if (!symbol)
            continue;
        param = &effect->lv2_params[effect->lv2_params_count++];
        param->symbol = symbol;
        param->id = info.id;
        param->flags = info.flags;
        param->hold = info.default_value;
    }
    return 0;
}

static const lv2_param_t *lv2_param_by_symbol(const effect_t *effect, const char *symbol)
{
    uint32_t i;

    for (i = 0; i < effect->lv2_params_count; i++)
        if (strcmp(effect->lv2_params[i].symbol, symbol) == 0)
            return &effect->lv2_params[i];
    return NULL;
}

static const lv2_param_t *lv2_param_by_id(const effect_t *effect, clap_id id)
{
    uint32_t i;

    for (i = 0; i < effect->lv2_params_count; i++)
        if (effect->lv2_params[i].id == id)
            return &effect->lv2_params[i];
    return NULL;
}

/* the plugin's own bypass of an LV2 plugin (lv2:enabled, a bypass port) is the host's: the value that keeps the plugin
 * processing is the host's bypass off, any other value on */
static int lv2_bypass_param(const lv2_param_t *param)
{
    return param && (param->flags & CLAP_PARAM_IS_BYPASS);
}

/* a write on a plugin no cycle drains yet reaches it through params.flush; one a cycle runs waits for the cycle */
static int clap_param_set(struct omx_clap_instance *clap, clap_id id, double value)
{
    struct omx_clap_param_row row;
    uint32_t state;

    if (omx_clap_host_param_row_of(clap, id, &row) != 0)
        return ERR_LV2_INVALID_PARAM_SYMBOL;
    if (value < row.min)
        value = row.min;
    else if (value > row.max)
        value = row.max;
    if (omx_clap_host_param_write(clap, id, value) != 0)
        return ERR_INVALID_OPERATION;
    state = atomic_load(&clap->stage.state);
    if (state == OMX_CLAP_IDLE || state == OMX_CLAP_ARMED)
        omx_clap_host_param_deliver(clap);
    return SUCCESS;
}

static int clap_param_get(struct omx_clap_instance *clap, clap_id id, double *value)
{
    if (!omx_clap_host_param_readable(clap, id))
        return ERR_LV2_INVALID_PARAM_SYMBOL;
    omx_clap_host_settle(clap, QUEUE_SETTLE_US);
    if (omx_clap_host_param_read(clap, id, value) != 0)
        return ERR_LV2_INVALID_PARAM_SYMBOL;
    return SUCCESS;
}

static int clap_state_save(struct omx_clap_instance *clap, const char *filename)
{
    size_t length = 0;
    uint8_t *buffer;
    FILE *file;
    int ok;

    if (!clap->state)
        return ERR_INVALID_OPERATION;

    omx_clap_host_settle(clap, QUEUE_SETTLE_US);
    buffer = malloc(CLAP_HOST_STATE_MAX_BYTES);
    if (!buffer || omx_clap_host_state_save(clap, buffer, CLAP_HOST_STATE_MAX_BYTES, &length) != 0)
    {
        free(buffer);
        return ERR_LV2_CANT_LOAD_STATE;
    }

    file = fopen(filename, "wb");
    if (!file)
    {
        free(buffer);
        return ERR_LV2_CANT_LOAD_STATE;
    }
    ok = fwrite(buffer, 1, length, file) == length;
    ok = fclose(file) == 0 && ok;
    free(buffer);
    return ok ? SUCCESS : ERR_LV2_CANT_LOAD_STATE;
}

static int clap_state_load(struct omx_clap_instance *clap, const char *filename)
{
    size_t length;
    uint8_t *buffer;
    FILE *file;
    int ok;

    if (!clap->state)
        return ERR_INVALID_OPERATION;

    file = fopen(filename, "rb");
    if (!file)
        return ERR_LV2_CANT_LOAD_STATE;

    buffer = malloc(CLAP_HOST_STATE_MAX_BYTES);
    if (!buffer)
    {
        fclose(file);
        return ERR_LV2_CANT_LOAD_STATE;
    }
    length = fread(buffer, 1, CLAP_HOST_STATE_MAX_BYTES, file);
    ok = !ferror(file) && length < CLAP_HOST_STATE_MAX_BYTES;
    fclose(file);

    if (ok)
        ok = omx_clap_host_state_load(clap, buffer, length) == 0;
    free(buffer);
    return ok ? SUCCESS : ERR_LV2_CANT_LOAD_STATE;
}

/* The lane's width asked of an inactive plugin (request_layout): -c channels on each main port, mono or stereo, the
 * input asked only when `has_input` (the plugin declared one at add; its JACK inputs after). 0 applied,
 * OMX_CLAP_LAYOUT_DECLARED when the plugin keeps its declared layout (the ports carry it as it is), -1 refused with
 * `why`; the description is read again either way. */
static int lane_request(struct omx_clap_instance *clap, int has_input, char why[OMX_CLAP_WHY_MAX])
{
    clap_audio_port_configuration_request_t reqs[2];
    const char *type = g_lane_channels == 1 ? CLAP_PORT_MONO : CLAP_PORT_STEREO;
    uint32_t n = 0;

    memset(reqs, 0, sizeof(reqs));
    if (has_input)
    {
        reqs[n].is_input = true;
        reqs[n].channel_count = g_lane_channels;
        reqs[n].port_type = type;
        n++;
    }
    reqs[n].is_input = false;
    reqs[n].channel_count = g_lane_channels;
    reqs[n].port_type = type;
    n++;
    return omx_clap_host_request_layout(clap, reqs, n, why);
}

/*
 * A ports rescan the tick reported, once per report: the plugin's layout may have moved, and only an inactive plugin
 * is asked. The instance is unpublished and deactivated, its cycles shut out of it until it is armed again or left
 * inactive (omx_cycle_gate_deactivate; a restart whose deactivate made the rescan left it inactive
 * already), the lane's width asked again (lane_request, which reads the description again whatever the plugin
 * answers), and the instance activated again with its audio role as it was when its main ports are still the JACK
 * ports registered at add. A description those ports do not carry, or one refused, leaves it deactivated, its audio
 * passing dry, said on stderr; its next rescan asks again. Relayouts on consecutive ticks that find the main layout
 * unchanged (a plugin rescanning in every activate) stop at RELAYOUT_UNCHANGED_MAX, said once, while the instance is
 * active; an inactive one is always answered. An RT that does not stop in time leaves the report for the next tick.
 */
static void ports_relayout(effect_t *effect)
{
    struct omx_clap_instance *clap = effect->clap;
    char why[OMX_CLAP_WHY_MAX] = "";
    pthread_t thread;
    int held, unchanged;
    uint32_t in_channels, channels, in_kind, out_kind;

    if (omx_clap_host_ports_rescanned(clap))
        effect->relayout_pending = 1;
    if (!effect->relayout_pending)
        return;
    if (effect->relayout_tick + 1 != g_idle_ticks)
        effect->relayouts_unchanged = 0;
    effect->relayout_tick = g_idle_ticks;
    if (clap->active && effect->relayouts_unchanged >= RELAYOUT_UNCHANGED_MAX)
    {
        if (effect->relayouts_unchanged++ == RELAYOUT_UNCHANGED_MAX)
            fprintf(stderr, "%s: %u relayouts in a row found its layout unchanged; its ports rescans go unanswered while it "
                    "keeps making them\n", clap->desc->id, RELAYOUT_UNCHANGED_MAX);
        effect->relayout_pending = 0;
        return;
    }
    // the audio role this thread gives back: only the control thread writes it (JACK's thread names its own in the
    // slot, which the role keeps whatever is given back); read again after the stop below, when one set since holds it
    thread = clap->audio_thread;
    held = clap->audio_role_held;
    in_channels = clap->in_channels;
    channels = clap->channels;
    in_kind = clap->main_in.kind;
    out_kind = clap->main_out.kind;
    pthread_mutex_lock(&g_reactivate);
    // the cycles stop the stage, then are shut out of the instance until it is armed again or left inactive
    if (omx_cycle_gate_deactivate(&effect->gate, clap, CLAP_HOST_ROLE_POLL_US, CLAP_HOST_ROLE_TIMEOUT_US) != 0)
    {
        pthread_mutex_unlock(&g_reactivate);
        return;
    }
    effect->relayout_pending = 0;
    // from here to the arm the audio role is this thread's to give back; the stop released it, so a role held now is
    // one set since, the newest
    // a relayout still waiting on its blocks keeps the audio role it saved; every outcome below decides again, and only
    // the short-block one waits again
    if (clap->audio_role_held)
    {
        thread = clap->audio_thread;
        held = 1;
    }
    else if (effect->pending.set)
    {
        thread = effect->pending.thread;
        held = effect->pending.held;
    }
    effect->pending.set = 0;
    if (lane_request(clap, effect->in_ports != 0, why) < 0)
        fprintf(stderr, "%s: its layout after the ports rescan is refused: %s; it passes dry\n", clap->desc->id, why);
    else if (clap->in_channels != effect->in_ports || clap->channels != effect->out_ports)
        fprintf(stderr, "%s: its main ports are now %u in and %u out, not the %u and %u it was added with; it passes dry until it "
                "is added again\n", clap->desc->id, clap->in_channels, clap->channels, effect->in_ports, effect->out_ports);
    else if (!aux_carried(effect))
        fprintf(stderr, "%s: its auxiliary ports are now %u in and %u out, not the ones it was added with; it passes dry until "
                "it is added again\n", clap->desc->id, clap->aux_inputs, clap->aux_outputs);
    // a description read again drops the bindings of a side whose auxiliary ports moved: the blocks are bound again
    else if (aux_side_bind(effect, true, effect->aux_frames) != 0 || aux_side_bind(effect, false, effect->aux_frames) != 0)
        fprintf(stderr, "%s: its auxiliary ports refuse their blocks after the ports rescan; it passes dry\n", clap->desc->id);
    else if ((effect->aux_in_count || effect->aux_out_count) && effect->aux_frames < jack_get_buffer_size(effect->jack_client))
    {
        // the buffer size grew and its callback has not made the blocks yet: an activation now is refused for them, the
        // callback activates once it has
        effect->pending.set = 1;
        effect->pending.thread = thread;
        effect->pending.held = held;
    }
    else if (omx_clap_host_activate(clap, jack_get_sample_rate(effect->jack_client), jack_get_buffer_size(effect->jack_client), why) != 0)
        fprintf(stderr, "%s: it does not activate again after the ports rescan: %s; it passes dry\n", clap->desc->id, why);
    else
    {
        // unpublish and stop released the audio role: the process thread's is given back
        if (held)
            omx_clap_host_set_audio_thread(clap, thread);
        omx_clap_host_arm(clap);
    }
    unchanged = clap->in_channels == in_channels && clap->channels == channels && clap->main_in.kind == in_kind
                && clap->main_out.kind == out_kind;
    effect->relayouts_unchanged = unchanged ? effect->relayouts_unchanged + 1 : 0;
    omx_cycle_gate_open(&effect->gate);
    pthread_mutex_unlock(&g_reactivate);
}

/* the control thread's tick of one effect: what the plugin asked of the host, its log, a block size JACK changed to
 * (resize_apply), a restart, the writes no cycle took. A restart or a resize stopped by a ports rescan is the relayout's
 * to answer (ports_relayout, right after, which so sees the new size); a failed restart is said on stderr */
static void clap_idle(effect_t *effect)
{
    struct omx_clap_instance *clap = effect->clap;
    const int restart = omx_clap_host_tick(clap);
    const char *log = omx_clap_host_log_take(clap);

    if (log)
        fprintf(stderr, "%s: %s\n", clap->desc->id, log);
    resize_apply(effect);
    if (restart && clap->active)
    {
        int restarted;

        // the cycles keep running: they stop the stage, then are shut out across the deactivate (omx_cycle_gate_restart),
        // and a buffer-size re-activation waits
        pthread_mutex_lock(&g_reactivate);
        restarted = omx_cycle_gate_restart(&effect->gate, clap, CLAP_HOST_ROLE_POLL_US, CLAP_HOST_ROLE_TIMEOUT_US);
        pthread_mutex_unlock(&g_reactivate);
        if (restarted == OMX_CLAP_RESTART_RESCANNED)
            effect->relayout_pending = 1;
        else if (restarted != 0)
            fprintf(stderr, "%s: its restart failed\n", clap->desc->id);
    }
    omx_clap_host_settle(clap, QUEUE_SETTLE_US);
}

static int parse_param_id(const char *control_symbol, clap_id *id)
{
    char *end;
    unsigned long value;

    if (!control_symbol || control_symbol[0] < '0' || control_symbol[0] > '9')
        return -1;
    value = strtoul(control_symbol, &end, 10);
    if (*end != '\0' || value >= CLAP_INVALID_ID)
        return -1;
    *id = (clap_id)value;
    return 0;
}

/* the symbol an output answers to: the name with every character outside [A-Za-z0-9_] mapped to '_', a '_' before a
 * leading digit, and _<channel> from 0 when the meter has more than one channel (`channel` -1 when it has one) */
static void output_symbol(char *symbol, size_t size, const char *name, int channel)
{
    size_t i, n = 0;

    if (name[0] >= '0' && name[0] <= '9')
        symbol[n++] = '_';
    for (i = 0; name[i] != '\0' && n + 1 < size; i++)
    {
        const char c = name[i];
        const int keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';

        symbol[n++] = keep ? c : '_';
    }
    symbol[n] = '\0';
    if (channel >= 0)
        snprintf(symbol + n, size - n, "_%d", channel);
}

/* whatever else answers to `symbol` on this instance: a parameter, by its id or :bypass, or an output derived already */
static int symbol_owner(const effect_t *effect, const char *symbol, char *owner, size_t size)
{
    clap_id id;
    uint32_t i;

    if (strcmp(symbol, BYPASS_PORT_SYMBOL) == 0 ||
        (parse_param_id(symbol, &id) == 0 && omx_clap_host_param_readable(effect->clap, id)))
    {
        snprintf(owner, size, "parameter %s", symbol);
        return 1;
    }
    for (i = 0; i < effect->outputs_count; i++)
    {
        const output_t *output = &effect->outputs[i];

        if (strcmp(output->symbol, symbol) != 0)
            continue;
        if (output->gain_adjustment)
            snprintf(owner, size, "the gain adjustment");
        else
            snprintf(owner, size, "meter %u \"%s\" channel %u", output->meter, output->name, output->channel);
        return 1;
    }
    return 0;
}

static int output_add(effect_t *effect, const char *name, int gain_adjustment, clap_id meter, uint32_t channel, uint32_t channels)
{
    char symbol[OUTPUT_SYMBOL_SIZE], owner[CLAP_NAME_SIZE + 64];
    output_t *outputs, *output;

    if (gain_adjustment)
        snprintf(symbol, sizeof(symbol), "%s", GAIN_ADJUSTMENT_SYMBOL);
    else
        output_symbol(symbol, sizeof(symbol), name, channels > 1 ? (int)channel : -1);

    if (symbol_owner(effect, symbol, owner, sizeof(owner)))
    {
        if (gain_adjustment)
            fprintf(stderr, "%s: the gain adjustment derives %s, the symbol of %s\n", effect->clap->desc->id, symbol, owner);
        else
            fprintf(stderr, "%s: meter %u \"%s\" channel %u derives %s, the symbol of %s\n",
                    effect->clap->desc->id, meter, name, channel, symbol, owner);
        return -1;
    }

    outputs = realloc(effect->outputs, (effect->outputs_count + 1) * sizeof(output_t));
    if (!outputs)
        return -1;
    effect->outputs = outputs;
    output = &outputs[effect->outputs_count++];
    memset(output, 0, sizeof(output_t));
    memcpy(output->symbol, symbol, sizeof(symbol));
    snprintf(output->name, sizeof(output->name), "%s", name);
    output->gain_adjustment = gain_adjustment;
    output->meter = meter;
    output->channel = channel;
    output->channels = channels;
    return 0;
}

/* an LV2 plugin's read-only parameter, an output control port, monitored by its lv2:symbol */
static int output_param_add(effect_t *effect, const lv2_param_t *param)
{
    output_t *outputs, *output;

    outputs = realloc(effect->outputs, (effect->outputs_count + 1) * sizeof(output_t));
    if (!outputs)
        return -1;
    effect->outputs = outputs;
    output = &outputs[effect->outputs_count++];
    memset(output, 0, sizeof(output_t));
    snprintf(output->symbol, sizeof(output->symbol), "%s", param->symbol);
    snprintf(output->name, sizeof(output->name), "%s", param->symbol);
    output->param = 1;
    output->meter = param->id;
    output->channels = 1;
    return 0;
}

/* control thread, at load: the plugin's clap.meters/1, else its clap.gain-adjustment-metering/0, each value an output
 * symbol (docs/meters.md). A meter that can't be read or a symbol that is taken refuses the plugin. */
static int outputs_build(effect_t *effect)
{
    const clap_plugin_t *plugin = effect->clap->plugin;
    const clap_plugin_meters_t *meters = plugin->get_extension(plugin, CLAP_EXT_METERS);
    const clap_plugin_gain_adjustment_metering_t *gain_adjustment;
    clap_meter_info_t info;
    uint32_t count, i, c;

    if (meters && meters->count && meters->get_info && meters->get_values)
    {
        effect->meters = meters;
        count = meters->count(plugin);
        for (i = 0; i < count; i++)
        {
            memset(&info, 0, sizeof(info));
            if (!meters->get_info(plugin, i, &info))
            {
                fprintf(stderr, "%s: meter %u has no info\n", effect->clap->desc->id, i);
                return -1;
            }
            info.name[sizeof(info.name) - 1] = '\0';
            if (info.name[0] == '\0' || info.channel_count == 0 || info.channel_count > METER_CHANNELS_MAX)
            {
                fprintf(stderr, "%s: meter %u \"%s\" has %u channels\n", effect->clap->desc->id, info.id, info.name, info.channel_count);
                return -1;
            }
            for (c = 0; c < info.channel_count; c++)
                if (output_add(effect, info.name, 0, info.id, c, info.channel_count) != 0)
                    return -1;
        }
        return 0;
    }

    if (effect->lv2_entry)
    {
        // an LV2 plugin's output control ports, by their symbols, as mod-host monitors them
        for (i = 0; i < effect->lv2_params_count; i++)
            if ((effect->lv2_params[i].flags & CLAP_PARAM_IS_READONLY) && output_param_add(effect, &effect->lv2_params[i]) != 0)
                return -1;
        return 0;
    }

    gain_adjustment = plugin->get_extension(plugin, CLAP_EXT_GAIN_ADJUSTMENT_METERING);
    if (gain_adjustment && gain_adjustment->get)
    {
        if (output_add(effect, GAIN_ADJUSTMENT_SYMBOL, 1, CLAP_INVALID_ID, 0, 1) != 0)
            return -1;
        effect->gain_adjustment = gain_adjustment;
    }
    return 0;
}

static int floats_differ(float a, float b)
{
    return fabsf(a - b) >= FLT_EPSILON;
}

/* the feedback words for what a plugin does with its parameters by itself: mod-host's param_set line for a value it
 * moved, and param_gesture <N> <param> begin|end for its user's hold of a control, the shape FreeMixer/plugin-hostd#47
 * proposes for plugin-hostd's vocabulary */
#define PARAM_SET_FEEDBACK      "param_set %i %s %f"
#define PARAM_GESTURE_FEEDBACK  "param_gesture %i %s %s"
/* the parameter events taken per batch, and the values one batch holds back to send the last of each */
#define PARAM_EVENTS_BATCH      64

/* what a feedback line names a parameter by: an LV2 control port's symbol, else the CLAP id in decimal */
static const char *param_name(const effect_t *effect, clap_id id, char *buf, size_t size)
{
    const lv2_param_t *lv2 = effect->lv2_entry ? lv2_param_by_id(effect, id) : NULL;

    if (lv2 && lv2->symbol)
        return lv2->symbol;
    snprintf(buf, size, "%u", id);
    return buf;
}

static void param_set_feedback(const effect_t *effect, clap_id id, double value)
{
    char line[128], name[16];

    snprintf(line, sizeof(line), PARAM_SET_FEEDBACK, effect->instance, param_name(effect, id, name, sizeof(name)), value);
    socket_send_feedback(line);
}

static void param_gesture_feedback(const effect_t *effect, clap_id id, int begin)
{
    char line[128], name[16];

    snprintf(line, sizeof(line), PARAM_GESTURE_FEEDBACK, effect->instance, param_name(effect, id, name, sizeof(name)),
             begin ? "begin" : "end");
    socket_send_feedback(line);
}

/* a gesture of the plugin as the feedback port carries it: a begin and its end, always in pairs. A begin opens the
 * parameter's gesture (a second begin on one already open says nothing more), an end closes it; an end with no open
 * gesture says nothing, since the begin it answers never went out or its end did already. A begin that cannot be
 * kept for want of memory is not sent, so its end is not either. */
static void param_gesture_notify(effect_t *effect, clap_id id, int begin)
{
    uint32_t j;

    for (j = 0; j < effect->gestures_open_count && effect->gestures_open[j] != id; j++)
        ;
    if (begin)
    {
        if (j < effect->gestures_open_count)
            return;
        if (effect->gestures_open_count == effect->gestures_open_cap)
        {
            const uint32_t cap = effect->gestures_open_cap ? 2u * effect->gestures_open_cap : 8u;
            clap_id *grown = realloc(effect->gestures_open, cap * sizeof(*grown));

            if (!grown)
                return;
            effect->gestures_open = grown;
            effect->gestures_open_cap = cap;
        }
        effect->gestures_open[effect->gestures_open_count++] = id;
    }
    else
    {
        if (j == effect->gestures_open_count)
            return;
        effect->gestures_open[j] = effect->gestures_open[--effect->gestures_open_count];
    }
    param_gesture_feedback(effect, id, begin);
}

/* control thread, at the idle rate: the values the plugin moved by itself, the last of each parameter per batch, and its
 * gestures, in order (a value held back goes out before a gesture on the same parameter). Only a row is reported: a
 * read-only parameter is an output, monitor_output's. When the library dropped events for want of room, every row's
 * value goes out as the plugin reads it now, so the controller misses no final value, and every gesture still open is
 * ended, so the controller is not left holding one whose end was among the dropped (a gesture the plugin still holds
 * reads as let go; the plugin's next begin opens it again). */
static void param_events_notify(effect_t *effect)
{
    struct omx_clap_param_event ev[PARAM_EVENTS_BATCH];
    struct { clap_id id; double value; } held[PARAM_EVENTS_BATCH];
    const uint32_t dropped = omx_clap_host_param_events_dropped(effect->clap);
    uint32_t n, i, j, k;

    while ((n = omx_clap_host_param_events_take(effect->clap, ev, PARAM_EVENTS_BATCH)) > 0)
    {
        k = 0;
        for (i = 0; i < n; i++)
        {
            if (!omx_clap_host_param_is_row(effect->clap, ev[i].id))
                continue;
            if (ev[i].kind == OMX_CLAP_PARAM_EVENT_VALUE)
            {
                for (j = 0; j < k && held[j].id != ev[i].id; j++)
                    ;
                held[j].id = ev[i].id;
                held[j].value = ev[i].value;
                k += j == k;
                continue;
            }
            for (j = 0; j < k && held[j].id != ev[i].id; j++)
                ;
            if (j < k)
            {
                param_set_feedback(effect, held[j].id, held[j].value);
                held[j] = held[--k];
            }
            param_gesture_notify(effect, ev[i].id, ev[i].kind == OMX_CLAP_PARAM_EVENT_GESTURE_BEGIN);
        }
        for (j = 0; j < k; j++)
            param_set_feedback(effect, held[j].id, held[j].value);
    }
    if (dropped != effect->param_events_dropped)
    {
        const uint32_t count = omx_clap_host_param_count(effect->clap);
        struct omx_clap_param_row row;
        double value;

        effect->param_events_dropped = dropped;
        for (i = 0; i < count; i++)
            if (omx_clap_host_param_row(effect->clap, i, &row) == 0 && omx_clap_host_param_read(effect->clap, row.id, &value) == 0)
                param_set_feedback(effect, row.id, value);
        while (effect->gestures_open_count > 0)
            param_gesture_notify(effect, effect->gestures_open[effect->gestures_open_count - 1], 0);
    }
}

/* control thread, at the idle rate: each monitored output whose value moved, and the first value of one just monitored,
 * goes out as output_set on the feedback socket; a move is mod-host's for an output port, FLT_EPSILON or more */
static void outputs_notify(effect_t *effect)
{
    double values[METER_CHANNELS_MAX];
    clap_id read_meter = CLAP_INVALID_ID;
    uint32_t have = 0;
    int read = 0;
    uint32_t i;

    for (i = 0; i < effect->outputs_count; i++)
    {
        output_t *output = &effect->outputs[i];
        float value;

        if (!output->monitored)
            continue;
        if (output->gain_adjustment)
        {
            const uint32_t bits = atomic_load_explicit(&effect->gain_adjustment_bits, memory_order_relaxed);

            memcpy(&value, &bits, sizeof(value));
        }
        else if (output->param)
        {
            double v;

            if (omx_clap_host_param_read(effect->clap, output->meter, &v) != 0)
                continue;
            value = (float)v;
        }
        else
        {
            if (!read || output->meter != read_meter)
            {
                read = 1;
                read_meter = output->meter;
                have = effect->meters->get_values(effect->clap->plugin, output->meter, values, output->channels);
            }
            if (output->channel >= have)
                continue;
            value = (float)values[output->channel];
        }
        if (output->notified && !floats_differ(output->value, value))
            continue;
        if (host_dispatch_output_set(effect->instance, output->symbol, value) < 0)
            continue;
        output->value = value;
        output->notified = 1;
    }
}

static void state_filename(char *buffer, size_t size, const char *dir, int instance)
{
    snprintf(buffer, size, "%s/effect_%i%s", dir, instance, STATE_FILE_SUFFIX);
}


/* one-contract 10.1 step 3: the layout of an instance after init and before activate against its pin; 0 when it
 * matches, or when no pin was expected */
static int layout_check(effect_t *effect, const char *pin, const char *id)
{
    char hex[PHD_SHA256_HEX_LEN + 1];

    if (!pin[0])
        return 0;
    if (layout_pin_write(effect->clap->plugin, effect->clap->params, NULL, NULL, hex) != 0)
    {
        fprintf(stderr, "%s: its parameter layout cannot be read\n", id);
        return -1;
    }
    if (strcmp(hex, pin) != 0)
    {
        fprintf(stderr, "%s: layout %s:%s, pinned %s:%s\n", id, PHD_PIN_LAYOUT_SCHEME, hex, PHD_PIN_LAYOUT_SCHEME, pin);
        return -1;
    }
    return 0;
}

/* `in_ports` main input and `out_ports` main output ports registered on `client` into `inputs` and `outputs`: SUCCESS
 * or ERR_JACK_PORT_REGISTER. Before jack_activate: no cycle runs. */
static int main_ports_register_on(jack_client_t *client, uint32_t in_ports, uint32_t out_ports, jack_port_t **inputs,
                                  jack_port_t **outputs)
{
    char port_name[LAYOUT_PORT_NAME_SIZE];
    uint32_t c;

    for (c = 0; c < in_ports; c++)
    {
        layout_port_name(port_name, true, LAYOUT_PORT_MAIN, c);
        inputs[c] = jack_port_register(client, port_name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
        if (!inputs[c])
            return ERR_JACK_PORT_REGISTER;
    }
    for (c = 0; c < out_ports; c++)
    {
        layout_port_name(port_name, false, LAYOUT_PORT_MAIN, c);
        outputs[c] = jack_port_register(client, port_name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        if (!outputs[c])
            return ERR_JACK_PORT_REGISTER;
    }
    return SUCCESS;
}

/* the instance's main ports registered on its client, as many each side as were registered at add */
static int main_ports_register(effect_t *effect)
{
    return main_ports_register_on(effect->jack_client, effect->in_ports, effect->out_ports, effect->input_ports,
                                  effect->output_ports);
}

/* the callbacks of the instance's own client, before it is activated: its role reads its own slot, which names no
 * thread until the client's thread-init callback stores its own (jack_thread_init) */
static void callbacks_set(effect_t *effect)
{
    atomic_store_explicit(&effect->thread_named, (pthread_t)0, memory_order_relaxed);
    omx_clap_host_name_slot(effect->clap, &effect->thread_named);
    jack_set_thread_init_callback(effect->jack_client, jack_thread_init, effect);
    jack_set_process_callback(effect->jack_client, process, effect);
    jack_set_buffer_size_callback(effect->jack_client, buffer_size, effect);
    jack_set_latency_callback(effect->jack_client, latency, effect);
}

/* a client of its own made ready for a chain's member, under the name the instance had: opened and its main and note
 * ports registered, but neither activated nor the instance's yet, so the head's cycle may still run the member
 * meanwhile. A member has no auxiliary port (effects_chain). SUCCESS, or the error with nothing left open. */
static int client_ready(const effect_t *effect, own_client_t *own)
{
    int error;

    memset(own, 0, sizeof(*own));
    own->client = open_jack_client(effect->instance, effect->client_name, NULL, 0);
    if (!own->client)
        return ERR_JACK_CLIENT_CREATION;
    error = main_ports_register_on(own->client, effect->in_ports, effect->out_ports, own->inputs, own->outputs);
    if (error == SUCCESS && effect->clap->note_inputs
        && !(own->midi_in = jack_port_register(own->client, MIDI_PORT_NAME, JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0)))
        error = ERR_JACK_PORT_REGISTER;
    if (error == SUCCESS && effect->clap->note_outputs
        && !(own->midi_out = jack_port_register(own->client, MIDI_OUT_PORT_NAME, JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0)))
        error = ERR_JACK_PORT_REGISTER;
    if (error != SUCCESS)
    {
        jack_client_close(own->client);
        own->client = NULL;
    }
    return error;
}

/* a ready client given up unused */
static void client_drop(own_client_t *own)
{
    if (own->client)
        jack_client_close(own->client);
    own->client = NULL;
}

/* a ready client made the instance's, which no walk runs any more: its ports and callbacks, then activated and its
 * latency published. SUCCESS, or ERR_JACK_CLIENT_ACTIVATION with that client closed and the instance in none. */
static int client_take(effect_t *effect, own_client_t *own)
{
    effect->jack_client = own->client;
    own->client = NULL;
    memcpy(effect->input_ports, own->inputs, sizeof(effect->input_ports));
    memcpy(effect->output_ports, own->outputs, sizeof(effect->output_ports));
    effect->midi_port = own->midi_in;
    effect->midi_out_port = own->midi_out;
    callbacks_set(effect);
    if (jack_activate(effect->jack_client) != 0)
    {
        jack_client_close(effect->jack_client);
        effect->jack_client = NULL;
        effect->midi_port = effect->midi_out_port = NULL;
        memset(effect->input_ports, 0, sizeof(effect->input_ports));
        memset(effect->output_ports, 0, sizeof(effect->output_ports));
        return ERR_JACK_CLIENT_ACTIVATION;
    }
    publish_latency(effect);
    return SUCCESS;
}

/* a client of its own again for a member no walk runs any more: `own` when one was made ready, else one made now.
 * SUCCESS, or the error with the instance freed, since it has no client left to run in. */
static int client_back(effect_t *effect, own_client_t *own)
{
    own_client_t made;
    int error = SUCCESS;

    if (!own)
    {
        own = &made;
        error = client_ready(effect, own);
    }
    if (error == SUCCESS && (error = client_take(effect, own)) == SUCCESS)
        return SUCCESS;
    fprintf(stderr, "effect_%d: no client of its own can be made again (%d); it is removed\n", effect->instance, error);
    instance_free(effect);
    return error;
}

/* control thread: a chain as it stands, on stderr, after every change: the read-back of a chain (docs/chain.md) */
static void chain_say(const chain_t *chain)
{
    struct omx_clap_chain_report report;
    const uint32_t count = atomic_load_explicit(&chain->count, memory_order_relaxed);
    uint32_t i;

    omx_clap_chain_report_init(&report);
    omx_clap_chain_report(chain->walk, &report);
    fprintf(stderr, "chain %d:", chain->id);
    for (i = 0; i < count; i++)
        fprintf(stderr, " effect_%d", atomic_load_explicit(&chain->members[i], memory_order_relaxed)->instance);
    fprintf(stderr, " in effect_%d's client, latency %u frames, walk %u\n", chain->head->instance, report.latency,
            report.revision);
}

/* the library's latency_changed, on the control thread: the head's ports answer with the members' new sum */
static void chain_latency_changed(void *ctx, uint32_t frames)
{
    chain_t *chain = ctx;

    atomic_store_explicit(&chain->head->latency_run, frames, memory_order_relaxed);
    jack_recompute_total_latencies(chain->head->jack_client);
}

/* `effect` into the chain's members at `position` and on the head's thread, then into the walk there, a stage of the
 * host's own (chain_stage): when this returns the head's next cycle runs it. The head's thread is read and the member's
 * set under g_chain_members, where the head's thread-init callback sets the head's and walks the members
 * (jack_thread_init), so a thread the head gets meanwhile reaches this member too. 0, or the library's refusal, the
 * members as they were. */
static int chain_insert(chain_t *chain, effect_t *effect, uint32_t position)
{
    struct omx_clap_chain_member member;
    const uint32_t count = atomic_load_explicit(&chain->count, memory_order_relaxed);
    const effect_t *head = chain->head;
    uint32_t i;
    int ret;

    omx_clap_chain_member_init(&member);
    member.process = chain_stage;
    member.ctx = effect;
    member.in_channels = effect->in_ports;
    member.out_channels = effect->out_ports;
    member.latency = effect->latency_published;
    if (position > count)
        return OMX_CLAP_CHAIN_ERR_ARG;
    if (count == OMX_CLAP_CHAIN_MEMBERS_MAX)
        return OMX_CLAP_CHAIN_ERR_FULL;
    // a member runs on its head's thread: its role reads the head's slot, so the thread the head names, now or later,
    // is its own before the walk first runs it
    if (effect != head)
        omx_clap_host_name_slot(effect->clap, &head->thread_named);
    pthread_mutex_lock(&g_chain_members);
    for (i = count; i > position; i--)
        atomic_store_explicit(&chain->members[i], atomic_load_explicit(&chain->members[i - 1], memory_order_relaxed),
                              memory_order_relaxed);
    atomic_store_explicit(&chain->members[position], effect, memory_order_relaxed);
    atomic_store_explicit(&chain->count, count + 1, memory_order_relaxed);
    pthread_mutex_unlock(&g_chain_members);
    effect->chain = chain;
    ret = omx_clap_chain_add(chain->walk, &member, position, &effect->chain_member);
    if (ret == OMX_CLAP_CHAIN_OK)
        return 0;
    effect->chain = NULL;
    if (effect != head)
        omx_clap_host_name_slot(effect->clap, &effect->thread_named);
    pthread_mutex_lock(&g_chain_members);
    for (i = position; i < count; i++)
        atomic_store_explicit(&chain->members[i], atomic_load_explicit(&chain->members[i + 1], memory_order_relaxed),
                              memory_order_relaxed);
    atomic_store_explicit(&chain->members[count], NULL, memory_order_relaxed);
    atomic_store_explicit(&chain->count, count, memory_order_relaxed);
    pthread_mutex_unlock(&g_chain_members);
    return ret;
}

/* the chain's members put in `order`, the head first and every member once: ONE walk the library publishes
 * (omx_clap_chain_order), so no cycle runs without one of them or in an order between the old one and this one; then
 * the members as the walk runs them. 0, or the library's refusal, nothing changed. */
static int chain_order(chain_t *chain, effect_t *const *order, uint32_t n)
{
    uint32_t ids[OMX_CLAP_CHAIN_MEMBERS_MAX], i;
    int ret;

    for (i = 0; i < n; i++)
        ids[i] = order[i]->chain_member;
    ret = omx_clap_chain_order(chain->walk, ids, n);
    if (ret != OMX_CLAP_CHAIN_OK)
        return ret;
    pthread_mutex_lock(&g_chain_members);
    for (i = 0; i < n; i++)
        atomic_store_explicit(&chain->members[i], order[i], memory_order_relaxed);
    pthread_mutex_unlock(&g_chain_members);
    return 0;
}

/* `effect` out of the walk, the chain closing around it (when the library's call returns no cycle runs it), and out of
 * the chain's members. Its client is still the head's: the caller gives it one or frees it. */
static void chain_extract(chain_t *chain, effect_t *effect)
{
    const uint32_t count = atomic_load_explicit(&chain->count, memory_order_relaxed);
    uint32_t i, k;

    omx_clap_chain_remove(chain->walk, effect->chain_member);
    pthread_mutex_lock(&g_chain_members);
    for (i = 0, k = 0; i < count; i++)
    {
        effect_t *member = atomic_load_explicit(&chain->members[i], memory_order_relaxed);

        if (member != effect)
            atomic_store_explicit(&chain->members[k++], member, memory_order_relaxed);
    }
    atomic_store_explicit(&chain->count, k, memory_order_relaxed);
    atomic_store_explicit(&chain->members[k], NULL, memory_order_relaxed);
    pthread_mutex_unlock(&g_chain_members);
    effect->chain = NULL;
}

/* an instance whose own client is deactivated (its cycles stopped, member_stop), or that has none (a member moving
 * from one walk to another, chain_rehead), into `chain` at `position`: the client closed, so it is no node of the graph
 * and has no port, then it runs in the head's client, on the head's thread. SUCCESS, or, when the library refuses it
 * (effects_chain checks first what it refuses), the error with the instance in a client of its own again or, when that
 * fails too, freed. */
static int chain_join(chain_t *chain, effect_t *effect, uint32_t position)
{
    int ret;

    if (effect->jack_client)
        jack_client_close(effect->jack_client);
    effect->jack_client = chain->head->jack_client;
    memset(effect->input_ports, 0, sizeof(effect->input_ports));
    memset(effect->output_ports, 0, sizeof(effect->output_ports));
    // a member's note ports went with its client: its note verbs reach it through the ring, midi_in is never read, and
    // what it sends out is dropped
    effect->midi_port = effect->midi_out_port = NULL;
    ret = chain_insert(chain, effect, position);
    if (ret == 0)
        return SUCCESS;
    fprintf(stderr, "chain %d: effect_%d cannot join it (%d)\n", chain->id, effect->instance, ret);
    effect->jack_client = NULL;
    return client_back(effect, NULL) == SUCCESS ? ERR_INVALID_OPERATION : ERR_JACK_CLIENT_CREATION;
}

/* the cycles of a member-to-be's own client stopped: 0, or JACK's refusal with the client as it was */
static int member_stop(effect_t *effect)
{
    return jack_deactivate(effect->jack_client);
}

/* a stopped client's links given up: run again (members_start) or closed as it joins */
static void links_forget(stopped_t *stopped)
{
    uint32_t k;

    for (k = 0; k < 2 * stopped->n_links; k++)
        free(stopped->links[k]);
    free(stopped->links);
    stopped->links = NULL;
    stopped->n_links = 0;
}

/* the links of `effect`'s main ports, as they are before its client stops, into `stopped`: 0, or -1 with none kept when
 * there is no memory for them */
static int links_record(effect_t *effect, stopped_t *stopped)
{
    jack_port_t *ports[2 * CLAP_HOST_PORT_CHANNELS];
    uint32_t n_ports = 0, p;

    stopped->effect = effect;
    stopped->links = NULL;
    stopped->n_links = 0;
    for (p = 0; p < effect->in_ports; p++)
        ports[n_ports++] = effect->input_ports[p];
    for (p = 0; p < effect->out_ports; p++)
        ports[n_ports++] = effect->output_ports[p];
    for (p = 0; p < n_ports; p++)
    {
        const char **peers = jack_port_get_all_connections(effect->jack_client, ports[p]);
        const int output = (jack_port_flags(ports[p]) & JackPortIsOutput) != 0;
        size_t i;

        for (i = 0; peers && peers[i]; i++)
        {
            char **grown = realloc(stopped->links, 2 * (stopped->n_links + 1) * sizeof(*grown));

            if (!grown)
            {
                jack_free(peers);
                links_forget(stopped);
                return -1;
            }
            stopped->links = grown;
            grown[2 * stopped->n_links] = strdup(output ? jack_port_name(ports[p]) : peers[i]);
            grown[2 * stopped->n_links + 1] = strdup(output ? peers[i] : jack_port_name(ports[p]));
            stopped->n_links++;
            if (!grown[2 * stopped->n_links - 2] || !grown[2 * stopped->n_links - 1])
            {
                jack_free(peers);
                links_forget(stopped);
                return -1;
            }
        }
        jack_free(peers);
    }
    return 0;
}

/* the clients member_stop stopped, stopped[0 .. n), run again, the last first, each with the links its ports had
 * (links_record): a client JACK will not run again, or a link it will not make again, said on stderr */
static void members_start(int id, stopped_t *stopped, uint32_t n)
{
    while (n > 0)
    {
        stopped_t *again = &stopped[--n];
        jack_client_t *client = again->effect->jack_client;
        uint32_t k;

        if (jack_activate(client) != 0)
            fprintf(stderr, "chain %d: effect_%d's own client cannot be run again; its %u link(s) are lost\n", id,
                    again->effect->instance, again->n_links);
        else
            for (k = 0; k < again->n_links; k++)
            {
                const int ret = jack_connect(client, again->links[2 * k], again->links[2 * k + 1]);

                if (ret != 0 && ret != EEXIST)
                    fprintf(stderr, "chain %d: effect_%d's link %s -> %s cannot be made again (%d)\n", id,
                            again->effect->instance, again->links[2 * k], again->links[2 * k + 1], ret);
            }
        links_forget(again);
    }
}

/* the clients member_stop stopped, stopped[0 .. n), kept stopped: their links given up, as the clients close when they
 * join */
static void members_keep_stopped(stopped_t *stopped, uint32_t n)
{
    while (n > 0)
        links_forget(&stopped[--n]);
}

/* the cycles stopped of every instance a chain line names after its head that is in no chain, so runs in a client of
 * its own (`chain`'s head, which a line with another head may name as a member, is chain_rehead's to stop, last), each
 * one's links recorded first. SUCCESS with them in stopped[0 .. *n_stopped), for members_start or members_keep_stopped;
 * or, when JACK will not stop one (or its links cannot be recorded), ERR_JACK_CLIENT_DEACTIVATION with the ones stopped
 * before it running again with their links */
static int members_stop(int id, const chain_t *chain, const int *instances, int n, stopped_t *stopped,
                        uint32_t *n_stopped)
{
    int i;

    *n_stopped = 0;
    for (i = 1; i < n; i++)
    {
        effect_t *effect = &g_effects[instances[i]];

        if (chain && effect->chain == chain)
            continue;
        if (links_record(effect, &stopped[*n_stopped]) != 0)
            fprintf(stderr, "chain %d: effect_%d's links cannot be recorded; its own client is not stopped\n", id,
                    effect->instance);
        else if (member_stop(effect) != 0)
        {
            fprintf(stderr, "chain %d: effect_%d's own client cannot be stopped\n", id, effect->instance);
            links_forget(&stopped[*n_stopped]);
        }
        else
        {
            (*n_stopped)++;
            continue;
        }
        members_start(id, stopped, *n_stopped);
        *n_stopped = 0;
        return ERR_JACK_CLIENT_DEACTIVATION;
    }
    return SUCCESS;
}

static void chain_reload(chain_t *chain, effect_t *const *out, const uint32_t *place, uint32_t n);

/* the members out[0 .. n) of `chain`, none of them its head, out of the walk one at a time, each into the client made
 * ready for it in own[k] and that client activated: all of them, SUCCESS; or none. When JACK will not activate one,
 * that one and every one moved before it go back into the walk at the places they left, last moved first, in the
 * head's client as before (the clients they were given closed), the clients made for the rest are closed, and the answer
 * is ERR_JACK_CLIENT_ACTIVATION: the chain's members and order as they were, no instance freed. place[k] is the place
 * out[k] left, for chain_reload. */
static int chain_unload(chain_t *chain, effect_t *const *out, own_client_t *own, uint32_t n, uint32_t *place)
{
    uint32_t count, k, i;

    for (k = 0; k < n; k++)
    {
        count = atomic_load_explicit(&chain->count, memory_order_relaxed);
        for (place[k] = 1; place[k] < count && atomic_load_explicit(&chain->members[place[k]], memory_order_relaxed) != out[k];
             place[k]++)
            ;
        chain_extract(chain, out[k]);
        out[k]->jack_client = NULL;
        if (client_take(out[k], &own[k]) == SUCCESS)
            continue;
        fprintf(stderr, "chain %d: effect_%d's own client cannot be activated; every member goes back to its place\n",
                chain->id, out[k]->instance);
        chain_reload(chain, out, place, k + 1);
        for (i = k + 1; i < n; i++)
            client_drop(&own[i]);
        return ERR_JACK_CLIENT_ACTIVATION;
    }
    return SUCCESS;
}

/* chain_unload undone for out[0 .. n), the last moved first: each one's own client closed (client_take closed a
 * refused one already) and the member back at place[k], the place it left, in the head's client: the walk as it was */
static void chain_reload(chain_t *chain, effect_t *const *out, const uint32_t *place, uint32_t n)
{
    while (n-- > 0)
    {
        if (out[n]->jack_client)
            jack_client_close(out[n]->jack_client);
        out[n]->jack_client = chain->head->jack_client;
        memset(out[n]->input_ports, 0, sizeof(out[n]->input_ports));
        memset(out[n]->output_ports, 0, sizeof(out[n]->output_ports));
        // the place it left a moment ago, in a walk of the same width: the library takes it back
        (void)chain_insert(chain, out[n], place[n]);
    }
}

/* the clients of their own the members out[0 .. n) of `chain` go back to, made, then the members moved into them
 * (chain_unload): SUCCESS with every one in its own client, or the error with the chain as it was and no instance freed */
static int chain_clients_back(chain_t *chain, effect_t *const *out, uint32_t n)
{
    own_client_t own[OMX_CLAP_CHAIN_MEMBERS_MAX];
    uint32_t place[OMX_CLAP_CHAIN_MEMBERS_MAX], k;
    int error;

    for (k = 0; k < n; k++)
        if ((error = client_ready(out[k], &own[k])) != SUCCESS)
        {
            fprintf(stderr, "chain %d: no client of its own can be made for effect_%d (%d); nothing changed\n", chain->id,
                    out[k]->instance, error);
            while (k > 0)
                client_drop(&own[--k]);
            return error;
        }
    return chain_unload(chain, out, own, n, place);
}

/* a free slot for a chain, NULL when this host runs CHAINS_MAX already */
static chain_t *chain_slot_free(void)
{
    uint32_t i;

    for (i = 0; i < CHAINS_MAX; i++)
        if (g_chains[i].id < 0)
            return &g_chains[i];
    return NULL;
}

/* the library's chain for `head`, to run in the slot `chain`: made before anything changes, since it is the one step of
 * forming a chain that can fail. SUCCESS, or ERR_MEMORY_ALLOCATION said on stderr. */
static int chain_walk_make(chain_t *chain, int id, const effect_t *head, struct omx_clap_chain **walk)
{
    struct omx_clap_chain_config config;

    omx_clap_chain_config_init(&config);
    config.channels = head->out_ports;
    config.latency_changed = chain_latency_changed;
    config.ctx = chain;
    if (omx_clap_chain_create(&config, walk) == OMX_CLAP_CHAIN_OK)
        return SUCCESS;
    fprintf(stderr, "chain %d: the library's chain cannot be made for effect_%d\n", id, head->instance);
    return ERR_MEMORY_ALLOCATION;
}

/* a chain of `head` alone in the free slot `chain`, on the library's chain chain_walk_make made for it: the head its
 * first member (an empty walk as wide as the head takes it), then run by the head's cycle, switched with the chain gate
 * closed (a cycle that finds it closed runs the head alone, the same) */
static void chain_start(chain_t *chain, int id, effect_t *head, struct omx_clap_chain *walk)
{
    chain->walk = walk;
    chain->head = head;
    chain->id = id;
    atomic_store_explicit(&chain->count, 0, memory_order_relaxed);
    (void)chain_insert(chain, head, 0);
    omx_cycle_gate_close(&head->chain_gate, CLAP_HOST_ROLE_POLL_US);
    atomic_store_explicit(&head->chain_run, chain, memory_order_relaxed);
    omx_cycle_gate_open(&head->chain_gate);
}

/* a chain whose members have all left, its head alone: the head's cycle runs it alone again (the gate closed around
 * the switch), its own latency between its ports, and the chain freed */
static void chain_end(chain_t *chain)
{
    effect_t *head = chain->head;

    omx_cycle_gate_close(&head->chain_gate, CLAP_HOST_ROLE_POLL_US);
    atomic_store_explicit(&head->chain_run, NULL, memory_order_relaxed);
    omx_cycle_gate_open(&head->chain_gate);
    omx_clap_chain_destroy(chain->walk);
    head->chain = NULL;
    fprintf(stderr, "chain %d: ended, effect_%d alone in its client\n", chain->id, head->instance);
    pthread_mutex_lock(&g_chain_members);
    memset(chain, 0, sizeof(*chain));
    chain->id = -1;
    pthread_mutex_unlock(&g_chain_members);
    publish_latency(head);
}

/* a member that is not the head out of its chain and freed: `remove` of it */
static void chain_drop(effect_t *effect)
{
    chain_t *chain = effect->chain;

    chain_extract(chain, effect);
    effect->jack_client = NULL;
    instance_free(effect);
    chain_say(chain);
}

/* a chain line whose head is not `chain`'s (effects_chain, its checks done), everything that can fail before the first
 * change: the clients of their own for the members the line leaves out and for the new head when it is a member now
 * made, the library's chain for the new head made, the instances the line names that run in clients of their own
 * stopped, the members going to clients of their own moved into them, all or none (chain_unload), and last the old
 * head's client stopped when the line names it as a member (undoing the move when JACK will not stop it, chain_reload).
 * From there nothing fails: the members the line names again move straight from the old walk to the
 * new one, the old head runs alone in its client again (chain_end), and the chain forms in the new head's client, in
 * the slot it had, at the places the line names. SUCCESS; or the error with the chain, its members, its order and every
 * instance as they were. */
static int chain_rehead(chain_t *chain, int id, effect_t *head, const int *instances, int n)
{
    effect_t *out[OMX_CLAP_CHAIN_MEMBERS_MAX], *moving[OMX_CLAP_CHAIN_MEMBERS_MAX];
    stopped_t stopped[OMX_CLAP_CHAIN_MEMBERS_MAX];
    own_client_t own[OMX_CLAP_CHAIN_MEMBERS_MAX];
    struct omx_clap_chain *walk = NULL;
    effect_t *const old_head = chain->head;
    const uint32_t count = atomic_load_explicit(&chain->count, memory_order_relaxed);
    uint32_t place[OMX_CLAP_CHAIN_MEMBERS_MAX], j, n_out = 0, n_moving = 0, n_stopped = 0;
    int i, error, named_old_head = 0;

    for (i = 1; i < n; i++)
        named_old_head |= &g_effects[instances[i]] == old_head;

    // the last first, as a chain ends: a member the line names again after its head moves straight, the new head and
    // the members it leaves out go to clients of their own
    for (j = count; j-- > 1;)
    {
        effect_t *member = atomic_load_explicit(&chain->members[j], memory_order_relaxed);

        for (i = 1; i < n && &g_effects[instances[i]] != member; i++)
            ;
        if (i < n)
            moving[n_moving++] = member;
        else
            out[n_out++] = member;
    }
    for (j = 0; j < n_out; j++)
        if ((error = client_ready(out[j], &own[j])) != SUCCESS)
        {
            fprintf(stderr, "chain %d: no client of its own can be made for effect_%d (%d); nothing changed\n", id,
                    out[j]->instance, error);
            break;
        }
    if (j < n_out || (error = chain_walk_make(chain, id, head, &walk)) != SUCCESS
        || (error = members_stop(id, chain, instances, n, stopped, &n_stopped)) != SUCCESS)
    {
        omx_clap_chain_destroy(walk);
        while (j > 0)
            client_drop(&own[--j]);
        return error;
    }
    // the old head's client, which runs the chain and carries its connections, stops last, once nothing after it can fail:
    // a client JACK stops loses its connections, so it is never stopped to be started again. The members stopped before
    // run again with the links they had (members_start)
    error = chain_unload(chain, out, own, n_out, place);
    if (error == SUCCESS && named_old_head && member_stop(old_head) != 0)
    {
        fprintf(stderr, "chain %d: effect_%d's own client cannot be stopped\n", id, old_head->instance);
        chain_reload(chain, out, place, n_out);
        error = ERR_JACK_CLIENT_DEACTIVATION;
    }
    if (error != SUCCESS)
    {
        members_start(id, stopped, n_stopped);
        omx_clap_chain_destroy(walk);
        chain_say(chain);
        return error;
    }
    members_keep_stopped(stopped, n_stopped);

    // from here nothing fails: the old walk keeps its head alone, the head's cycle runs it alone again
    for (j = 0; j < n_moving; j++)
    {
        chain_extract(chain, moving[j]);
        moving[j]->jack_client = NULL;
    }
    chain_end(chain);
    chain_start(chain, id, head, walk);
    // the members join at their places in the line's order, each walk the line's order without the ones still to join
    for (i = 1; i < n; i++)
    {
        int ret = chain_join(chain, &g_effects[instances[i]], (uint32_t)i);

        if (ret != SUCCESS)
            error = ret;
    }
    chain_say(chain);
    return error;
}

/*
************************************************************************************************************************
*           GLOBAL FUNCTIONS
************************************************************************************************************************
*/

/* what this host differs in from the defaults: mod-host clamps and scans nothing, a plugin is live on jack_activate, an
 * instrument takes MIDI, and the plugin is told which host it is in */
static int configure_core(void)
{
    struct omx_clap_host_config config;

    omx_clap_host_config_default(&config);
    config.clamp = 0;
    config.nonfinite = 0;
    config.warmup = 0;
    config.note_inputs = 1;
    config.preset_load = 1;
    config.track_info = 1;
    config.remote_controls = 1;
    config.lane_channels = 0;   // a JACK port per main channel: any width is carried (omx_clap_host_run_ports)
    if (g_idle_hold != INT32_MIN)
        config.idle_hold_us = g_idle_hold;
    config.name = "omx-clap-host";
    config.vendor = "Pau Aliagas";
    config.url = "";
    return omx_clap_host_configure(&config);
}

/* the LV2 adapter, once: every feature it provides, the numbers above. lilv is not loaded here, only by the first LV2
 * add, so a host that never adds an LV2 plugin never maps it */
void effects_set_state_root(const char *dir)
{
    g_state_root_given = dir;
}

/* the state root: the one -s names, else $XDG_STATE_HOME/omx-clap-host (an absolute XDG_STATE_HOME only, as the base
 * directory specification says), else $HOME/.local/state/omx-clap-host; NULL when there is none */
static const char *state_root(char *buf, size_t size)
{
    const char *xdg = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
    int n;

    if (g_state_root_given)
        n = snprintf(buf, size, "%s", g_state_root_given);
    else if (xdg && xdg[0] == '/')
        n = snprintf(buf, size, "%s/omx-clap-host", xdg);
    else if (home && home[0] == '/')
        n = snprintf(buf, size, "%s/.local/state/omx-clap-host", home);
    else
        return NULL;
    return n > 0 && (size_t)n < size ? buf : NULL;
}

/* an existing directory this process cannot make entries in is no root; one that does not exist yet the adapter makes */
static int state_root_usable(const char *root)
{
    struct stat st;

    if (root[0] != '/')
        return 0;
    if (stat(root, &st) != 0)
        return errno == ENOENT;
    return S_ISDIR(st.st_mode) && access(root, W_OK | X_OK) == 0;
}

static int configure_lv2_with(const char *root, char why[OMX_CLAP_LV2_WHY_MAX])
{
    omx_clap_lv2_config_t config;

    memset(&config, 0, sizeof(config));
    config.features = omx_clap_lv2_provided;
    config.worker_poll_us = OMX_HOSTED_STAGE_WORKER_POLL_US;
    config.worker_ring_bytes = OMX_HOSTED_STAGE_WORKER_RING_BYTES;
    config.atom_buffer_bytes = LV2_ATOM_BUFFER_BYTES;
    config.latency_hold_ms = CLAP_HOST_LATENCY_HOLD_MS;
    config.lilv_soname = NULL;
    config.state_root = root;
    if (omx_clap_lv2_configure(&config, why) != 0)
        return -1;
    // Under JACK every process() gets the buffer size and any change of it, up or down, re-activates (recorded by
    // buffer_size, applied by the tick's resize_apply, the cycles dry meanwhile), so within one activation the block
    // length is fixed: declared, and the adapter refuses a cycle of another length, counted apart from the plugin's own
    // faults (#192).
    {
        struct omx_lv2_feature_options options;

        omx_lv2_feature_options_init(&options);
        options.block.declares |= OMX_LV2_BLOCK_FIXED;
        return omx_clap_lv2_feature_options(&options, why);
    }
}

/* the adapter with a state root, so state:makePath is offered; a root that cannot be made or written leaves it
 * unoffered, said once, and a plugin that requires it is refused at add (hosting.features.missing) as before */
static void configure_lv2(void)
{
    char why[OMX_CLAP_LV2_WHY_MAX], buf[PATH_MAX];
    const char *root = state_root(buf, sizeof(buf));

    if (root && !state_root_usable(root))
    {
        fprintf(stderr, "the state root %s is not a directory this host can write: state:makePath is not offered\n", root);
        root = NULL;
    }
    g_lv2_ready = configure_lv2_with(root, why) == 0;
    if (!g_lv2_ready && root && strcmp(why, "lv2.configure.state-root") == 0)
    {
        fprintf(stderr, "the state root %s cannot be made (%s): state:makePath is not offered\n", root, why);
        g_lv2_ready = configure_lv2_with(NULL, why) == 0;
    }
    if (!g_lv2_ready)
        fprintf(stderr, "the LV2 adapter refused its configuration: %s; no LV2 plugin can be added\n", why);
}

// JACK's xrun callback, on a thread of its own: it counts and writes nothing, effects_idle writes the line
static int xrun_cb(void *arg)
{
    xrun_report_note(arg);
    return 0;
}

// the thread pool's waits past the block (#395) since the last tick: an xrun-class event, added to the xruns count the
// feedback line carries, and said on stderr; a pool made again starts its count over
static void pool_late_note(void)
{
    struct omx_clap_thread_pool_info info;

    if (g_thread_pool == EFFECTS_THREAD_POOL_OFF)
        return;
    omx_clap_host_thread_pool(&info);
    if (info.late < g_pool_late_seen)
        g_pool_late_seen = 0;
    if (info.late == g_pool_late_seen)
        return;
    fprintf(stderr, "thread pool: %llu waits for a helper past the block, counted as xruns\n",
            (unsigned long long)(info.late - g_pool_late_seen));
    atomic_fetch_add_explicit(&g_xruns.count, info.late - g_pool_late_seen, memory_order_relaxed);
    g_pool_late_seen = info.late;
}

// the xruns line, when the count changed and a second has passed since the last one (xrun_report.h)
static void xruns_feedback(void)
{
    struct timespec now;
    uint64_t count;
    char line[64];

    pool_late_note();
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!xrun_report_due(&g_xruns, (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u, &count))
        return;
    snprintf(line, sizeof(line), HOST_EVENT_XRUNS_FMT, (unsigned long long)count);
    socket_send_feedback(line);
}

int effects_init(void)
{
    uint32_t i;

    if (configure_core() != 0)
    {
        fprintf(stderr, "can't configure the clap core\n");
        return ERR_INSTANCE_INVALID;
    }
    for (i = 0; i < CHAINS_MAX; i++)
        g_chains[i].id = -1;
    configure_lv2();
    window_init();
    g_jack_global_client = jack_client_open("omx-clap-host", JackNoStartServer, NULL);
    if (!g_jack_global_client)
    {
        fprintf(stderr, "can't get jack client\n");
        return ERR_JACK_CLIENT_CREATION;
    }
    // its xrun callback is set before it is active: JACK reports every xrun of the graph to every client
    xrun_report_init(&g_xruns);
    jack_set_xrun_callback(g_jack_global_client, xrun_cb, &g_xruns);
    // active, with no port and no process callback, so its frame clock runs: a transport command is anchored on it at
    // the frame the graph is at, the clock each instance's cycle reads
    if (jack_activate(g_jack_global_client) != 0)
    {
        fprintf(stderr, "can't activate jack client\n");
        return ERR_JACK_CLIENT_ACTIVATION;
    }
    return SUCCESS;
}

int effects_finish(void)
{
    effects_remove(REMOVE_ALL);
    window_finish();
    if (g_jack_global_client)
        jack_client_close(g_jack_global_client);
    g_jack_global_client = NULL;
    return SUCCESS;
}

/* an LV2 plugin's URI: lv2:<uri>, or the URI alone as mod-host takes it; NULL for anything else */
static const char *lv2_uri_of(const char *uri)
{
    if (!uri || strncmp(uri, URI_SCHEME, strlen(URI_SCHEME)) == 0)
        return NULL;
    if (strncmp(uri, LV2_URI_SCHEME, strlen(LV2_URI_SCHEME)) == 0)
        uri += strlen(LV2_URI_SCHEME);
    // an absolute URI: a scheme, then ':' and something after it
    if (!isalpha((unsigned char)uri[0]) || !strchr(uri, ':') || strchr(uri, ':')[1] == '\0')
        return NULL;
    return uri;
}

const char *effects_add_reason(void)
{
    return g_add_reason;
}

/* a device the core could not open is said once, after the add that found it; the plugin runs with no hold */
static void idle_hold_say_once(void)
{
    struct omx_clap_idle_hold hold;

    if (g_idle_hold_said)
        return;
    omx_clap_host_idle_hold(&hold);
    if (hold.code[0] == '\0')
        return;
    fprintf(stderr, "idle hold: %s: /dev/cpu_dma_latency cannot be opened for writing; the plugins run with no hold\n", hold.code);
    g_idle_hold_said = 1;
}

/* a word of the hold: "off", or a non-negative integer, microseconds */
static const char *idle_hold_word_of(int32_t target_us, char *buf, size_t size)
{
    if (target_us == OMX_CLAP_IDLE_HOLD_OFF)
        return "off";
    snprintf(buf, size, "%d", target_us);
    return buf;
}

int effects_add(const char *uri, int instance, const char *client_name)
{
    char path[PATH_MAX];
    char why[OMX_CLAP_WHY_MAX];
    char pin[PHD_SHA256_HEX_LEN + 1];
    const char *id, *lv2_uri;
    effect_t *effect;
    int error;

    g_add_reason[0] = '\0';
    lv2_uri = lv2_uri_of(uri);
    if (lv2_uri)
        id = lv2_uri;
    else if (parse_uri(uri, path, sizeof(path), &id) != 0)
        return ERR_LV2_INVALID_URI;
    if (!INSTANCE_IS_VALID(instance))
        return ERR_INSTANCE_INVALID;
    if (instance_exist(instance))
        return ERR_INSTANCE_ALREADY_EXISTS;

    // a pin is for the one add that follows it
    memcpy(pin, g_layout_pins[instance], sizeof(pin));
    g_layout_pins[instance][0] = '\0';

    effect = &g_effects[instance];
    memset(effect, 0, sizeof(effect_t));
    effect->instance = instance;

    // with the name JACK gave it: a member that leaves a chain opens its client again under it (client_back)
    effect->jack_client = open_jack_client(instance, client_name, effect->client_name, sizeof(effect->client_name));
    if (!effect->jack_client)
    {
        fprintf(stderr, "can't get jack client\n");
        return ERR_JACK_CLIENT_CREATION;
    }

    error = lv2_uri ? open_lv2(lv2_uri, effect) : open_clap(path, id, &effect->clap);
    if (error != SUCCESS)
        goto error;
    if (lv2_uri && lv2_params_build(effect) != 0)
    {
        fprintf(stderr, "%s: its parameters cannot be read\n", id);
        error = ERR_LV2_INSTANTIATION;
        goto error;
    }
    if (layout_check(effect, pin, id) != 0)
    {
        error = OMX_REPLY_HOST_PIN_LAYOUT_MISMATCH_RESP;
        goto error;
    }
    if (outputs_build(effect) != 0)
    {
        error = ERR_LV2_INSTANTIATION;
        goto error;
    }
    // the lane's width asked before the ports are registered from the description: a plugin that cannot offer it
    // keeps its declared layout (OMX_CLAP_LAYOUT_DECLARED), carried as it is
    if ((effect->clap->in_channels && effect->clap->in_channels != g_lane_channels) || effect->clap->channels != g_lane_channels)
    {
        why[0] = '\0';
        if (lane_request(effect->clap, effect->clap->in_channels != 0, why) < 0)
        {
            fprintf(stderr, "%s: %s\n", id, why);
            snprintf(g_add_reason, sizeof(g_add_reason), "%s", why);
            error = ERR_LV2_INSTANTIATION;
            goto error;
        }
    }

    effect->in_ports = effect->clap->in_channels;
    effect->out_ports = effect->clap->channels;
    if ((error = main_ports_register(effect)) != SUCCESS || (error = aux_register(effect, true)) != SUCCESS
        || (error = aux_register(effect, false)) != SUCCESS)
        goto error;
    if (aux_blocks(effect, jack_get_buffer_size(effect->jack_client)) != 0)
    {
        error = ERR_MEMORY_ALLOCATION;
        goto error;
    }

    if (effect->clap->note_inputs)
    {
        effect->midi_port = jack_port_register(effect->jack_client, MIDI_PORT_NAME, JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
        if (!effect->midi_port)
        {
            error = ERR_JACK_PORT_REGISTER;
            goto error;
        }
        effect->note_ring_buf = malloc(NOTE_RING_BYTES);
        if (!effect->note_ring_buf || omx_msgring_init(&effect->note_ring, effect->note_ring_buf, NOTE_RING_BYTES) != 0)
        {
            error = ERR_MEMORY_ALLOCATION;
            goto error;
        }
    }

    if (effect->clap->note_outputs)
    {
        effect->midi_out_port = jack_port_register(effect->jack_client, MIDI_OUT_PORT_NAME, JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
        if (!effect->midi_out_port)
        {
            error = ERR_JACK_PORT_REGISTER;
            goto error;
        }
    }

    callbacks_set(effect);

    // --thread-pool on: the plugin's tasks go to the process's pool, its helpers at the JACK thread's priority (#395)
    if (g_thread_pool == EFFECTS_THREAD_POOL_ON)
    {
        const int priority = jack_client_real_time_priority(effect->jack_client);

        omx_clap_host_thread_pool_use(effect->clap, 1, priority > 0 && priority <= 99 ? priority : 0);
    }

    if (omx_clap_host_activate(effect->clap, jack_get_sample_rate(effect->jack_client), jack_get_buffer_size(effect->jack_client), why) != 0)
    {
        if (lv2_uri)
            lv2_reason(why);
        fprintf(stderr, "%s: %s\n", id, why);
        snprintf(g_add_reason, sizeof(g_add_reason), "%s", why);
        error = ERR_LV2_INSTANTIATION;
        goto error;
    }
    effect->rate = jack_get_sample_rate(effect->jack_client);
    host_transport_read(&g_transport, &effect->intent);
    omx_clap_host_set_transport(effect->clap, &effect->transport_src);
    omx_clap_host_arm(effect->clap);

    if (jack_activate(effect->jack_client) != 0)
    {
        fprintf(stderr, "can't activate jack_client\n");
        error = ERR_JACK_CLIENT_ACTIVATION;
        goto error;
    }
    publish_latency(effect);
    idle_hold_say_once();
    if (g_show_ui)
    {
        const char *code;

        // the add stands whatever the window answers: the plugin runs, and stderr says why it shows none
        if (window_show(effect->clap, effect->clap->desc->name, &code) != 0)
            fprintf(stderr, "effect_%d: no window: %s\n", instance, code);
    }

    return instance;

error:
    instance_free(effect);
    return error;
}

/* the layout pin the next add of an instance checks: "<scheme>:<sha256>", a scheme this host does not know refused as no
 * pin at all */
int effects_pin_expect(int instance, const char *layout)
{
    char scheme[32], hex[PHD_SHA256_HEX_LEN + 1];

    if (!INSTANCE_IS_VALID(instance))
        return ERR_INSTANCE_INVALID;
    if (phd_pin_layout_parse(layout, scheme, sizeof(scheme), hex) != 0)
        return ERR_INVALID_OPERATION;
    if (strcmp(scheme, PHD_PIN_LAYOUT_SCHEME) != 0)
    {
        fprintf(stderr, "instance %d: layout pin scheme %s is not %s\n", instance, scheme, PHD_PIN_LAYOUT_SCHEME);
        return OMX_REPLY_HOST_PIN_ABSENT_RESP;
    }
    memcpy(g_layout_pins[instance], hex, sizeof(hex));
    return SUCCESS;
}

int effects_remove(int effect_id)
{
    int start, end, j;

    if (effect_id == REMOVE_ALL)
    {
        start = 0;
        end = MAX_PLUGIN_INSTANCES;
    }
    else
    {
        if (!instance_exist(effect_id))
            return ERR_INSTANCE_NON_EXISTS;
        start = effect_id;
        end = start + 1;
    }

    // every chain's members go first when all go, so that no head's removal gives them a client of their own
    for (j = 0; effect_id == REMOVE_ALL && j < CHAINS_MAX; j++)
        while (g_chains[j].id >= 0 && atomic_load_explicit(&g_chains[j].count, memory_order_relaxed) > 1)
            chain_drop(atomic_load_explicit(&g_chains[j].members[1], memory_order_relaxed));

    for (j = start; j < end; j++)
    {
        effect_t *effect = &g_effects[j];

        if (!instance_exist(j))
            continue;
        // a member leaves the walk, the chain closing around it; a head's members each get a client of their own back
        if (client_borrowed(effect))
        {
            chain_drop(effect);
            continue;
        }
        if (effect->chain)
        {
            chain_t *chain = effect->chain;
            effect_t *out[OMX_CLAP_CHAIN_MEMBERS_MAX];
            uint32_t count = atomic_load_explicit(&chain->count, memory_order_relaxed), n_out = 0, k;
            int error;

            // the last first, as a chain ends; a client JACK will not make or activate for one keeps the chain and the
            // head as they were, and the remove answers the error
            for (k = count; k-- > 1;)
                out[n_out++] = atomic_load_explicit(&chain->members[k], memory_order_relaxed);
            if ((error = chain_clients_back(chain, out, n_out)) != SUCCESS)
            {
                chain_say(chain);
                return error;
            }
            chain_end(chain);
        }
        if (jack_deactivate(effect->jack_client) != 0)
            return ERR_JACK_CLIENT_DEACTIVATION;
        instance_free(effect);
    }

    return SUCCESS;
}

/* chain <chain> <instance> ...: the instances, in this order, one chain run in the first one's client (docs/chain.md).
 * A refusal changes nothing: every check, every client of its own that a member leaving the walk goes back to and a
 * new chain's library chain come before the first change, and a member named anew whose own client JACK will not stop
 * is refused with the ones stopped before it started again, with the links they had. A chain already running with the
 * same head then changes in place: the members left out leave the walk, the members kept take the line's order in ONE
 * walk (a move; a refusal of the library keeps the order it had and the line answers ERR_INVALID_OPERATION), the
 * members named anew join at their places, and the members left out run in the clients made for them; one JACK will
 * not activate stays in the walk, at its end, and the line answers the error. A line with another head is
 * chain_rehead's: all of it or none. */
int effects_chain(int id, const int *instances, int n)
{
    chain_t *chain = NULL, *slot = NULL;
    struct omx_clap_chain *walk = NULL;
    effect_t *head;
    effect_t *leaving[OMX_CLAP_CHAIN_MEMBERS_MAX], *kept[OMX_CLAP_CHAIN_MEMBERS_MAX];
    stopped_t joining[OMX_CLAP_CHAIN_MEMBERS_MAX];
    own_client_t own[OMX_CLAP_CHAIN_MEMBERS_MAX];
    uint32_t count, j, n_leaving = 0, n_joining = 0, n_kept = 0;
    int i, k, error, ret;

    if (id < 0 || n < 1 || n > (int)OMX_CLAP_CHAIN_MEMBERS_MAX)
        return ERR_INVALID_OPERATION;
    for (i = 0; i < n; i++)
        if (!instance_exist(instances[i]))
        {
            fprintf(stderr, "chain %d: there is no effect_%d\n", id, instances[i]);
            return ERR_INSTANCE_NON_EXISTS;
        }
    for (k = 0; k < CHAINS_MAX && !chain; k++)
        if (g_chains[k].id == id)
            chain = &g_chains[k];
    head = &g_effects[instances[0]];
    for (i = 0; i < n; i++)
    {
        const effect_t *effect = &g_effects[instances[i]];

        for (k = 0; k < i; k++)
            if (instances[k] == instances[i])
            {
                fprintf(stderr, "chain %d: effect_%d is named twice\n", id, instances[i]);
                return ERR_INVALID_OPERATION;
            }
        if (effect->chain && effect->chain != chain)
        {
            fprintf(stderr, "chain %d: effect_%d is in chain %d\n", id, instances[i], effect->chain->id);
            return ERR_INVALID_OPERATION;
        }
        // one width end to end: every member's main input and output as wide as the head's main output
        if (effect->in_ports != head->out_ports || effect->out_ports != head->out_ports)
        {
            fprintf(stderr, "chain %d: effect_%d (%u in, %u out) is not as wide as effect_%d's output (%u)\n", id, instances[i],
                    effect->in_ports, effect->out_ports, head->instance, head->out_ports);
            return ERR_INVALID_OPERATION;
        }
        // an auxiliary port goes with its client too: a member's side chain would read silence and its auxiliary
        // outputs reach nothing, so only the head keeps its own
        if (i > 0 && (effect->aux_in_count || effect->aux_out_count))
        {
            fprintf(stderr, "chain %d: effect_%d has an auxiliary port (%u in, %u out), which only the chain's first member "
                    "keeps\n", id, instances[i], effect->aux_in_count, effect->aux_out_count);
            return ERR_INVALID_OPERATION;
        }
    }
    if (chain && chain->head != head)
        return chain_rehead(chain, id, head, instances, n);
    if (!chain && !(slot = chain_slot_free()))
    {
        fprintf(stderr, "chain %d: this host runs %u chains already\n", id, CHAINS_MAX);
        return ERR_INVALID_OPERATION;
    }

    // the clients of their own the members the line leaves out go back to, made before anything changes
    count = chain ? atomic_load_explicit(&chain->count, memory_order_relaxed) : 0;
    for (j = 1; j < count; j++)
    {
        effect_t *member = atomic_load_explicit(&chain->members[j], memory_order_relaxed);

        for (i = 1; i < n && &g_effects[instances[i]] != member; i++)
            ;
        if (i < n)
            continue;
        leaving[n_leaving] = member;
        if ((error = client_ready(member, &own[n_leaving])) != SUCCESS)
        {
            fprintf(stderr, "chain %d: no client of its own can be made for effect_%d (%d); nothing changed\n", id,
                    member->instance, error);
            while (n_leaving > 0)
                client_drop(&own[--n_leaving]);
            return error;
        }
        n_leaving++;
    }
    // a new chain's library chain, then the members the line names anew stop their own clients' cycles; a client JACK
    // will not stop and the ones stopped before it run again, and the line answers the error
    if ((!chain && (error = chain_walk_make(slot, id, head, &walk)) != SUCCESS)
        || (error = members_stop(id, chain, instances, n, joining, &n_joining)) != SUCCESS)
    {
        if (!chain)
            omx_clap_chain_destroy(walk);
        while (n_leaving > 0)
            client_drop(&own[--n_leaving]);
        return error;
    }
    members_keep_stopped(joining, n_joining);
    if (!chain)
    {
        chain = slot;
        chain_start(chain, id, head, walk);
    }

    // from here JACK refuses nothing that changes the walk. The members the line leaves out leave it first
    for (j = 0; j < n_leaving; j++)
        chain_extract(chain, leaving[j]);
    // the members it keeps take the line's order in one walk (a move): the head, then the others as the line names them.
    // A refusal of the library leaves the walk in the order it had and the line answers it
    kept[n_kept++] = head;
    for (i = 1; i < n; i++)
        if (g_effects[instances[i]].chain == chain)
            kept[n_kept++] = &g_effects[instances[i]];
    for (j = 0; j < n_kept && atomic_load_explicit(&chain->members[j], memory_order_relaxed) == kept[j]; j++)
        ;
    error = SUCCESS;
    if (j < n_kept && (ret = chain_order(chain, kept, n_kept)) != 0)
    {
        fprintf(stderr, "chain %d: the library refuses the order (%d); the walk keeps the one it had\n", id, ret);
        error = ERR_INVALID_OPERATION;
    }
    // the members named anew join at their places in the line's order: each walk is the line's order without the
    // members still to join
    for (i = 1; i < n; i++)
    {
        effect_t *effect = &g_effects[instances[i]];

        if (effect->chain == chain)
            continue;
        if ((ret = chain_join(chain, effect, (uint32_t)i)) != SUCCESS)
            error = ret;
    }
    // and the members left out run in the clients made for them; one JACK will not activate stays in the walk, at its
    // end, and the line answers the error
    for (j = 0; j < n_leaving; j++)
        if (client_take(leaving[j], &own[j]) != SUCCESS)
        {
            fprintf(stderr, "chain %d: effect_%d's own client cannot be activated; it stays in the chain\n", id,
                    leaving[j]->instance);
            leaving[j]->jack_client = chain->head->jack_client;
            if (chain_insert(chain, leaving[j], atomic_load_explicit(&chain->count, memory_order_relaxed)) != 0)
            {
                leaving[j]->jack_client = NULL;
                instance_free(leaving[j]);
            }
            error = ERR_JACK_CLIENT_ACTIVATION;
        }
    chain_say(chain);
    return error;
}

int effects_bypass(int effect_id, int value)
{
    if (!instance_exist(effect_id))
        return ERR_INSTANCE_NON_EXISTS;
    omx_clap_host_bypass(g_effects[effect_id].clap, value);
    return SUCCESS;
}

/* the parameter a param_set, param_get or param_info names: its CLAP id in decimal, or, for an LV2 plugin, a control
 * port's lv2:symbol; `*lv2` the LV2 control port it is, NULL for a CLAP plugin's parameter or a patch parameter */
static int param_of(const effect_t *effect, const char *control_symbol, clap_id *id, const lv2_param_t **lv2)
{
    const lv2_param_t *param = NULL;

    if (parse_param_id(control_symbol, id) == 0)
        param = effect->lv2_entry ? lv2_param_by_id(effect, *id) : NULL;
    else if (effect->lv2_entry && (param = lv2_param_by_symbol(effect, control_symbol)) != NULL)
        *id = param->id;
    else
        return -1;
    *lv2 = param;
    return 0;
}

int effects_set_parameter(int effect_id, const char *control_symbol, float value)
{
    const lv2_param_t *lv2;
    clap_id id;

    if (!instance_exist(effect_id))
        return ERR_INSTANCE_NON_EXISTS;
    if (strcmp(control_symbol, BYPASS_PORT_SYMBOL) == 0)
    {
        omx_clap_host_bypass(g_effects[effect_id].clap, value > 0.5f);
        return SUCCESS;
    }
    if (param_of(&g_effects[effect_id], control_symbol, &id, &lv2) != 0)
        return ERR_LV2_INVALID_PARAM_SYMBOL;
    if (lv2_bypass_param(lv2))
    {
        omx_clap_host_bypass(g_effects[effect_id].clap, (value > 0.5f) != (lv2->hold > 0.5));
        return SUCCESS;
    }
    return clap_param_set(g_effects[effect_id].clap, id, value);
}

int effects_get_parameter(int effect_id, const char *control_symbol, float *value)
{
    const lv2_param_t *lv2;
    clap_id id;
    double v;
    int ret;

    if (!instance_exist(effect_id))
        return ERR_INSTANCE_NON_EXISTS;
    if (strcmp(control_symbol, BYPASS_PORT_SYMBOL) == 0)
    {
        *value = omx_clap_host_bypassed(g_effects[effect_id].clap) ? 1.0f : 0.0f;
        return SUCCESS;
    }
    if (param_of(&g_effects[effect_id], control_symbol, &id, &lv2) != 0)
        return ERR_LV2_INVALID_PARAM_SYMBOL;
    if (lv2_bypass_param(lv2))
    {
        const int processing = lv2->hold > 0.5;

        *value = omx_clap_host_bypassed(g_effects[effect_id].clap) ? !processing : processing;
        return SUCCESS;
    }

    ret = clap_param_get(g_effects[effect_id].clap, id, &v);
    if (ret == SUCCESS)
        *value = (float)v;
    return ret;
}

/* the rows of the last preset_list, gone */
static void presets_forget(effect_t *effect)
{
    uint32_t i;

    for (i = 0; i < effect->presets_count; i++)
    {
        free(effect->preset_keys[i]);
        free(effect->preset_names[i]);
    }
    free(effect->preset_keys);
    free(effect->preset_names);
    effect->preset_keys = effect->preset_names = NULL;
    effect->presets_count = 0;
}

struct preset_listing
{
    const char *plugin_id;  // the instance's plugin: a preset naming other plugins only is left out
    int lv2;                // an LV2 plugin through the adapter: a PLUGIN location's key is the preset's URI, bare
    char **keys, **names;
    uint32_t n, cap;
    int failed;             // memory ran out: the listing is refused, not cut short
};

/* one preset of the plugin's file into the listing, keyed as docs/presets.md says: the URI for LV2, plugin:<key> for a
 * PLUGIN location, file:<path> for a FILE location holding one preset and file:<path>#<key> for one inside a container,
 * a space and a % written %20 and %25. A key never starts with '/': an absolute path is a preset file of ours. */
static int preset_collect(void *ctx, const struct omx_clap_preset *preset)
{
    struct preset_listing *l = ctx;
    char key[2 * PATH_MAX + 16], encoded[3 * sizeof(key)];
    uint32_t i;
    int n;

    if (preset->plugin_count > 0)
    {
        for (i = 0; i < preset->plugin_count && strcmp(preset->plugin_ids[i], l->plugin_id); i++)
            ;
        if (i == preset->plugin_count)
            return 0;
    }
    if (preset->location_kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN)
    {
        if (!preset->load_key || !preset->load_key[0])
            return 0;
        n = snprintf(key, sizeof(key), l->lv2 ? "%s" : HOST_PRESET_KEY_PLUGIN "%s", preset->load_key);
    }
    else
    {
        if (!preset->location || !preset->location[0])
            return 0;
        n = preset->load_key ? snprintf(key, sizeof(key), HOST_PRESET_KEY_FILE "%s#%s", preset->location, preset->load_key)
                             : snprintf(key, sizeof(key), HOST_PRESET_KEY_FILE "%s", preset->location);
    }
    if (n < 0 || (size_t)n >= sizeof(key) || preset_key_encode(key, encoded, sizeof(encoded)) != 0)
        return 0;
    if (l->n == l->cap)
    {
        const uint32_t cap = l->cap ? 2 * l->cap : 64;
        char **keys = realloc(l->keys, cap * sizeof(*keys));

        if (keys)
            l->keys = keys;
        char **names = keys ? realloc(l->names, cap * sizeof(*names)) : NULL;

        if (!keys || !names)
        {
            l->failed = 1;
            return 1;
        }
        l->names = names;
        l->cap = cap;
    }
    l->keys[l->n] = strdup(encoded);
    l->names[l->n] = strdup(preset->name ? preset->name : "");
    if (!l->keys[l->n] || !l->names[l->n])
    {
        free(l->keys[l->n]);
        free(l->names[l->n]);
        l->failed = 1;
        return 1;
    }
    l->n++;
    return 0;
}

/* preset_list: the plugin's own presets, read again from its file's preset-discovery factory (a CLAP plugin) or the
 * adapter's entry (an LV2 plugin, its bundle's pset:Preset); kept for preset_get until the next preset_list. 0 for a
 * plugin that offers none. */
int effects_preset_list(int effect_id)
{
    effect_t *effect;
    struct preset_listing l;

    if (!instance_exist(effect_id))
        return ERR_INSTANCE_NON_EXISTS;
    effect = &g_effects[effect_id];
    memset(&l, 0, sizeof(l));
    l.plugin_id = effect->clap->desc->id;
    l.lv2 = effect->lv2_entry != NULL;
    if (effect->lv2_entry)
        omx_clap_host_entry_presets(effect->lv2_entry, preset_collect, &l);
    else
        omx_clap_host_presets(effect->clap->bin, preset_collect, &l);
    presets_forget(effect);
    if (l.failed)
    {
        effect->preset_keys = l.keys;
        effect->preset_names = l.names;
        effect->presets_count = l.n;
        presets_forget(effect);
        return ERR_MEMORY_ALLOCATION;
    }
    effect->preset_keys = l.keys;
    effect->preset_names = l.names;
    effect->presets_count = l.n;
    return (int)l.n;
}

/* preset_get: "resp 0 <load_key> <name>", the name the rest of the line with a byte below 0x20 a space; -902 for an
 * index the last preset_list did not give */
int effects_preset_get(int effect_id, int index, char *reply, size_t size)
{
    const effect_t *effect;
    int n;

    if (!instance_exist(effect_id))
        return ERR_INSTANCE_NON_EXISTS;
    effect = &g_effects[effect_id];
    if (index < 0 || (uint32_t)index >= effect->presets_count)
        return ERR_INVALID_OPERATION;
    n = snprintf(reply, size, "resp 0 %s %s", effect->preset_keys[index], effect->preset_names[index]);
    if (n < 0 || (size_t)n >= size)
        return ERR_INVALID_OPERATION;
    for (char *c = reply; *c; c++)
        if ((unsigned char)*c < 0x20)
            *c = ' ';
    return SUCCESS;
}

int effects_preset_load(int effect_id, const char *location)
{
    struct omx_clap_instance *clap;
    char key[2 * PATH_MAX + 16];
    struct stat st;
    char *mark;

    if (!instance_exist(effect_id))
        return ERR_INSTANCE_NON_EXISTS;
    clap = g_effects[effect_id].clap;
    if (!clap->preset_load)
        return ERR_INVALID_OPERATION;
    if (snprintf(key, sizeof(key), "%s", location) >= (int)sizeof(key))
        return ERR_LV2_INVALID_PRESET_URI;
    preset_key_decode(key);
    // a key of preset_get: plugin:<key>, file:<path> or file:<path>#<key> (a path that exists whole is a file)
    if (!strncmp(key, HOST_PRESET_KEY_PLUGIN, strlen(HOST_PRESET_KEY_PLUGIN)))
        return omx_clap_host_preset_load_key(clap, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, NULL,
                                             key + strlen(HOST_PRESET_KEY_PLUGIN)) == 0 ? SUCCESS : ERR_LV2_INVALID_PRESET_URI;
    if (!strncmp(key, HOST_PRESET_KEY_FILE, strlen(HOST_PRESET_KEY_FILE)))
    {
        char *path = key + strlen(HOST_PRESET_KEY_FILE);
        const char *inner = NULL;

        if (stat(path, &st) != 0 && (mark = strrchr(path, '#')) != NULL)
        {
            *mark = '\0';
            inner = mark + 1;
        }
        return omx_clap_host_preset_load_key(clap, CLAP_PRESET_DISCOVERY_LOCATION_FILE, path, inner) == 0
            ? SUCCESS : ERR_LV2_INVALID_PRESET_URI;
    }
    // an LV2 preset is named by its URI, as mod-host takes it: a preset of the plugin's bundle, the URI its load key
    if (g_effects[effect_id].lv2_entry)
        return omx_clap_host_preset_load_key(clap, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, NULL, key) == 0
            ? SUCCESS : ERR_LV2_INVALID_PRESET_URI;
    return omx_clap_host_preset_load(clap, key) == 0 ? SUCCESS : ERR_LV2_INVALID_PRESET_URI;
}

/* a preset verb's refusal: "resp -104 <code>", the code one of plugin-hostd's preset refusals (docs/presets.md) */
static int preset_refuse(char *reply, size_t size, const char *code)
{
    snprintf(reply, size, "resp %d %s", ERR_LV2_INVALID_PRESET_URI, code);
    return ERR_LV2_INVALID_PRESET_URI;
}

static int pj_set_string(struct pj *obj, const char *key, const char *text)
{
    struct pj v;

    memset(&v, 0, sizeof(v));
    return pj_string(&v, text) != 0 || pj_set(obj, key, &v) != 0 ? -1 : 0;
}

/* the instance's plugin as a preset file names it: its id, its descriptor's version, and its layout's fingerprint
 * (omx-layout/1:<sha256>, plugin-hostd's pin) */
static int preset_identity(const effect_t *effect, const char **id, const char **version, char *fingerprint, size_t size)
{
    char hex[PHD_SHA256_HEX_LEN + 1];

    *id = effect->clap->desc->id;
    *version = effect->clap->desc->version ? effect->clap->desc->version : "";
    if (layout_pin_write(effect->clap->plugin, effect->clap->params, NULL, NULL, hex) != 0)
        return -1;
    snprintf(fingerprint, size, "%s:%s", PHD_PIN_LAYOUT_SCHEME, hex);
    return 0;
}

/* the name param_set takes for a parameter row: an LV2 control port's symbol, else the CLAP id in decimal */
static const char *preset_symbol(const effect_t *effect, clap_id id, char *buf, size_t size)
{
    const lv2_param_t *lv2 = effect->lv2_entry ? lv2_param_by_id(effect, id) : NULL;

    if (lv2)
        return lv2->symbol;
    snprintf(buf, size, "%u", id);
    return buf;
}

/* preset_save: the instance into an org.openmixer.preset/2 file at `path` named `name`. An existing preset file at the
 * path keeps its id, createdAt and every member this host does not write; anything else at the path is refused. */
int effects_preset_save(int effect_id, const char *path, const char *name, char *reply, size_t size)
{
    effect_t *effect;
    struct pj doc, plugin, params, state, row, v;
    struct omx_clap_param_row *rows = NULL;
    const char *pid, *version;
    char fingerprint[96], now[32], tmp[PATH_MAX + 8], symbol[16];
    uint8_t *body = NULL;
    size_t body_len = 0;
    char *text = NULL;
    struct stat st;
    time_t t = time(NULL);
    struct tm tm;
    int count, i, ret;
    FILE *f;

    if (!instance_exist(effect_id))
        return ERR_INSTANCE_NON_EXISTS;
    effect = &g_effects[effect_id];
    if (!path || path[0] != '/' || !name || !name[0])
        return preset_refuse(reply, size, PRESET_CODE_INVALID);
    memset(&doc, 0, sizeof(doc));
    if (stat(path, &st) == 0)
    {
        const char *format;

        if (pj_read_file(path, &doc) != 0 || doc.type != PJ_OBJECT || !(format = pj_get_str(&doc, "format")) ||
            strncmp(format, PRESET_FORMAT_PREFIX, strlen(PRESET_FORMAT_PREFIX)) != 0)
        {
            pj_free(&doc);
            return preset_refuse(reply, size, PRESET_CODE_INVALID);
        }
    }
    else
        pj_object(&doc);
    if (preset_identity(effect, &pid, &version, fingerprint, sizeof(fingerprint)) != 0)
    {
        pj_free(&doc);
        return preset_refuse(reply, size, PRESET_CODE_LOAD_REFUSED);
    }
    gmtime_r(&t, &tm);
    strftime(now, sizeof(now), "%Y-%m-%dT%H:%M:%SZ", &tm);

    ret = -1;
    memset(&plugin, 0, sizeof(plugin));
    memset(&params, 0, sizeof(params));
    memset(&state, 0, sizeof(state));
    pj_object(&plugin);
    pj_array(&params);
    if (pj_set_string(&plugin, "id", pid) || pj_set_string(&plugin, "version", version) ||
        pj_set_string(&plugin, "fingerprint", fingerprint))
        goto out;
    count = omx_clap_host_param_roster(effect->clap, &rows, NULL);
    for (i = 0; i < count; i++)
    {
        double value;

        if (clap_param_get(effect->clap, rows[i].id, &value) != SUCCESS)
            continue;
        memset(&row, 0, sizeof(row));
        memset(&v, 0, sizeof(v));
        pj_object(&row);
        if (pj_set_string(&row, "symbol", preset_symbol(effect, rows[i].id, symbol, sizeof(symbol))) ||
            pj_set_string(&row, "unit", "") || pj_number(&v, value) || pj_set(&row, "value", &v) || pj_push(&params, &row))
        {
            pj_free(&row);
            goto out;
        }
    }
    if (effect->clap->state)
    {
        char *b64;

        body = malloc(CLAP_HOST_STATE_MAX_BYTES);
        omx_clap_host_settle(effect->clap, QUEUE_SETTLE_US);
        if (!body || omx_clap_host_state_save_context(effect->clap, CLAP_STATE_CONTEXT_FOR_PRESET, body,
                                                      CLAP_HOST_STATE_MAX_BYTES, &body_len) != 0)
        {
            ret = -2;
            goto out;
        }
        if (!(b64 = preset_base64_encode(body, body_len)))
            goto out;
        pj_object(&state);
        ret = pj_set_string(&state, "encoding", PRESET_ENCODING_BASE64) ||
              pj_set_string(&state, "context", effect->lv2_entry ? PRESET_CONTEXT_LV2 : PRESET_CONTEXT_PRESET) ||
              pj_set_string(&state, "bytes", b64);
        free(b64);
        if (ret)
        {
            ret = -1;
            goto out;
        }
    }
    if (!pj_get_str(&doc, "id"))
    {
        char minted[512];

        snprintf(minted, sizeof(minted), "%s@%s", pid, now);
        if (pj_set_string(&doc, "id", minted))
            goto out;
    }
    if (!pj_get_str(&doc, "createdAt") && pj_set_string(&doc, "createdAt", now))
        goto out;
    if (pj_set_string(&doc, "format", PRESET_FORMAT_STATE) || pj_set(&doc, "plugin", &plugin) ||
        pj_set_string(&doc, "name", name) || pj_set_string(&doc, "modifiedAt", now) ||
        pj_set_string(&doc, "carries", effect->clap->state ? PRESET_CARRIES_PARAMS_STATE : PRESET_CARRIES_PARAMS) ||
        pj_set(&doc, "params", &params))
        goto out;
    if (effect->clap->state)
    {
        if (pj_set(&doc, "state", &state))
            goto out;
    }
    else
    {
        // a plugin without state writes none, and a rewrite drops the body an older save left
        struct pj none;

        memset(&none, 0, sizeof(none));
        if (pj_get(&doc, "state") && pj_set(&doc, "state", &none))
            goto out;
    }
    if (!(text = pj_write(&doc, 1)))
        goto out;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (!(f = fopen(tmp, "w")))
    {
        ret = -3;
        goto out;
    }
    ret = fputs(text, f) < 0;
    ret = (fclose(f) != 0 || ret || rename(tmp, path) != 0) ? -3 : 0;
    if (ret)
        unlink(tmp);
out:
    free(rows);
    free(body);
    free(text);
    pj_free(&plugin);
    pj_free(&params);
    pj_free(&state);
    pj_free(&doc);
    if (ret == 0)
    {
        snprintf(reply, size, "resp 0");
        return SUCCESS;
    }
    return preset_refuse(reply, size, ret == -2 ? PRESET_CODE_LOAD_REFUSED : PRESET_CODE_INVALID);
}

/* preset_load of a file: an org.openmixer.preset/1 or /2 file for the instance's plugin, its state body loaded first
 * and then each parameter at its exact value */
static int preset_file_load(effect_t *effect, const char *path, char *reply, size_t size)
{
    struct pj doc, *plugin, *params, *state;
    const char *format, *carries, *pid, *version;
    char fingerprint[96];
    int v2, ret;
    uint32_t i;

    memset(&doc, 0, sizeof(doc));
    if (pj_read_file(path, &doc) != 0 || doc.type != PJ_OBJECT || !(format = pj_get_str(&doc, "format")))
    {
        pj_free(&doc);
        return preset_refuse(reply, size, PRESET_CODE_INVALID);
    }
    v2 = !strcmp(format, PRESET_FORMAT_STATE);
    if (!v2 && strcmp(format, PRESET_FORMAT_PARAMS))
    {
        pj_free(&doc);
        return preset_refuse(reply, size, PRESET_CODE_INVALID);
    }
    plugin = pj_get(&doc, "plugin");
    carries = pj_get_str(&doc, "carries");
    params = pj_get(&doc, "params");
    state = pj_get(&doc, "state");
    ret = preset_identity(effect, &pid, &version, fingerprint, sizeof(fingerprint));
    if (!plugin || plugin->type != PJ_OBJECT || !pj_get_str(plugin, "id") || (params && params->type != PJ_ARRAY) ||
        (v2 && (!carries || !pj_get_str(plugin, "version") || !pj_get_str(plugin, "fingerprint"))))
        ret = preset_refuse(reply, size, PRESET_CODE_INVALID);
    else if (ret != 0)
        ret = preset_refuse(reply, size, PRESET_CODE_LOAD_REFUSED);
    else if (strcmp(pj_get_str(plugin, "id"), pid) ||
             (v2 && (strcmp(pj_get_str(plugin, "version"), version) || strcmp(pj_get_str(plugin, "fingerprint"), fingerprint))))
        ret = preset_refuse(reply, size, PRESET_CODE_PLUGIN_CHANGED);
    else if (state && !effect->clap->state)
        ret = preset_refuse(reply, size, PRESET_CODE_NEEDS_STATE);
    else
        ret = SUCCESS;
    if (ret == SUCCESS && state)
    {
        const char *bytes = pj_get_str(state, "bytes");
        size_t len = 0;
        uint8_t *body = bytes ? preset_base64_decode(bytes, &len) : NULL;

        if (!body)
            ret = preset_refuse(reply, size, PRESET_CODE_INVALID);
        else if (omx_clap_host_state_load_context(effect->clap, CLAP_STATE_CONTEXT_FOR_PRESET, body, len) != 0)
            ret = preset_refuse(reply, size, PRESET_CODE_LOAD_REFUSED);
        free(body);
    }
    for (i = 0; ret == SUCCESS && params && i < params->n; i++)
    {
        const struct pj *row = &params->kids[i];
        const struct pj *value = pj_get(row, "value");
        const char *symbol = pj_get_str(row, "symbol");
        const lv2_param_t *lv2 = effect->lv2_entry && symbol ? lv2_param_by_symbol(effect, symbol) : NULL;
        char *end;
        unsigned long id;

        if (!symbol || !value || value->type != PJ_NUMBER)
            continue;
        if (lv2)
            id = lv2->id;
        else
        {
            id = strtoul(symbol, &end, 10);
            if (*end || end == symbol)
                continue;
        }
        clap_param_set(effect->clap, (clap_id)id, value->num);
    }
    pj_free(&doc);
    if (ret == SUCCESS)
        snprintf(reply, size, "resp 0");
    return ret;
}

/* preset_load as the socket answers it: a file path (absolute) is a preset file, anything else a key of preset_get */
int effects_preset_load_reply(int effect_id, const char *location, char *reply, size_t size)
{
    int ret;

    if (!instance_exist(effect_id))
        return ERR_INSTANCE_NON_EXISTS;
    if (location[0] == '/')
    {
        char path[PATH_MAX];

        snprintf(path, sizeof(path), "%s", location);
        preset_key_decode(path);
        return preset_file_load(&g_effects[effect_id], path, reply, size);
    }
    // a key the plugin does not offer, or a plugin with no clap.preset-load for a key. A location in mod-host's form (a
    // preset URI, not a key of preset_get) is answered as mod-host answers it, the bare resp -104, for a CLAP plugin as
    // for an LV2 one: plugin-hostd's scenario corpus holds every worker to mod-host's replies
    ret = effects_preset_load(effect_id, location);
    if (strncmp(location, HOST_PRESET_KEY_PLUGIN, strlen(HOST_PRESET_KEY_PLUGIN)) &&
        strncmp(location, HOST_PRESET_KEY_FILE, strlen(HOST_PRESET_KEY_FILE)))
        return ret;
    if (ret == ERR_LV2_INVALID_PRESET_URI || ret == ERR_INVALID_OPERATION)
        return preset_refuse(reply, size, PRESET_CODE_NOT_OFFERED);
    if (ret == SUCCESS)
        snprintf(reply, size, "resp 0");
    return ret;
}

int effects_state_save(const char *dir)
{
    char filename[PATH_MAX];
    int i, ret, error = SUCCESS;

    if (access(dir, F_OK) != 0 && mkdir(dir, 0755) != 0)
        return ERR_LV2_CANT_LOAD_STATE;

    for (i = 0; i < MAX_PLUGIN_INSTANCES; i++)
    {
        if (!instance_exist(i))
            continue;
        state_filename(filename, sizeof(filename), dir, i);
        ret = clap_state_save(g_effects[i].clap, filename);
        if (ret == ERR_INVALID_OPERATION)
            unlink(filename);
        else if (ret != SUCCESS)
            error = ret;
    }
    return error;
}

int effects_state_load(const char *dir)
{
    char filename[PATH_MAX];
    int i, ret, error = SUCCESS;

    for (i = 0; i < MAX_PLUGIN_INSTANCES; i++)
    {
        if (!instance_exist(i))
            continue;
        state_filename(filename, sizeof(filename), dir, i);
        if (access(filename, F_OK) != 0)
            continue;
        ret = clap_state_load(g_effects[i].clap, filename);
        if (ret != SUCCESS)
            error = ret;
    }
    return error;
}

int effects_connect(const char *portA, const char *portB)
{
    int ret = jack_connect(g_jack_global_client, portA, portB);

    if (ret != 0 && ret != EEXIST)
        return ERR_JACK_PORT_CONNECTION;
    return SUCCESS;
}

int effects_disconnect(const char *portA, const char *portB)
{
    if (jack_disconnect(g_jack_global_client, portA, portB) != 0)
        return ERR_JACK_PORT_DISCONNECTION;
    return SUCCESS;
}

float effects_jack_cpu_load(void)
{
    return g_jack_global_client ? jack_cpu_load(g_jack_global_client) : 0.0f;
}

/* note_drops: the stage's note counters, read relaxed as the audio thread leaves them: every note event the block had no
 * room for, of them the releases (a note off, all notes off, a pedal let go), the sysex past the block's pool, and the
 * messages no dialect of the plugin carries */
int effects_note_drops(int effect_id, char *reply, size_t size)
{
    const struct omx_clap_stage *stage;

    if (!instance_exist(effect_id))
        return ERR_INSTANCE_NON_EXISTS;
    stage = &g_effects[effect_id].clap->stage;
    snprintf(reply, size, "resp 0 %u %u %u %u", atomic_load_explicit(&stage->notes_dropped, memory_order_relaxed),
             atomic_load_explicit(&stage->releases_dropped, memory_order_relaxed),
             atomic_load_explicit(&stage->sysex_dropped, memory_order_relaxed),
             atomic_load_explicit(&stage->notes_unmappable, memory_order_relaxed));
    return SUCCESS;
}

/* note_input: "resp 0 midi_in" for an instance that takes notes, "resp 0 -" for one that takes none */
int effects_note_input(int effect_id, char *reply, size_t size)
{
    if (!instance_exist(effect_id))
        return ERR_INSTANCE_NON_EXISTS;
    snprintf(reply, size, "resp 0 %s", g_effects[effect_id].note_ring_buf ? MIDI_PORT_NAME : HOST_WORD_NONE);
    return SUCCESS;
}

/* the note verbs, on the control thread: `count` MIDI messages of `size` bytes each, handed to the audio thread whole
 * or not at all, which plays them at the start of its next cycle through the same omx_clap_host_note_in midi_in feeds */
int effects_note(int effect_id, const uint8_t (*messages)[3], uint32_t count)
{
    effect_t *effect;
    uint32_t i;

    if (!instance_exist(effect_id))
        return ERR_INSTANCE_NON_EXISTS;
    effect = &g_effects[effect_id];
    if (!effect->note_ring_buf)
        return OMX_REPLY_HOST_NO_NOTE_INPUT_RESP;
    if (NOTE_RING_BYTES - omx_msgring_used(&effect->note_ring) < count * omx_msgring_record_bytes(3) + omx_msgring_record_bytes(3))
        return OMX_REPLY_HOST_NOTE_QUEUE_FULL_RESP;     // the audio thread has not drained the ring: no room, nothing queued
    for (i = 0; i < count; i++)
        omx_msgring_push(&effect->note_ring, messages[i], 3);
    return SUCCESS;
}

/* the control thread: a change of the host's transport, anchored at the frame the graph is at now */
static void transport_change(int rolling, double bpm, double bpb)
{
    const uint32_t now = g_jack_global_client ? jack_frame_time(g_jack_global_client) : 0u;
    const double rate = g_jack_global_client ? (double)jack_get_sample_rate(g_jack_global_client) : 48000.0;

    g_intent = host_transport_change(&g_intent, now, rate, rolling, bpm, bpb);
    host_transport_publish(&g_transport, &g_intent);
}

static int bpm_valid(double bpm)
{
    return bpm >= HOST_TRANSPORT_BPM_MIN && bpm <= HOST_TRANSPORT_BPM_MAX;
}

static int bpb_valid(double bpb)
{
    return bpb >= HOST_TRANSPORT_BPB_MIN && bpb <= HOST_TRANSPORT_BPB_MAX;
}

int effects_set_bpm(double bpm)
{
    if (!bpm_valid(bpm))
        return ERR_JACK_VALUE_OUT_OF_RANGE;
    transport_change(g_intent.rolling, bpm, 0.0);
    return SUCCESS;
}

int effects_set_bpb(double bpb)
{
    if (!bpb_valid(bpb))
        return ERR_JACK_VALUE_OUT_OF_RANGE;
    transport_change(g_intent.rolling, 0.0, bpb);
    return SUCCESS;
}

int effects_transport(int rolling, double bpb, double bpm)
{
    if (!bpm_valid(bpm) || !bpb_valid(bpb))
        return ERR_JACK_VALUE_OUT_OF_RANGE;
    transport_change(rolling != 0, bpm, bpb);
    return SUCCESS;
}

/* "none" is the host's own transport (or the JACK one with -t); Ableton Link and MIDI clock it has neither of */
int effects_transport_sync(const char *mode)
{
    if (!mode || !strcmp(mode, "link") || !strcmp(mode, "midi"))
        return ERR_INVALID_OPERATION;
    return SUCCESS;
}

void effects_follow_jack_transport(int on)
{
    g_follow_jack = on != 0;
}

void effects_set_lane_channels(uint32_t channels)
{
    g_lane_channels = channels == 1 ? 1 : 2;
}

int effects_idle_hold_word(const char *word, int32_t *target_us)
{
    char *end;
    long value;

    if (!word || !*word)
        return 0;
    if (strcmp(word, "off") == 0)
    {
        *target_us = OMX_CLAP_IDLE_HOLD_OFF;
        return 1;
    }
    if (!isdigit((unsigned char)word[0]))
        return 0;
    value = strtol(word, &end, 10);
    if (*end != '\0' || value < 0 || value > INT32_MAX)
        return 0;
    *target_us = (int32_t)value;
    return 1;
}

void effects_set_idle_hold(int32_t target_us)
{
    g_idle_hold = target_us;
}

int effects_idle_hold_set(const char *word)
{
    int32_t target;

    if (!effects_idle_hold_word(word, &target) || omx_clap_host_idle_hold_set(target) != 0)
        return ERR_INVALID_OPERATION;
    idle_hold_say_once();
    return SUCCESS;
}

int effects_idle_hold(char *reply, size_t size)
{
    struct omx_clap_idle_hold hold;
    char target[16], held[16], aggregate[16];

    omx_clap_host_idle_hold(&hold);
    if (hold.aggregate_us < 0)
        snprintf(aggregate, sizeof(aggregate), "-");
    else
        snprintf(aggregate, sizeof(aggregate), "%d", hold.aggregate_us);
    snprintf(reply, size, "resp 0 %s %s %s %u %s", idle_hold_word_of(hold.target_us, target, sizeof(target)),
             idle_hold_word_of(hold.held_us, held, sizeof(held)), aggregate, hold.active, hold.code[0] ? hold.code : "-");
    return SUCCESS;
}

/* note_out_drops: the plugin's note output counters, read relaxed as the audio thread leaves them: the events the
 * library's ring had no room for, those with no MIDI form, and the messages midi_out had no room for */
int effects_note_out_drops(int effect_id, char *reply, size_t size)
{
    const effect_t *effect;

    if (!instance_exist(effect_id))
        return ERR_INSTANCE_NON_EXISTS;
    effect = &g_effects[effect_id];
    snprintf(reply, size, "resp 0 %u %u %u", atomic_load_explicit(&effect->clap->note_out.dropped, memory_order_relaxed),
             atomic_load_explicit(&effect->clap->note_out.unmapped, memory_order_relaxed),
             atomic_load_explicit(&effect->midi_out_full, memory_order_relaxed));
    return SUCCESS;
}

void effects_idle(void)
{
    int i;

    g_idle_ticks++;
    xruns_feedback();
    // a member's block size its head's callback left to this thread is recorded before the member's tick reads it
    chain_members_reconcile();
    for (i = 0; i < MAX_PLUGIN_INSTANCES; i++)
    {
        effect_t *effect = &g_effects[i];

        if (!instance_exist(i))
            continue;
        clap_idle(effect);
        ports_relayout(effect);
        // a thread JACK named while the restart, the resize or the relayout held the role is set as soon as it is let go
        chain_members_reconcile();
        if (omx_clap_host_remote_controls_changed(effect->clap))
        {
            char line[64];

            snprintf(line, sizeof(line), HOST_EVENT_REMOTE_PAGES_CHANGED_FMT, effect->instance);
            socket_send_feedback(line);
        }
        if (omx_clap_host_latency(effect->clap) != effect->latency_published)
            publish_latency(effect);
        param_events_notify(effect);
        outputs_notify(effect);
    }
    window_idle();
}

void effects_show_ui_at_add(int on)
{
    g_show_ui = on;
}

int effects_thread_pool_word(const char *word, int *mode)
{
    if (!strcmp(word, "qualified"))
        *mode = EFFECTS_THREAD_POOL_QUALIFIED;
    else if (!strcmp(word, "off"))
        *mode = EFFECTS_THREAD_POOL_OFF;
    else if (!strcmp(word, "on"))
        *mode = EFFECTS_THREAD_POOL_ON;
    else
        return 0;
    return 1;
}

void effects_set_thread_pool(int mode)
{
    g_thread_pool = mode;
}

int effects_show_ui(int effect_id, const char **code)
{
    *code = NULL;
    if (!instance_exist(effect_id))
        return ERR_INSTANCE_NON_EXISTS;
    if (window_show(g_effects[effect_id].clap, g_effects[effect_id].clap->desc->name, code) != 0)
        return ERR_EXTERNAL_UI_UNAVAILABLE;
    return SUCCESS;
}

int effects_hide_ui(int effect_id)
{
    if (!instance_exist(effect_id))
        return ERR_INSTANCE_NON_EXISTS;
    return window_hide(g_effects[effect_id].clap) == 0 ? SUCCESS : ERR_EXTERNAL_UI_UNAVAILABLE;
}

/* the CLAP flag of a strip kind, in the order of HOST_TRACK_KINDS; 0 for an input channel */
static uint64_t track_kind_flag(const char *kind)
{
    static const uint64_t flags[OMX_HOST_TRACK_KINDS_COUNT] = {
        CLAP_TRACK_INFO_IS_FOR_BUS, CLAP_TRACK_INFO_IS_FOR_RETURN_TRACK, CLAP_TRACK_INFO_IS_FOR_MASTER,
    };
    const int n = host_track_kind(kind);

    _Static_assert(OMX_HOST_TRACK_KINDS_COUNT == 3, "a CLAP flag for each word of HOST_TRACK_KINDS");
    return n > 0 ? flags[n - 1] : 0;
}

/* track_info: the words already checked against the grammar (host_track_info_valid) */
int effects_track_info(int effect_id, const char *name, const char *color, const char *kind)
{
    clap_color_t rgb;
    unsigned long value;

    if (!instance_exist(effect_id))
        return ERR_INSTANCE_NON_EXISTS;
    if (strcmp(color, HOST_WORD_NONE) != 0)
    {
        value = strtoul(color + 1, NULL, 16);
        rgb.alpha = 255;
        rgb.red = (uint8_t)(value >> 16);
        rgb.green = (uint8_t)(value >> 8);
        rgb.blue = (uint8_t)value;
    }
    if (omx_clap_host_track_info_set(g_effects[effect_id].clap, name,
                                     strcmp(color, HOST_WORD_NONE) ? &rgb : NULL, track_kind_flag(kind)) != 0)
        return ERR_INVALID_OPERATION;
    return SUCCESS;
}

/* remote_pages: the plugin's page count, 0 without clap.remote-controls */
int effects_remote_pages(int effect_id)
{
    const struct omx_clap_instance *clap;

    if (!instance_exist(effect_id))
        return ERR_INSTANCE_NON_EXISTS;
    clap = g_effects[effect_id].clap;
    return clap->remote_controls ? (int)clap->remote_controls->count(clap->plugin) : 0;
}

/* a name a plugin reports as one quoted word: a byte below 0x20 a space, cut at the last whole UTF-8 character within
 * HOST_STRING_MAX bytes, '"' written \" and a backslash \\ */
static void quoted(char *out, size_t size, const char *name)
{
    char clean[HOST_STRING_MAX + 1];
    size_t n = 0, o = 0;

    for (; name[n] && n < HOST_STRING_MAX; n++)
        clean[n] = (unsigned char)name[n] < 0x20 ? ' ' : name[n];
    if (name[n])
        while (n > 0 && ((unsigned char)name[n] & 0xc0) == 0x80)
            n--;
    clean[n] = '\0';
    out[o++] = '"';
    for (n = 0; clean[n] && o + 3 < size; n++)
    {
        if (clean[n] == '"' || clean[n] == '\\')
            out[o++] = '\\';
        out[o++] = clean[n];
    }
    out[o++] = '"';
    out[o] = '\0';
}

/* remote_page_get: the page's reply into `reply`; a slot is the id param_set takes, HOST_WORD_NONE for an empty one,
 * an id params does not list, and an id param_set would refuse */
int effects_remote_page_get(int effect_id, int page, char *reply, size_t size)
{
    struct omx_clap_instance *clap;
    clap_remote_controls_page_t info;
    char section[2 * HOST_STRING_MAX + 3], name[2 * HOST_STRING_MAX + 3];
    size_t used;
    int i;

    if (!instance_exist(effect_id))
        return ERR_INSTANCE_NON_EXISTS;
    clap = g_effects[effect_id].clap;
    memset(&info, 0, sizeof(info));
    if (!clap->remote_controls || page < 0 || (uint32_t)page >= clap->remote_controls->count(clap->plugin) ||
        !clap->remote_controls->get(clap->plugin, (uint32_t)page, &info))
        return ERR_INVALID_OPERATION;
    info.section_name[sizeof(info.section_name) - 1] = '\0';
    info.page_name[sizeof(info.page_name) - 1] = '\0';
    quoted(section, sizeof(section), info.section_name);
    quoted(name, sizeof(name), info.page_name);
    used = snprintf(reply, size, "resp 0 %u %s %s", info.page_id, section, name);
    for (i = 0; i < HOST_REMOTE_PAGE_SLOTS && used < size; i++)
    {
        const clap_id id = info.param_ids[i];

        if (id == CLAP_INVALID_ID || !omx_clap_host_param_is_row(clap, id))
            used += snprintf(reply + used, size - used, " %s", HOST_WORD_NONE);
        else
            used += snprintf(reply + used, size - used, " %u", id);
    }
    return used < size ? SUCCESS : ERR_INVALID_OPERATION;
}

/* param_info: -103 for what is not a parameter param_set takes; -511 for a parameter of a plugin whose
 * org.openmixer.param-contract/1 is absent or broke a rule (the controller falls back to qualification); else the unit,
 * the scale and the step the contract gives, the range and the default clap.params gives, and the contract's symbol */
int effects_param_info(int effect_id, const char *symbol, char *reply, size_t size)
{
    struct omx_clap_param_meaning m;
    char min[32], max[32], def[32], step[32];
    const lv2_param_t *lv2;
    clap_id id;
    int used;

    if (!instance_exist(effect_id))
        return ERR_INSTANCE_NON_EXISTS;
    if (param_of(&g_effects[effect_id], symbol, &id, &lv2) != 0 || !omx_clap_host_param_is_row(g_effects[effect_id].clap, id))
        return ERR_LV2_INVALID_PARAM_SYMBOL;
    if (omx_clap_host_param_meaning(g_effects[effect_id].clap, id, &m) != 0
        || omx_clap_param_number(m.min, min, sizeof(min)) < 0 || omx_clap_param_number(m.max, max, sizeof(max)) < 0
        || omx_clap_param_number(m.def, def, sizeof(def)) < 0 || omx_clap_param_number(m.step, step, sizeof(step)) < 0)
        return OMX_REPLY_HOST_NO_PARAM_CONTRACT_RESP;
    used = snprintf(reply, size, "resp 0 %s %s %s %s %s %s %s", omx_clap_param_unit_name(m.unit),
                    omx_clap_param_scale_name(m.scale), min, max, def, step, m.symbol);
    return used > 0 && (size_t)used < size ? SUCCESS : ERR_INVALID_OPERATION;
}

/* one registered port as audio_ports prints it (layout_port_text's record): its width the registered one, its kind,
 * layout and name the description's port at that place when it has that width, unknown and nameless when it does not
 * (a description read again after add that the instance passes dry with) */
static void audio_port_record(FILE *out, const effect_t *effect, bool is_input, uint32_t aux, uint32_t channels)
{
    static const char *const kinds[] = { "unknown", "mono", "stereo", "surround", "ambisonic" };
    static const char *const normalizations[] = { "maxn", "sn3d", "n3d", "sn2d", "n2d" };
    const struct omx_clap_instance *clap = effect->clap;
    const struct omx_clap_port *port = aux == LAYOUT_PORT_MAIN ? (is_input ? &clap->main_in : &clap->main_out)
                                     : aux < (is_input ? clap->aux_inputs : clap->aux_outputs) ? (is_input ? &clap->aux_in[aux] : &clap->aux_out[aux])
                                     : NULL;
    struct layout_port_text t;

    if (port && port->channels != channels)
        port = NULL;
    memset(&t, 0, sizeof(t));
    t.is_input = is_input;
    t.aux = aux;
    t.channels = channels;
    t.kind = port && port->kind < sizeof(kinds) / sizeof(kinds[0]) ? kinds[port->kind] : kinds[0];
    t.name = port ? port->name : "";
    t.jack = true;
    if (port && port->kind == OMX_CLAP_PORT_SURROUND)
    {
        t.map = port->map;
        t.map_n = port->channels;
    }
    else if (port && port->kind == OMX_CLAP_PORT_AMBISONIC)
    {
        t.ordering = port->ambisonic_ordering == CLAP_AMBISONIC_ORDERING_FUMA ? "fuma" : "acn";
        t.normalization = port->ambisonic_normalization < sizeof(normalizations) / sizeof(normalizations[0])
                          ? normalizations[port->ambisonic_normalization] : "sn3d";
        t.assumed = port->ambisonic_assumed != 0;     // the description's: the configuration is the host's assumption
    }
    fputc(' ', out);
    layout_port_text(out, &t, 1);
}

int effects_audio_ports(int effect_id, char **reply)
{
    const effect_t *effect;
    size_t size = 0;
    FILE *out;
    uint32_t p;
    int dry;

    if (!instance_exist(effect_id))
        return ERR_INSTANCE_NON_EXISTS;
    effect = &g_effects[effect_id];
    *reply = NULL;
    out = open_memstream(reply, &size);
    if (!out)
        return ERR_MEMORY_ALLOCATION;
    // the ports registered at add, inputs first and each side's main port first; dry while the instance is not run
    // an active instance whose auxiliary blocks are shorter than the JACK cycle passes every cycle dry too (process);
    // both are written under g_reactivate by the buffer-size callback and the relayout, and read under it here. The
    // registered ports never move after add.
    pthread_mutex_lock(&g_reactivate);
    dry = !effect->clap->active || ((effect->aux_in_count || effect->aux_out_count)
                                    && effect->aux_frames < jack_get_buffer_size(effect->jack_client));
    pthread_mutex_unlock(&g_reactivate);
    fputs(dry ? "resp 0 dry" : "resp 0", out);
    if (effect->in_ports)
        audio_port_record(out, effect, true, LAYOUT_PORT_MAIN, effect->in_ports);
    for (p = 0; p < effect->aux_in_n; p++)
        audio_port_record(out, effect, true, p, effect->aux_in_widths[p]);
    if (effect->out_ports)
        audio_port_record(out, effect, false, LAYOUT_PORT_MAIN, effect->out_ports);
    for (p = 0; p < effect->aux_out_n; p++)
        audio_port_record(out, effect, false, p, effect->aux_out_widths[p]);
    if (fclose(out) != 0 || !*reply)
    {
        free(*reply);
        *reply = NULL;
        return ERR_MEMORY_ALLOCATION;
    }
    return SUCCESS;
}

/* monitor_output: 0 once `symbol` is monitored, its first value going out with the next idle call */
int effects_monitor_output(int effect_id, const char *symbol)
{
    effect_t *effect;
    uint32_t i;

    if (!instance_exist(effect_id))
        return ERR_INSTANCE_NON_EXISTS;
    effect = &g_effects[effect_id];
    for (i = 0; i < effect->outputs_count; i++)
    {
        output_t *output = &effect->outputs[i];

        if (strcmp(output->symbol, symbol) != 0)
            continue;
        if (!output->monitored)
        {
            output->monitored = 1;
            output->notified = 0;
        }
        return SUCCESS;
    }
    return ERR_LV2_INVALID_PARAM_SYMBOL;
}
