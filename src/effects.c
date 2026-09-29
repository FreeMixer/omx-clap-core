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
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <jack/jack.h>

#include "effects.h"
#include "clap_host.h"


/*
************************************************************************************************************************
*           LOCAL DEFINES
************************************************************************************************************************
*/

#define REMOVE_ALL              (-1)
#define URI_SCHEME              "clap:"
#define BYPASS_PORT_SYMBOL      ":bypass"
#define STATE_FILE_SUFFIX       ".clapstate"
#define INSTANCE_IS_VALID(id)   ((id) >= 0 && (id) < MAX_INSTANCES)
#define REQUESTED_CLIENT_NAME_BUF_SIZE  256


/*
************************************************************************************************************************
*           LOCAL DATA TYPES
************************************************************************************************************************
*/

typedef struct EFFECT_T {
    int instance;
    jack_client_t *jack_client;
    jack_port_t *input_ports[CLAP_HOST_MAX_CHANNELS];
    jack_port_t *output_ports[CLAP_HOST_MAX_CHANNELS];
    clap_instance_t *clap;
    uint32_t latency_published;
} effect_t;


/*
************************************************************************************************************************
*           LOCAL GLOBAL VARIABLES
************************************************************************************************************************
*/

static effect_t g_effects[MAX_INSTANCES];
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

    clap_host_denormals_off();
    if (effect && effect->clap)
        clap_host_set_audio_thread(effect->clap, pthread_self());
}

static int process(jack_nframes_t nframes, void *arg)
{
    effect_t *effect = arg;
    const float *inputs[CLAP_HOST_MAX_CHANNELS];
    float *outputs[CLAP_HOST_MAX_CHANNELS];
    uint32_t c;

    if (!effect || !effect->clap)
        return 0;

    for (c = 0; c < effect->clap->input_channels; c++)
        inputs[c] = jack_port_get_buffer(effect->input_ports[c], nframes);
    for (c = 0; c < effect->clap->output_channels; c++)
        outputs[c] = jack_port_get_buffer(effect->output_ports[c], nframes);

    clap_host_run(effect->clap, inputs, outputs, nframes);
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
        from_count = effect->clap->input_channels;
        to = effect->output_ports;
        to_count = effect->clap->output_channels;
    }
    else
    {
        from = effect->output_ports;
        from_count = effect->clap->output_channels;
        to = effect->input_ports;
        to_count = effect->clap->input_channels;
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
    effect->latency_published = effect->clap->latency_frames;
    fprintf(stderr, "effect_%i: latency %u frames\n", effect->instance, effect->latency_published);
    jack_recompute_total_latencies(effect->jack_client);
}

/* jack stops the process cycle while this runs, so the control thread can take the audio thread's place */
static int buffer_size(jack_nframes_t nframes, void *arg)
{
    effect_t *effect = arg;

    if (effect && effect->clap && effect->clap->active && nframes > effect->clap->max_frames)
    {
        clap_host_stop(effect->clap);
        clap_host_deactivate(effect->clap);
        if (clap_host_activate(effect->clap, jack_get_sample_rate(effect->jack_client), nframes) == SUCCESS)
            clap_host_arm(effect->clap);
    }
    return 0;
}

static void instance_free(effect_t *effect)
{
    if (effect->clap)
    {
        clap_host_stop(effect->clap);
        clap_host_close(effect->clap);
    }
    if (effect->jack_client)
        jack_client_close(effect->jack_client);
    memset(effect, 0, sizeof(effect_t));
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

static void state_filename(char *buffer, size_t size, const char *dir, int instance)
{
    snprintf(buffer, size, "%s/effect_%i%s", dir, instance, STATE_FILE_SUFFIX);
}


/*
************************************************************************************************************************
*           GLOBAL FUNCTIONS
************************************************************************************************************************
*/

int effects_init(void)
{
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

    effect = &g_effects[instance];
    memset(effect, 0, sizeof(effect_t));
    effect->instance = instance;

    effect->jack_client = open_jack_client(instance, client_name);
    if (!effect->jack_client)
    {
        fprintf(stderr, "can't get jack client\n");
        return ERR_JACK_CLIENT_CREATION;
    }

    error = clap_host_open(path, id, &effect->clap);
    if (error != SUCCESS)
        goto error;

    for (c = 0; c < effect->clap->input_channels; c++)
    {
        snprintf(port_name, sizeof(port_name), "in_%u", c + 1);
        effect->input_ports[c] = jack_port_register(effect->jack_client, port_name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
        if (!effect->input_ports[c])
        {
            error = ERR_JACK_PORT_REGISTER;
            goto error;
        }
    }
    for (c = 0; c < effect->clap->output_channels; c++)
    {
        snprintf(port_name, sizeof(port_name), "out_%u", c + 1);
        effect->output_ports[c] = jack_port_register(effect->jack_client, port_name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        if (!effect->output_ports[c])
        {
            error = ERR_JACK_PORT_REGISTER;
            goto error;
        }
    }

    jack_set_thread_init_callback(effect->jack_client, jack_thread_init, effect);
    jack_set_process_callback(effect->jack_client, process, effect);
    jack_set_buffer_size_callback(effect->jack_client, buffer_size, effect);
    jack_set_latency_callback(effect->jack_client, latency, effect);

    error = clap_host_activate(effect->clap, jack_get_sample_rate(effect->jack_client), jack_get_buffer_size(effect->jack_client));
    if (error != SUCCESS)
        goto error;
    clap_host_arm(effect->clap);

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
    return clap_host_bypass(g_effects[effect_id].clap, value);
}

int effects_set_parameter(int effect_id, const char *control_symbol, float value)
{
    clap_id id;

    if (!instance_exist(effect_id))
        return ERR_INSTANCE_NON_EXISTS;
    if (strcmp(control_symbol, BYPASS_PORT_SYMBOL) == 0)
        return clap_host_bypass(g_effects[effect_id].clap, value > 0.5f);
    if (parse_param_id(control_symbol, &id) != 0)
        return ERR_LV2_INVALID_PARAM_SYMBOL;
    return clap_host_param_set(g_effects[effect_id].clap, id, value);
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
        *value = clap_host_bypassed(g_effects[effect_id].clap) ? 1.0f : 0.0f;
        return SUCCESS;
    }
    if (parse_param_id(control_symbol, &id) != 0)
        return ERR_LV2_INVALID_PARAM_SYMBOL;

    ret = clap_host_param_get(g_effects[effect_id].clap, id, &v);
    if (ret == SUCCESS)
        *value = (float)v;
    return ret;
}

int effects_preset_load(int effect_id, const char *location)
{
    if (!instance_exist(effect_id))
        return ERR_INSTANCE_NON_EXISTS;
    return clap_host_preset_load(g_effects[effect_id].clap, location);
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
        ret = clap_host_state_save(g_effects[i].clap, filename);
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
        ret = clap_host_state_load(g_effects[i].clap, filename);
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
        clap_host_idle(effect->clap);
        if (effect->clap->latency_frames != effect->latency_published)
            publish_latency(effect);
    }
}
