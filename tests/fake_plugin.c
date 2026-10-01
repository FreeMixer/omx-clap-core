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

/* A .clap holding the port layouts the host refuses and one it accepts:
 *   org.omx-clap-host.test.wide        one main pair, 4 channels each
 *   org.omx-clap-host.test.sidechain   a stereo main pair and one more input
 *   org.omx-clap-host.test.notes       a stereo main pair and a note input
 *   org.omx-clap-host.test.widen       a mono main input and a stereo main output
 *   org.omx-clap-host.test.auxout      a stereo main pair and a stereo auxiliary output
 *   org.omx-clap-host.test.passthrough stereo in to stereo out, parameter 0
 *                                      is the latency it reports; a write
 *                                      makes it ask for a restart and
 *                                      announce the change
 * Every plugin reads back three read-only parameters: 1 the calls of activate and 2 the calls of process, which is how a
 * test sees a warm-up, and 3 whether the host it was created in offers the preset-load extension.
 * FAKE_LAYOUT_DEFAULT is the default parameter 0 declares (64 without it): the same binary with another layout.
 * FAKE_LOG names a file each instance appends "<id> init", "<id> activate", "<id> process" (its first call only) and
 * "<id> destroy" to, which outlives an instance the host destroyed.
 * passthrough also has clap.track-info, whose changed appends "<id> track_info <got> <flags> <a,r,g,b> <name>" with what
 * the host's get answered and, told it is on a bus, calls the host's remote_controls.changed; and clap.remote-controls
 * with two pages, a get past them appending "<id> remote_controls.get past the count". A call of either from any thread
 * but the one that ran init appends "<id> wrong thread <what>". */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <clap/clap.h>

#define ID_WIDE         "org.omx-clap-host.test.wide"
#define ID_SIDECHAIN    "org.omx-clap-host.test.sidechain"
#define ID_NOTES        "org.omx-clap-host.test.notes"
#define ID_PASSTHROUGH  "org.omx-clap-host.test.passthrough"
#define ID_WIDEN        "org.omx-clap-host.test.widen"
#define ID_AUXOUT       "org.omx-clap-host.test.auxout"
#define LATENCY_DEFAULT 64.0
#define LATENCY_MAX     4096.0

typedef struct FAKE_T {
    clap_plugin_t plugin;
    const clap_host_t *host;
    uint32_t main_channels;
    uint32_t out_channels;
    uint32_t extra_inputs;
    uint32_t extra_outputs;
    uint32_t note_inputs;
    uint32_t latency;
    double latency_param;
    uint32_t activations;
    uint32_t process_calls;
    uint32_t host_preset_load;
    int logged_process;
    pthread_t init_thread;
} fake_t;

static const char *const g_features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL };

#define DESCRIPTOR(name, plugin_id) \
    { CLAP_VERSION_INIT, plugin_id, name, "omx-clap-host", "", "", "", "0", name, g_features }

static const clap_plugin_descriptor_t g_descriptors[] = {
    DESCRIPTOR("wide", ID_WIDE),
    DESCRIPTOR("sidechain", ID_SIDECHAIN),
    DESCRIPTOR("notes", ID_NOTES),
    DESCRIPTOR("passthrough", ID_PASSTHROUGH),
    DESCRIPTOR("widen", ID_WIDEN),
    DESCRIPTOR("auxout", ID_AUXOUT),
};

#define DESCRIPTOR_COUNT (sizeof(g_descriptors) / sizeof(g_descriptors[0]))

static void fake_log(const clap_plugin_t *plugin, const char *what)
{
    const char *path = getenv("FAKE_LOG");
    FILE *f;

    if (!path || !(f = fopen(path, "a")))
        return;
    fprintf(f, "%s %s\n", plugin->desc->id, what);
    fclose(f);
}

static double layout_default(void)
{
    const char *value = getenv("FAKE_LAYOUT_DEFAULT");

    return value && *value ? atof(value) : LATENCY_DEFAULT;
}

static uint32_t audio_ports_count(const clap_plugin_t *plugin, bool is_input)
{
    const fake_t *fake = plugin->plugin_data;
    return 1 + (is_input ? fake->extra_inputs : fake->extra_outputs);
}

