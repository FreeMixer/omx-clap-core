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
#include <plugin-hostd/protocol.h>

#include "effects.h"
#include "clap_host.h"
#include "omx_clap_ext.h"
#include "layout_pin.h"
#include "host-dispatch.h"


/*
************************************************************************************************************************
*           LOCAL DEFINES
************************************************************************************************************************
*/

#define REMOVE_ALL              (-1)
#define URI_SCHEME              "clap:"
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


/*
************************************************************************************************************************
*           LOCAL DATA TYPES
************************************************************************************************************************
*/

/* one value a plugin reports, answered to by its symbol: a channel of an org.openmixer.meters/1 meter, or the gain
 * adjustment of clap.gain-adjustment-metering/0 */
typedef struct OUTPUT_T {
    char symbol[OUTPUT_SYMBOL_SIZE];
    char name[CLAP_NAME_SIZE];  // the meter's, for a refusal to name
    int gain_adjustment;        // the standard value, not a meter
    clap_id meter;
    uint32_t channel;
    uint32_t channels;          // the meter's: what read fills
    int monitored;
    int notified;               // a value went out since monitor_output
    float value;                // the last value that went out
} output_t;

typedef struct EFFECT_T {
    int instance;
    jack_client_t *jack_client;
    jack_port_t *input_ports[CLAP_HOST_MAIN_PORT_CHANNELS];
    jack_port_t *output_ports[CLAP_HOST_MAIN_PORT_CHANNELS];
    jack_port_t *midi_port;
    struct omx_clap_instance *clap;
    uint32_t latency_published;
    const omx_clap_plugin_meters_t *meters;
    const clap_plugin_gain_adjustment_metering_t *gain_adjustment;
    _Atomic uint32_t gain_adjustment_bits;  // the float the audio thread published after its last cycle
    output_t *outputs;
    uint32_t outputs_count;
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

    omx_hosted_denormals_off();
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
                omx_clap_note_in(&effect->clap->stage, event.time, event.buffer, event.size);
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
    free(effect->outputs);
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
        return ERR_LV2_INSTANTIATION;
    }
    return SUCCESS;
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

/* control thread, at load: the plugin's org.openmixer.meters/1, else its clap.gain-adjustment-metering/0, each value an
 * output symbol. A meter that can't be read or a symbol that is taken refuses the plugin. */
static int outputs_build(effect_t *effect)
{
    const clap_plugin_t *plugin = effect->clap->plugin;
    const omx_clap_plugin_meters_t *meters = plugin->get_extension(plugin, OMX_CLAP_EXT_METERS);
    const clap_plugin_gain_adjustment_metering_t *gain_adjustment;
    omx_clap_meter_info_t info;
    uint32_t count, i, c;

    if (meters && meters->count && meters->get_info && meters->read)
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

/* control thread, at the idle rate: each monitored output whose value moved, and the first value of one just monitored,
 * goes out as output_set on the feedback socket; a move is mod-host's for an output port, FLT_EPSILON or more */
static void outputs_notify(effect_t *effect)
{
    float values[METER_CHANNELS_MAX];
    clap_id read_meter = CLAP_INVALID_ID;
    int read = 0, have = 0;
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
        else
        {
            if (!read || output->meter != read_meter)
            {
                read = 1;
                read_meter = output->meter;
                have = effect->meters->read(effect->clap->plugin, output->meter, values, output->channels);
            }
            if (!have)
                continue;
            value = values[output->channel];
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
    config.name = "omx-clap-host";
    config.vendor = "Pau Aliagas";
    config.url = "";
    return omx_clap_host_configure(&config);
}

int effects_init(void)
{
    if (configure_core() != 0)
    {
        fprintf(stderr, "can't configure the clap core\n");
        return ERR_INSTANCE_INVALID;
    }
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

int effects_add(const char *uri, int instance, const char *client_name)
{
    char path[PATH_MAX];
    char port_name[32];
    char why[OMX_CLAP_WHY_MAX];
    char pin[PHD_SHA256_HEX_LEN + 1];
    const char *id;
    effect_t *effect;
    uint32_t c;
    int error;

    if (parse_uri(uri, path, sizeof(path), &id) != 0)
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

    error = open_clap(path, id, &effect->clap);
    if (error != SUCCESS)
        goto error;
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
        fprintf(stderr, "%s: %s\n", id, why);
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

int effects_set_parameter(int effect_id, const char *control_symbol, float value)
{
    clap_id id;

    if (!instance_exist(effect_id))
        return ERR_INSTANCE_NON_EXISTS;
    if (strcmp(control_symbol, BYPASS_PORT_SYMBOL) == 0)
    {
        omx_clap_host_bypass(g_effects[effect_id].clap, value > 0.5f);
        return SUCCESS;
    }
    if (parse_param_id(control_symbol, &id) != 0)
        return ERR_LV2_INVALID_PARAM_SYMBOL;
    return clap_param_set(g_effects[effect_id].clap, id, value);
}

int effects_get_parameter(int effect_id, const char *control_symbol, float *value)
{
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
    if (parse_param_id(control_symbol, &id) != 0)
        return ERR_LV2_INVALID_PARAM_SYMBOL;

    ret = clap_param_get(g_effects[effect_id].clap, id, &v);
    if (ret == SUCCESS)
        *value = (float)v;
    return ret;
}

int effects_preset_load(int effect_id, const char *location)
{
    if (!instance_exist(effect_id))
        return ERR_INSTANCE_NON_EXISTS;
    if (!g_effects[effect_id].clap->preset_load)
        return ERR_INVALID_OPERATION;
    return omx_clap_host_preset_load(g_effects[effect_id].clap, location) == 0 ? SUCCESS : ERR_LV2_INVALID_PRESET_URI;
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

void effects_idle(void)
{
    int i;

    for (i = 0; i < MAX_PLUGIN_INSTANCES; i++)
    {
        effect_t *effect = &g_effects[i];

        if (!instance_exist(i))
            continue;
        clap_idle(effect->clap);
        if (omx_clap_host_latency(effect->clap) != effect->latency_published)
            publish_latency(effect);
        outputs_notify(effect);
    }
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
