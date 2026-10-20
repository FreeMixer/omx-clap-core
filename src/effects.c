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
#include <unistd.h>
#include <jack/jack.h>
#include <jack/midiport.h>
#include <clap/ext/draft/gain-adjustment-metering.h>
#include <clap/factory/preset-discovery.h>
#include <plugin-hostd/protocol.h>

#include "effects.h"
#include "clap_host.h"
#include "clap_meters.h"
#include "hosted_stage_limits.h"
#include "host_transport.h"
#include "layout_pin.h"
#include "lv2_locate.h"
#include "omx_msgring.h"
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

// the output the gain adjustment of clap.gain-adjustment-metering answers to
#define GAIN_ADJUSTMENT_SYMBOL  "gain_adjustment_metering"
#define METER_CHANNELS_MAX      64
#define OUTPUT_SYMBOL_SIZE      (CLAP_NAME_SIZE + 16)

// the LV2 adapter's numbers: its worker's poll and ring are the hosted stage's declared ones (data/hosting.json), the
// same the console's in-process adapter is handed; the smallest atom buffer (0: the adapter's own); the latency hold
#define LV2_ATOM_BUFFER_BYTES   0u
// the adapter's code for a plugin it has no way to run, lv2/core's LV2_CODE_NO_REALISATION
#define HOSTING_NO_REALISATION  "hosting.no-realisation"


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

typedef struct EFFECT_T {
    int instance;
    jack_client_t *jack_client;
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
    int activate_pending;
    pthread_t pending_thread;
    int pending_held;
    int relayout_pending;                   // a reported ports rescan not yet answered (ports_relayout)
    uint32_t relayout_tick;                 // the idle tick of the last relayout
    uint32_t relayouts_unchanged;           // relayouts on consecutive ticks that found the main layout unchanged
    jack_port_t *midi_port;
    struct omx_msgring note_ring;           // control thread -> audio thread: the note verbs' MIDI messages
    void *note_ring_buf;                    // its NOTE_RING_BYTES, NULL for a plugin that takes no notes
    jack_port_t *midi_out_port;             // the plugin's note output, when it has one
    _Atomic uint32_t midi_out_full;         // messages midi_out had no room for
    struct omx_clap_instance *clap;
    uint32_t latency_published;
    const clap_plugin_meters_t *meters;
    const clap_plugin_gain_adjustment_metering_t *gain_adjustment;
    _Atomic uint32_t gain_adjustment_bits;  // the float the audio thread published after its last cycle
    output_t *outputs;
    uint32_t outputs_count;
    const clap_plugin_entry_t *lv2_entry;   // an LV2 plugin: the adapter's entry of its bundle; NULL for a CLAP plugin
    lv2_param_t *lv2_params;
    uint32_t lv2_params_count;
    uint32_t param_events_dropped;          // the library's count of dropped parameter events, as last seen
    clap_id *gestures_open;                 // the parameters whose gesture begin went out on the feedback port, no end yet
    uint32_t gestures_open_count, gestures_open_cap;
    double rate;                            // the graph's rate when the instance was added
    struct host_transport_intent intent;    // the host's transport as this instance's audio thread last read it whole
    struct omx_clap_transport_src transport_src;  // the record the audio thread works out each block and the plugin reads
} effect_t;


/*
************************************************************************************************************************
*           LOCAL GLOBAL VARIABLES
************************************************************************************************************************
*/

static effect_t g_effects[MAX_INSTANCES];
// the layout digest pin_expect named for the next add of an instance, empty when none
static char g_layout_pins[MAX_INSTANCES][PHD_SHA256_HEX_LEN + 1];
static jack_client_t *g_jack_global_client;
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
/* the control thread's idle ticks, counted by effects_idle */
static uint32_t g_idle_ticks;


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