static bool audio_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_audio_port_info_t *info)
{
    const fake_t *fake = plugin->plugin_data;

    if (index >= audio_ports_count(plugin, is_input))
        return false;
    memset(info, 0, sizeof(*info));
    info->id = index;
    snprintf(info->name, sizeof(info->name), "%s %u", is_input ? "in" : "out", index);
    info->flags = index == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0;
    info->channel_count = index == 0 ? (is_input ? fake->main_channels : fake->out_channels) : (is_input ? 1 : 2);
    info->port_type = info->channel_count == 2 ? CLAP_PORT_STEREO : (info->channel_count == 1 ? CLAP_PORT_MONO : NULL);
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t g_audio_ports = { audio_ports_count, audio_ports_get };

static uint32_t note_ports_count(const clap_plugin_t *plugin, bool is_input)
{
    const fake_t *fake = plugin->plugin_data;
    return is_input ? fake->note_inputs : 0;
}

static bool note_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_note_port_info_t *info)
{
    if (index >= note_ports_count(plugin, is_input))
        return false;
    memset(info, 0, sizeof(*info));
    info->id = index;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    strcpy(info->name, "notes");
    return true;
}

static const clap_plugin_note_ports_t g_note_ports = { note_ports_count, note_ports_get };

static uint32_t params_count(const clap_plugin_t *plugin)
{
    (void)plugin;
    return 4;
}

static bool params_get_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info)
{
    (void)plugin;
    if (index > 3)
        return false;
    memset(info, 0, sizeof(*info));
    info->id = index;
    info->flags = CLAP_PARAM_IS_STEPPED | (index ? CLAP_PARAM_IS_READONLY : 0);
    info->min_value = 0.0;
    info->max_value = index ? 1e9 : LATENCY_MAX;
    info->default_value = index ? 0.0 : layout_default();
    strcpy(info->name, index == 0 ? "latency" : (index == 1 ? "activations" : (index == 2 ? "process calls" : "host preset-load")));
    return true;
}

static bool params_get_value(const clap_plugin_t *plugin, clap_id id, double *value)
{
    const fake_t *fake = plugin->plugin_data;
    if (id > 3)
        return false;
    *value = id == 0 ? fake->latency_param : (id == 1 ? fake->activations : (id == 2 ? fake->process_calls : fake->host_preset_load));
    return true;
}

static bool params_value_to_text(const clap_plugin_t *plugin, clap_id id, double value, char *out, uint32_t size)
{
    (void)plugin;
    if (id != 0)
        return false;
    snprintf(out, size, "%.0f", value);
    return true;
}

static bool params_text_to_value(const clap_plugin_t *plugin, clap_id id, const char *text, double *value)
{
    (void)plugin;
    if (id != 0)
        return false;
    *value = atof(text);
    return true;
}

static void take_events(fake_t *fake, const clap_input_events_t *in)
{
    uint32_t i, n = in->size(in);

    for (i = 0; i < n; i++)
    {
        const clap_event_header_t *header = in->get(in, i);
        if (header->space_id == CLAP_CORE_EVENT_SPACE_ID && header->type == CLAP_EVENT_PARAM_VALUE)
        {
            const clap_event_param_value_t *event = (const clap_event_param_value_t *)header;
            if (event->param_id == 0)
            {
                fake->latency_param = event->value;
                fake->host->request_callback(fake->host);
            }
        }
    }
}

static void params_flush(const clap_plugin_t *plugin, const clap_input_events_t *in, const clap_output_events_t *out)
{
    (void)out;
    take_events(plugin->plugin_data, in);
}

static const clap_plugin_params_t g_params = {
    params_count, params_get_info, params_get_value, params_value_to_text, params_text_to_value, params_flush
};

static uint32_t latency_get(const clap_plugin_t *plugin)
{
    const fake_t *fake = plugin->plugin_data;
    return fake->latency;
}

static const clap_plugin_latency_t g_latency = { latency_get };

static bool plugin_init(const clap_plugin_t *plugin)
{
    fake_t *fake = plugin->plugin_data;

    fake->init_thread = pthread_self();
    fake_log(plugin, "init");
    return true;
}

static void main_thread_only(const clap_plugin_t *plugin, const char *what)
{
    const fake_t *fake = plugin->plugin_data;
    char line[64];

    if (pthread_equal(pthread_self(), fake->init_thread))
        return;
    snprintf(line, sizeof(line), "wrong thread %s", what);
    fake_log(plugin, line);
}

