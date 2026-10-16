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
#include "layout_pin.h"
#include "lv2_locate.h"
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
#define STATE_FILE_SUFFIX       ".clapstate"
#define INSTANCE_IS_VALID(id)   ((id) >= 0 && (id) < MAX_INSTANCES)
#define REQUESTED_CLIENT_NAME_BUF_SIZE  256

// a write waits for the next cycle this long before the control thread delivers it itself
#define QUEUE_SETTLE_US         50000

// the output the gain adjustment of clap.gain-adjustment-metering answers to
#define GAIN_ADJUSTMENT_SYMBOL  "gain_adjustment_metering"
#define METER_CHANNELS_MAX      64
#define OUTPUT_SYMBOL_SIZE      (CLAP_NAME_SIZE + 16)

// the LV2 adapter's numbers: its worker's poll and ring, the smallest atom buffer (0: the adapter's own), the latency hold
#define LV2_WORKER_POLL_US      1000u
#define LV2_WORKER_RING_BYTES   4096u
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
    jack_port_t *input_ports[CLAP_HOST_MAIN_PORT_CHANNELS];
    jack_port_t *output_ports[CLAP_HOST_MAIN_PORT_CHANNELS];
    jack_port_t *midi_port;
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

static int process(jack_nframes_t nframes, void *arg)
{
    effect_t *effect = arg;
    const float *inputs[CLAP_HOST_MAIN_PORT_CHANNELS];
    float *outputs[CLAP_HOST_MAIN_PORT_CHANNELS];
    uint32_t c, runs = 0;

    if (!effect || !effect->clap)
        return 0;

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

    for (c = 0; c < effect->clap->in_channels; c++)
        inputs[c] = jack_port_get_buffer(effect->input_ports[c], nframes);
    for (c = 0; c < effect->clap->channels; c++)
        outputs[c] = jack_port_get_buffer(effect->output_ports[c], nframes);

    if (effect->gain_adjustment)
        runs = atomic_load_explicit(&effect->clap->stage.h.runs, memory_order_relaxed);
    omx_clap_run_io(&effect->clap->stage, inputs, outputs, nframes);
    if (effect->gain_adjustment)
        publish_gain_adjustment(effect, runs);
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
        from_count = effect->clap->in_channels;
        to = effect->output_ports;
        to_count = effect->clap->channels;
    }
    else
    {
        from = effect->output_ports;
        from_count = effect->clap->channels;
        to = effect->input_ports;
        to_count = effect->clap->in_channels;
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

/* jack stops the process cycle while this runs, so the control thread can take the audio thread's place */
static int buffer_size(jack_nframes_t nframes, void *arg)
{
    effect_t *effect = arg;

    if (effect && effect->clap && effect->clap->active && nframes > effect->clap->max_block)
    {
        omx_clap_host_stop(effect->clap);
        omx_clap_host_deactivate(effect->clap);
        if (omx_clap_host_activate(effect->clap, jack_get_sample_rate(effect->jack_client), nframes, NULL) == 0)
            omx_clap_host_arm(effect->clap);
    }
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
    free(effect->outputs);
    free(effect->lv2_params);
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

/* control thread, at the idle rate: the values the plugin moved by itself, the last of each parameter per batch, and its
 * gestures, in order (a value held back goes out before a gesture on the same parameter). Only a row is reported: a
 * read-only parameter is an output, monitor_output's. When the library dropped events for want of room, every row's
 * value goes out as the plugin reads it now, so the controller misses no final value. */
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
            {
                char line[128], name[16];

                snprintf(line, sizeof(line), PARAM_GESTURE_FEEDBACK, effect->instance,
                         param_name(effect, ev[i].id, name, sizeof(name)),
                         ev[i].kind == OMX_CLAP_PARAM_EVENT_GESTURE_BEGIN ? "begin" : "end");
                socket_send_feedback(line);
            }
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
    config.worker_poll_us = LV2_WORKER_POLL_US;
    config.worker_ring_bytes = LV2_WORKER_RING_BYTES;
    config.atom_buffer_bytes = LV2_ATOM_BUFFER_BYTES;
    config.latency_hold_ms = CLAP_HOST_LATENCY_HOLD_MS;
    config.lilv_soname = NULL;
    config.state_root = root;
    return omx_clap_lv2_configure(&config, why);
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
    char port_name[32];
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

    for (c = 0; c < effect->clap->in_channels; c++)
    {
        snprintf(port_name, sizeof(port_name), "in_%u", c + 1);
        effect->input_ports[c] = jack_port_register(effect->jack_client, port_name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
        if (!effect->input_ports[c])
        {
            error = ERR_JACK_PORT_REGISTER;
            goto error;
        }
    }
    for (c = 0; c < effect->clap->channels; c++)
    {
        snprintf(port_name, sizeof(port_name), "out_%u", c + 1);
        effect->output_ports[c] = jack_port_register(effect->jack_client, port_name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        if (!effect->output_ports[c])
        {
            error = ERR_JACK_PORT_REGISTER;
            goto error;
        }
    }

    if (effect->clap->note_inputs)
    {
        effect->midi_port = jack_port_register(effect->jack_client, MIDI_PORT_NAME, JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
        if (!effect->midi_port)
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

void effects_idle(void)
{
    int i;

    for (i = 0; i < MAX_PLUGIN_INSTANCES; i++)
    {
        effect_t *effect = &g_effects[i];

        if (!instance_exist(i))
            continue;
        clap_idle(effect->clap);
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
 * PHD_STRING_MAX bytes, '"' written \" */
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
        if (clean[n] == '"')
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