static jack_client_t *open_jack_client(int instance, const char *client_name)
{
    char effect_name[32];
    char requested_name[REQUESTED_CLIENT_NAME_BUF_SIZE];
    jack_client_t *jack_client;
    jack_status_t jack_status;
    size_t name_limit;

    snprintf(effect_name, sizeof(effect_name), "effect_%i", instance);

    if (!client_name || client_name[0] == '\0')
        return jack_client_open(effect_name, JackNoStartServer, &jack_status);

    name_limit = (size_t)jack_client_name_size() - 1;
    sanitize_client_name(requested_name, sizeof(requested_name), client_name, name_limit);
    if (requested_name[0] == '\0')
        return jack_client_open(effect_name, JackNoStartServer, &jack_status);

    jack_client = jack_client_open(requested_name, JackNoStartServer | JackUseExactName, &jack_status);
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

        jack_client = jack_client_open(suffixed_name, JackNoStartServer | JackUseExactName, &jack_status);
    }
    if (!jack_client)
        jack_client = jack_client_open(effect_name, JackNoStartServer, &jack_status);

    return jack_client;
}

static void jack_thread_init(void *arg)
{
    effect_t *effect = arg;

    omx_denormals_off();
    if (effect && effect->clap)
        omx_clap_host_set_audio_thread(effect->clap, pthread_self());
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

static int process(jack_nframes_t nframes, void *arg)
{
    effect_t *effect = arg;
    const float *inputs[CLAP_HOST_PORT_CHANNELS];
    float *outputs[CLAP_HOST_PORT_CHANNELS];
    uint32_t c, runs = 0;

    if (!effect || !effect->clap)
        return 0;

    if (effect->note_ring_buf)
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
    if (effect->midi_port)
    {
        void *midi = jack_port_get_buffer(effect->midi_port, nframes);
        jack_nframes_t count = jack_midi_get_event_count(midi);
        jack_midi_event_t event;
        jack_nframes_t e;

        for (e = 0; e < count; e++)
            if (jack_midi_event_get(&event, midi, e) == 0)
                omx_clap_host_note_in(effect->clap, event.time, event.buffer, event.size);
    }

    // the ports registered at add, never the description: a relayout reads that again while this cycle runs, and the
    // instance is active only while its main ports are these
    for (c = 0; c < effect->in_ports; c++)
        inputs[c] = jack_port_get_buffer(effect->input_ports[c], nframes);
    for (c = 0; c < effect->out_ports; c++)
        outputs[c] = jack_port_get_buffer(effect->output_ports[c], nframes);
    // a cycle while the instance is deactivated (buffer_size re-activating it, JACK stopping this client's cycle for
    // the callback but this thread waiting on nothing of it; a relayout): the audio passes dry, a source's output
    // silent, and the plugin is not run
    if (!effect->clap->active || ((effect->aux_in_count || effect->aux_out_count) && nframes > effect->aux_frames))
    {
        for (c = 0; c < effect->out_ports; c++)
        {
            if (effect->in_ports == 0)
                memset(outputs[c], 0, nframes * sizeof(float));
            else if (outputs[c] != inputs[c < effect->in_ports ? c : effect->in_ports - 1])
                memcpy(outputs[c], inputs[c < effect->in_ports ? c : effect->in_ports - 1], nframes * sizeof(float));
        }
        for (c = 0; c < effect->aux_out_count; c++)
            memset(jack_port_get_buffer(effect->aux_out_ports[c], nframes), 0, nframes * sizeof(float));
        return 0;
    }
    for (c = 0; c < effect->aux_in_count; c++)
        memcpy(effect->aux_in_bufs[c], jack_port_get_buffer(effect->aux_in_ports[c], nframes), nframes * sizeof(float));

    if (effect->gain_adjustment)
        runs = atomic_load_explicit(&effect->clap->stage.h.runs, memory_order_relaxed);
    transport_block(effect);
    omx_clap_host_run_ports(effect->clap, inputs, outputs, nframes);
    for (c = 0; c < effect->aux_out_count; c++)
        memcpy(jack_port_get_buffer(effect->aux_out_ports[c], nframes), effect->aux_out_bufs[c], nframes * sizeof(float));
    if (effect->gain_adjustment)
        publish_gain_adjustment(effect, runs);
    if (effect->midi_out_port)
        notes_out(effect, nframes);
    return 0;
}

/* the plugin's latency on top of what reaches the ports of the other direction */
static void latency(jack_latency_callback_mode_t mode, void *arg)
{
    effect_t *effect = arg;
    jack_port_t **from, **to;
    jack_latency_range_t range, total;
    uint32_t c, from_count, to_count;

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

    total.min += effect->latency_published;
    total.max += effect->latency_published;
    for (c = 0; c < to_count; c++)
        jack_port_set_latency_range(to[c], mode, &total);
}

/* control thread: the figure the latency callback answers with, and a recompute so the graph asks */
static void publish_latency(effect_t *effect)
{
    effect->latency_published = omx_clap_host_latency(effect->clap);
    fprintf(stderr, "effect_%i: latency %u frames\n", effect->instance, effect->latency_published);
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

/* one side's auxiliary ports of the description registered, a JACK port per channel named by layout_port_name, their
 * widths kept: SUCCESS, ERR_MEMORY_ALLOCATION or ERR_JACK_PORT_REGISTER. Before jack_activate: no cycle runs. */
static int aux_register(effect_t *effect, bool is_input)
{
    const struct omx_clap_instance *clap = effect->clap;
    const uint32_t n = is_input ? clap->aux_inputs : clap->aux_outputs;
    const struct omx_clap_port *ports = is_input ? clap->aux_in : clap->aux_out;
    jack_port_t **jack;
    uint32_t *widths;
    uint32_t p, k, c = 0, count = 0;
    char name[LAYOUT_PORT_NAME_SIZE];

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
    {
        for (k = 0; k < ports[p].channels; k++)
        {
            layout_port_name(name, is_input, p, k);
            jack[c] = jack_port_register(effect->jack_client, name, JACK_DEFAULT_AUDIO_TYPE,
                                         is_input ? JackPortIsInput : JackPortIsOutput, 0);
            if (!jack[c++])
                return ERR_JACK_PORT_REGISTER;
        }
        widths[p] = ports[p].channels;
    }
    if (is_input)
        effect->aux_in_count = count, effect->aux_in_n = n;
    else
        effect->aux_out_count = count, effect->aux_out_n = n;
    return SUCCESS;
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

/* One effect is one JACK client, so a change of the buffer size calls this on as many client threads at once as there
 * are effects. A re-activation is a main-thread call of the plugin (CLAP's own rule) and, for an LV2 plugin, a lilv call
 * on the adapter's one world (the default state is loaded at activate), and neither bears two threads at a time: two
 * effects re-activating together crashed in sord's node table (jack_lv2_e2e, the shrink). One lock serialises them. */
static pthread_mutex_t g_reactivate = PTHREAD_MUTEX_INITIALIZER;

/* jack stops this client's process cycle while this runs, so this thread can take the audio thread's place: the
 * auxiliary blocks are made again at the new size and bound, and an active instance re-activated at it */
static int buffer_size(jack_nframes_t nframes, void *arg)
{
    effect_t *effect = arg;
    int active, reactivate, reblock;

    if (!effect || !effect->clap)
        return 0;
    // any change, a shrink too: the adapter promised the plugin the activation's length (bufsz:fixedBlockLength) and
    // refuses a cycle of another one, so an instance left at the old size would run no block at all
    pthread_mutex_lock(&g_reactivate);
    active = effect->clap->active;
    reactivate = active && nframes != effect->clap->max_block;
    reblock = (effect->aux_in_count || effect->aux_out_count) && nframes != effect->aux_frames;
    if (reactivate || reblock || effect->activate_pending)
    {
        // stop takes the audio role for this thread and releases it: the process thread's is given back after; a
        // pending relayout's is the one the relayout saved
        const pthread_t thread = active ? effect->clap->audio_thread : effect->pending_thread;
        const int held = active ? effect->clap->audio_role_held : effect->pending_held;
        const int activate = active || effect->activate_pending;

        effect->activate_pending = 0;
        if (active)
        {
            omx_clap_host_stop(effect->clap);
            omx_clap_host_deactivate(effect->clap);
        }
        // a block that cannot be made leaves the ports unbound and every longer cycle dry (process)
        if (reblock && aux_blocks(effect, nframes) != 0)
            fprintf(stderr, "effect_%i: its auxiliary blocks cannot be made for %u frames; its auxiliary ports are unbound "
                    "and it passes dry\n", effect->instance, nframes);
        if (activate && omx_clap_host_activate(effect->clap, jack_get_sample_rate(effect->jack_client), nframes, NULL) == 0)
        {
            if (held)
                omx_clap_host_set_audio_thread(effect->clap, thread);
            omx_clap_host_arm(effect->clap);
        }
    }
    pthread_mutex_unlock(&g_reactivate);
    return 0;
}

static void instance_free(effect_t *effect)
{
    // the client goes first: no cycle can run the stage while the plugin is stopped and destroyed
    if (effect->jack_client)
        jack_client_close(effect->jack_client);
    if (effect->clap)
    {
        omx_clap_host_stop(effect->clap);
        omx_clap_host_close(effect->clap);
    }
    // the entry outlives the plugin made from it
    if (effect->lv2_entry)
        omx_clap_lv2_entry_release(effect->lv2_entry);
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
        fprintf(stderr, "%s: %s: the LV2 adapter is not configured\n", uri, HOSTING_NO_REALISATION);
        snprintf(g_add_reason, sizeof(g_add_reason), "%s", HOSTING_NO_REALISATION);
        return ERR_LV2_INSTANTIATION;
    }
    if (!lv2_locate(NULL, uri, lv2_candidate, &search))
    {
        if (search.why[0])
        {
            // a bundle that may hold it could not be read: lilv missing, or the bundle broken
            fprintf(stderr, "%s: %s\n", uri, search.why);
            snprintf(g_add_reason, sizeof(g_add_reason), "%s", search.why);
            return ERR_LV2_INSTANTIATION;
        }
        fprintf(stderr, "%s: no bundle on the LV2 path holds it\n", uri);
        return ERR_LV2_INVALID_URI;
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
 * is asked. The instance is unpublished and deactivated (a restart whose deactivate made the rescan left it inactive
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
    thread = clap->audio_thread;
    held = clap->audio_role_held;
    in_channels = clap->in_channels;
    channels = clap->channels;
    in_kind = clap->main_in.kind;
    out_kind = clap->main_out.kind;
    pthread_mutex_lock(&g_reactivate);
    if (clap->active)
    {
        if (omx_clap_host_unpublish(clap, CLAP_HOST_ROLE_POLL_US, CLAP_HOST_ROLE_TIMEOUT_US) != 0)
        {
            pthread_mutex_unlock(&g_reactivate);
            return;
        }
        omx_clap_host_stop(clap);
        omx_clap_host_deactivate(clap);
    }
    effect->relayout_pending = 0;
    // a relayout still waiting on its blocks keeps the audio role it saved; every outcome below decides again, and only
    // the short-block one waits again
    if (effect->activate_pending)
    {
        thread = effect->pending_thread;
        held = effect->pending_held;
    }
    effect->activate_pending = 0;
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
        effect->activate_pending = 1;
        effect->pending_thread = thread;
        effect->pending_held = held;
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
    pthread_mutex_unlock(&g_reactivate);
}

/* the control thread's tick of one effect: what the plugin asked of the host, its log, a restart, the writes no cycle took */
static void clap_idle(struct omx_clap_instance *clap)
{
    const int restart = omx_clap_host_tick(clap);
    const char *log = omx_clap_host_log_take(clap);

    if (log)
        fprintf(stderr, "%s: %s\n", clap->desc->id, log);
    if (restart && clap->active)
        omx_clap_host_restart(clap);
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
    // Under JACK every process() gets the buffer size and any change of it, up or down, re-activates (buffer_size
    // above), so within one activation the block length is fixed: declared, and the adapter refuses a cycle of another
    // length, counted apart from the plugin's own faults (#192).
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

int effects_init(void)
{
    if (configure_core() != 0)
    {
        fprintf(stderr, "can't configure the clap core\n");
        return ERR_INSTANCE_INVALID;
    }
    configure_lv2();
    g_jack_global_client = jack_client_open("omx-clap-host", JackNoStartServer, NULL);
    if (!g_jack_global_client)
    {
        fprintf(stderr, "can't get jack client\n");
        return ERR_JACK_CLIENT_CREATION;
    }
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

int effects_add(const char *uri, int instance, const char *client_name)
{
    char path[PATH_MAX];
    char port_name[LAYOUT_PORT_NAME_SIZE];
    char why[OMX_CLAP_WHY_MAX];
    char pin[PHD_SHA256_HEX_LEN + 1];
    const char *id, *lv2_uri;
    effect_t *effect;
    uint32_t c;
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

    effect->jack_client = open_jack_client(instance, client_name);
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
        error = PHD_ERR_PIN_LAYOUT_MISMATCH;
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

    for (c = 0; c < effect->clap->in_channels; c++)
    {
        layout_port_name(port_name, true, LAYOUT_PORT_MAIN, c);
        effect->input_ports[c] = jack_port_register(effect->jack_client, port_name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
        if (!effect->input_ports[c])
        {
            error = ERR_JACK_PORT_REGISTER;
            goto error;
        }
    }
    effect->in_ports = effect->clap->in_channels;
    for (c = 0; c < effect->clap->channels; c++)
    {
        layout_port_name(port_name, false, LAYOUT_PORT_MAIN, c);
        effect->output_ports[c] = jack_port_register(effect->jack_client, port_name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        if (!effect->output_ports[c])
        {
            error = ERR_JACK_PORT_REGISTER;
            goto error;
        }
    }
    effect->out_ports = effect->clap->channels;
    if ((error = aux_register(effect, true)) != SUCCESS || (error = aux_register(effect, false)) != SUCCESS)
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

    jack_set_thread_init_callback(effect->jack_client, jack_thread_init, effect);
    jack_set_process_callback(effect->jack_client, process, effect);
    jack_set_buffer_size_callback(effect->jack_client, buffer_size, effect);
    jack_set_latency_callback(effect->jack_client, latency, effect);

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
        return PHD_ERR_PIN_ABSENT;
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

    for (j = start; j < end; j++)
    {
        effect_t *effect = &g_effects[j];

        if (!instance_exist(j))
            continue;
        if (jack_deactivate(effect->jack_client) != 0)
            return ERR_JACK_CLIENT_DEACTIVATION;
        instance_free(effect);
    }

    return SUCCESS;
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

int effects_preset_load(int effect_id, const char *location)
{
    struct omx_clap_instance *clap;

    if (!instance_exist(effect_id))
        return ERR_INSTANCE_NON_EXISTS;
    clap = g_effects[effect_id].clap;
    if (!clap->preset_load)
        return ERR_INVALID_OPERATION;
    // an LV2 preset is named by its URI, as mod-host takes it: a preset of the plugin's bundle, the URI its load key
    if (g_effects[effect_id].lv2_entry)
        return clap->preset_load->from_location(clap->plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, NULL, location)
            ? SUCCESS : ERR_LV2_INVALID_PRESET_URI;
    return omx_clap_host_preset_load(clap, location) == 0 ? SUCCESS : ERR_LV2_INVALID_PRESET_URI;
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

/* plugin-hostd's code for a note verb its worker had no room to queue; declared by plugin-hostd 0.5 (FreeMixer/plugin-hostd#91),
 * the same number here until that header is the floor */
#ifndef PHD_ERR_NOTE_QUEUE_FULL
#define PHD_ERR_NOTE_QUEUE_FULL (-517)
#endif

/* note_input: "resp 0 midi_in" for an instance that takes notes, "resp 0 -" for one that takes none */
int effects_note_input(int effect_id, char *reply, size_t size)
{
    if (!instance_exist(effect_id))
        return ERR_INSTANCE_NON_EXISTS;
    snprintf(reply, size, "resp 0 %s", g_effects[effect_id].note_ring_buf ? MIDI_PORT_NAME : PHD_NO_NOTE_INPUT);
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
        return PHD_ERR_NO_NOTE_INPUT;
    if (NOTE_RING_BYTES - omx_msgring_used(&effect->note_ring) < count * omx_msgring_record_bytes(3) + omx_msgring_record_bytes(3))
        return PHD_ERR_NOTE_QUEUE_FULL;     // the audio thread has not drained the ring: no room, nothing queued
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
    for (i = 0; i < MAX_PLUGIN_INSTANCES; i++)
    {
        effect_t *effect = &g_effects[i];

        if (!instance_exist(i))
            continue;
        clap_idle(effect->clap);
        ports_relayout(effect);
        if (omx_clap_host_remote_controls_changed(effect->clap))
        {
            char line[64];

            snprintf(line, sizeof(line), PHD_EVENT_REMOTE_PAGES_CHANGED_FMT, effect->instance);
            socket_send_feedback(line);
        }
        if (omx_clap_host_latency(effect->clap) != effect->latency_published)
            publish_latency(effect);
        param_events_notify(effect);
        outputs_notify(effect);
    }
}

/* the CLAP flag of a strip kind of PHD_TRACK_KINDS, 0 for an input channel */
#define TRACK_KIND_FLAG_BUS     CLAP_TRACK_INFO_IS_FOR_BUS
#define TRACK_KIND_FLAG_RETURN  CLAP_TRACK_INFO_IS_FOR_RETURN_TRACK
#define TRACK_KIND_FLAG_MASTER  CLAP_TRACK_INFO_IS_FOR_MASTER

static uint64_t track_kind_flag(const char *kind)
{
#define X(id, name) if (kind && !strcmp(kind, name)) return TRACK_KIND_FLAG_##id;
    PHD_TRACK_KINDS(X)
#undef X
    return 0;
}

/* track_info: the words already checked against the grammar (phd_track_info_valid) */
int effects_track_info(int effect_id, const char *name, const char *color, const char *kind)
{
    clap_color_t rgb;
    unsigned long value;

    if (!instance_exist(effect_id))
        return ERR_INSTANCE_NON_EXISTS;
    if (strcmp(color, PHD_NO_COLOR) != 0)
    {
        value = strtoul(color + 1, NULL, 16);
        rgb.alpha = 255;
        rgb.red = (uint8_t)(value >> 16);
        rgb.green = (uint8_t)(value >> 8);
        rgb.blue = (uint8_t)value;
    }
    if (omx_clap_host_track_info_set(g_effects[effect_id].clap, name,
                                     strcmp(color, PHD_NO_COLOR) ? &rgb : NULL, track_kind_flag(kind)) != 0)
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
 * PHD_STRING_MAX bytes, '"' written \" and a backslash \\ */
static void quoted(char *out, size_t size, const char *name)
{
    char clean[PHD_STRING_MAX + 1];
    size_t n = 0, o = 0;

    for (; name[n] && n < PHD_STRING_MAX; n++)
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

/* remote_page_get: the page's reply into `reply`; a slot is the id param_set takes, PHD_EMPTY_SLOT for an empty one,
 * an id params does not list, and an id param_set would refuse */
int effects_remote_page_get(int effect_id, int page, char *reply, size_t size)
{
    struct omx_clap_instance *clap;
    clap_remote_controls_page_t info;
    char section[2 * PHD_STRING_MAX + 3], name[2 * PHD_STRING_MAX + 3];
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
    for (i = 0; i < PHD_REMOTE_PAGE_SLOTS && used < size; i++)
    {
        const clap_id id = info.param_ids[i];

        if (id == CLAP_INVALID_ID || !omx_clap_host_param_is_row(clap, id))
            used += snprintf(reply + used, size - used, " %s", PHD_EMPTY_SLOT);
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
        return PHD_ERR_NO_PARAM_CONTRACT;
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
        // an LV2 group declares no normalisation, and a CLAP plugin without clap.ambisonic none at all: the host's
        t.assumed = effect->lv2_entry != NULL || !clap->ambisonic || !clap->ambisonic->get_config;
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

    if (!instance_exist(effect_id))
        return ERR_INSTANCE_NON_EXISTS;
    effect = &g_effects[effect_id];
    *reply = NULL;
    out = open_memstream(reply, &size);
    if (!out)
        return ERR_MEMORY_ALLOCATION;
    // the ports registered at add, inputs first and each side's main port first; dry while the instance is not active
    fputs(effect->clap->active ? "resp 0" : "resp 0 dry", out);
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