static void track_info_changed(const clap_plugin_t *plugin)
{
    const fake_t *fake = plugin->plugin_data;
    const clap_host_track_info_t *host_track = fake->host->get_extension(fake->host, CLAP_EXT_TRACK_INFO);
    const clap_host_remote_controls_t *host_remote = fake->host->get_extension(fake->host, CLAP_EXT_REMOTE_CONTROLS);
    clap_track_info_t info;
    char line[CLAP_NAME_SIZE + 96];
    bool got;

    main_thread_only(plugin, "track_info.changed");
    memset(&info, 0, sizeof(info));
    got = host_track && host_track->get(fake->host, &info);
    snprintf(line, sizeof(line), "track_info %d %llx %u,%u,%u,%u %s", got, (unsigned long long)info.flags, info.color.alpha,
             info.color.red, info.color.green, info.color.blue, info.name);
    fake_log(plugin, line);
    if (got && (info.flags & CLAP_TRACK_INFO_IS_FOR_BUS) && host_remote)
        host_remote->changed(fake->host);
}

static const clap_plugin_track_info_t g_track_info = { track_info_changed };

static uint32_t remote_controls_count(const clap_plugin_t *plugin)
{
    main_thread_only(plugin, "remote_controls.count");
    return 2;
}

/* page 0: a parameter, a read-only one, an empty slot and an id params does not list; page 1: one parameter, last */
static bool remote_controls_get(const clap_plugin_t *plugin, uint32_t index, clap_remote_controls_page_t *page)
{
    uint32_t i;

    main_thread_only(plugin, "remote_controls.get");
    if (index > 1)
    {
        fake_log(plugin, "remote_controls.get past the count");
        return false;
    }
    memset(page, 0, sizeof(*page));
    for (i = 0; i < CLAP_REMOTE_CONTROLS_COUNT; i++)
        page->param_ids[i] = CLAP_INVALID_ID;
    if (index == 0)
    {
        page->page_id = 7;
        strcpy(page->section_name, "Main");
        strcpy(page->page_name, "Page \"A\"\t1");
        page->param_ids[0] = 0;
        page->param_ids[1] = 1;
        page->param_ids[3] = 99;
    }
    else
    {
        page->page_id = 8;
        strcpy(page->page_name, "Two");
        page->param_ids[7] = 0;
    }
    return true;
}

static const clap_plugin_remote_controls_t g_remote_controls = { remote_controls_count, remote_controls_get };

static void plugin_destroy(const clap_plugin_t *plugin)
{
    fake_log(plugin, "destroy");
    free(plugin->plugin_data);
}

static bool plugin_activate(const clap_plugin_t *plugin, double sample_rate, uint32_t min_frames, uint32_t max_frames)
{
    fake_t *fake = plugin->plugin_data;

    (void)sample_rate; (void)min_frames; (void)max_frames;
    fake->activations++;
    fake_log(plugin, "activate");
    return true;
}

static void plugin_deactivate(const clap_plugin_t *plugin)
{
    (void)plugin;
}

static bool plugin_start_processing(const clap_plugin_t *plugin)
{
    (void)plugin;
    return true;
}

static void plugin_stop_processing(const clap_plugin_t *plugin)
{
    (void)plugin;
}

static void plugin_reset(const clap_plugin_t *plugin)
{
    (void)plugin;
}

static clap_process_status plugin_process(const clap_plugin_t *plugin, const clap_process_t *process)
{
    fake_t *fake = plugin->plugin_data;
    const clap_audio_buffer_t *in = &process->audio_inputs[0];
    clap_audio_buffer_t *out = &process->audio_outputs[0];
    uint32_t c, i;

    fake->process_calls++;
    if (!fake->logged_process)
    {
        fake->logged_process = 1;
        fake_log(plugin, "process");
    }
    take_events(fake, process->in_events);
    if (process->audio_outputs_count != 1 + fake->extra_outputs)
        return CLAP_PROCESS_ERROR;
    for (c = 0; c < out->channel_count; c++)
        memcpy(out->data32[c], in->data32[c < in->channel_count ? c : 0], sizeof(float) * process->frames_count);
    // an auxiliary output is the host's to hand over: a buffer it cannot write is a crash here
    for (c = 1; c < process->audio_outputs_count; c++)
        for (i = 0; i < process->audio_outputs[c].channel_count; i++)
            memset(process->audio_outputs[c].data32[i], 0, sizeof(float) * process->frames_count);
    return CLAP_PROCESS_CONTINUE;
}

static const void *plugin_get_extension(const clap_plugin_t *plugin, const char *id)
{
    const fake_t *fake = plugin->plugin_data;

    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &g_audio_ports;
    if (!strcmp(id, CLAP_EXT_NOTE_PORTS) && fake->note_inputs)
        return &g_note_ports;
    if (!strcmp(id, CLAP_EXT_PARAMS))
        return &g_params;
    if (!strcmp(id, CLAP_EXT_LATENCY))
        return &g_latency;
    if (!strcmp(plugin->desc->id, ID_PASSTHROUGH) && !strcmp(id, CLAP_EXT_TRACK_INFO))
        return &g_track_info;
    if (!strcmp(plugin->desc->id, ID_PASSTHROUGH) && !strcmp(id, CLAP_EXT_REMOTE_CONTROLS))
        return &g_remote_controls;
    return NULL;
}

/* the latency parameter landed: publish the new figure and ask to be restarted */
static void plugin_on_main_thread(const clap_plugin_t *plugin)
{
    fake_t *fake = plugin->plugin_data;
    const uint32_t latency = (uint32_t)fake->latency_param;
    const clap_host_latency_t *host_latency;

    if (latency == fake->latency)
        return;
    fake->latency = latency;
    host_latency = fake->host->get_extension(fake->host, CLAP_EXT_LATENCY);
    if (host_latency)
        host_latency->changed(fake->host);
    fake->host->request_restart(fake->host);
}

static uint32_t factory_get_plugin_count(const clap_plugin_factory_t *factory)
{
    (void)factory;
    return DESCRIPTOR_COUNT;
}

static const clap_plugin_descriptor_t *factory_get_plugin_descriptor(const clap_plugin_factory_t *factory, uint32_t index)
{
    (void)factory;
    return index < DESCRIPTOR_COUNT ? &g_descriptors[index] : NULL;
}

static const clap_plugin_t *factory_create_plugin(const clap_plugin_factory_t *factory, const clap_host_t *host, const char *plugin_id)
{
    fake_t *fake;
    uint32_t i;

    (void)factory;
    for (i = 0; i < DESCRIPTOR_COUNT; i++)
        if (!strcmp(g_descriptors[i].id, plugin_id))
            break;
    if (i == DESCRIPTOR_COUNT)
        return NULL;

    fake = calloc(1, sizeof(fake_t));
    fake->host = host;
    fake->host_preset_load = host->get_extension(host, CLAP_EXT_PRESET_LOAD) != NULL;
    fake->main_channels = !strcmp(plugin_id, ID_WIDE) ? 4 : (!strcmp(plugin_id, ID_WIDEN) ? 1 : 2);
    fake->out_channels = !strcmp(plugin_id, ID_WIDE) ? 4 : 2;
    fake->extra_outputs = !strcmp(plugin_id, ID_AUXOUT) ? 1 : 0;
    fake->extra_inputs = !strcmp(plugin_id, ID_SIDECHAIN) ? 1 : 0;
    fake->note_inputs = !strcmp(plugin_id, ID_NOTES) ? 1 : 0;
    fake->latency = (uint32_t)LATENCY_DEFAULT;
    fake->latency_param = LATENCY_DEFAULT;

    fake->plugin.desc = &g_descriptors[i];
    fake->plugin.plugin_data = fake;
    fake->plugin.init = plugin_init;
    fake->plugin.destroy = plugin_destroy;
    fake->plugin.activate = plugin_activate;
    fake->plugin.deactivate = plugin_deactivate;
    fake->plugin.start_processing = plugin_start_processing;
    fake->plugin.stop_processing = plugin_stop_processing;
    fake->plugin.reset = plugin_reset;
    fake->plugin.process = plugin_process;
    fake->plugin.get_extension = plugin_get_extension;
    fake->plugin.on_main_thread = plugin_on_main_thread;
    return &fake->plugin;
}

static const clap_plugin_factory_t g_factory = {
    factory_get_plugin_count, factory_get_plugin_descriptor, factory_create_plugin
};

static bool entry_init(const char *path)
{
    (void)path;
    return true;
}

static void entry_deinit(void)
{
}

static const void *entry_get_factory(const char *factory_id)
{
    return !strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) ? &g_factory : NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory
};
