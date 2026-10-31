/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * A stereo pass-through with one parameter, kept in its state, whose state.load answers false while it processes, as
 * most of omx-plugins do: the state must reach it with no cycle running it (docs/chain.md, state_load).
 *
 *   org.omx-clap-host.test.state   parameter 0 "value", 0 to 10, default 4; a preset's load sets it to 9, and answers
 *                                  false while it processes too
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <clap/clap.h>
#include <clap/ext/preset-load.h>

#define ID_STATE "org.omx-clap-host.test.state"
#define VALUE_DEFAULT 4.0
#define VALUE_MAX 10.0

typedef struct FAKE_T {
    clap_plugin_t plugin;
    _Atomic double value;
    atomic_int processing;
} fake_t;

static const char *const g_features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL };
static const clap_plugin_descriptor_t g_descriptor = {
    CLAP_VERSION_INIT, ID_STATE, "state", "omx-clap-host", "", "", "", "0", "state", g_features
};

static uint32_t ports_count(const clap_plugin_t *plugin, bool is_input)
{
    (void)plugin;
    (void)is_input;
    return 1;
}

static bool ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_audio_port_info_t *info)
{
    (void)plugin;
    if (index != 0)
        return false;
    memset(info, 0, sizeof(*info));
    info->id = is_input ? 0 : 1;
    snprintf(info->name, sizeof(info->name), "%s", is_input ? "in" : "out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t g_ports = { ports_count, ports_get };

static uint32_t params_count(const clap_plugin_t *plugin)
{
    (void)plugin;
    return 1;
}

static bool params_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info)
{
    (void)plugin;
    if (index != 0)
        return false;
    memset(info, 0, sizeof(*info));
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    snprintf(info->name, sizeof(info->name), "value");
    info->max_value = VALUE_MAX;
    info->default_value = VALUE_DEFAULT;
    return true;
}

static bool params_value(const clap_plugin_t *plugin, clap_id id, double *value)
{
    const fake_t *fake = plugin->plugin_data;

    if (id != 0)
        return false;
    *value = atomic_load(&fake->value);
    return true;
}

static bool params_to_text(const clap_plugin_t *plugin, clap_id id, double value, char *out, uint32_t size)
{
    (void)plugin;
    (void)id;
    snprintf(out, size, "%g", value);
    return true;
}

static bool params_from_text(const clap_plugin_t *plugin, clap_id id, const char *text, double *value)
{
    (void)plugin;
    (void)id;
    *value = atof(text);
    return true;
}

static void take_events(fake_t *fake, const clap_input_events_t *in)
{
    uint32_t i, n = in ? in->size(in) : 0;

    for (i = 0; i < n; i++)
    {
        const clap_event_header_t *h = in->get(in, i);

        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE)
        {
            const clap_event_param_value_t *e = (const clap_event_param_value_t *)h;

            if (e->param_id == 0)
                atomic_store(&fake->value, e->value);
        }
    }
}

static void params_flush(const clap_plugin_t *plugin, const clap_input_events_t *in, const clap_output_events_t *out)
{
    (void)out;
    take_events(plugin->plugin_data, in);
}

static const clap_plugin_params_t g_params = {
    params_count, params_info, params_value, params_to_text, params_from_text, params_flush
};

static bool state_save(const clap_plugin_t *plugin, const clap_ostream_t *stream)
{
    const fake_t *fake = plugin->plugin_data;
    const double value = atomic_load(&fake->value);

    return stream->write(stream, &value, sizeof(value)) == (int64_t)sizeof(value);
}

static bool state_load(const clap_plugin_t *plugin, const clap_istream_t *stream)
{
    fake_t *fake = plugin->plugin_data;
    double value;

    if (atomic_load(&fake->processing))
        return false;
    if (stream->read(stream, &value, sizeof(value)) != (int64_t)sizeof(value))
        return false;
    atomic_store(&fake->value, value);
    return true;
}

static const clap_plugin_state_t g_state = { state_save, state_load };

static bool preset_from_location(const clap_plugin_t *plugin, uint32_t kind, const char *location, const char *key)
{
    fake_t *fake = plugin->plugin_data;

    (void)kind;
    (void)location;
    (void)key;
    if (atomic_load(&fake->processing))
        return false;
    atomic_store(&fake->value, 9.0);
    return true;
}

static const clap_plugin_preset_load_t g_preset = { preset_from_location };

static bool plugin_init(const clap_plugin_t *plugin)
{
    (void)plugin;
    return true;
}

static void plugin_destroy(const clap_plugin_t *plugin)
{
    free(plugin->plugin_data);
}

static bool plugin_activate(const clap_plugin_t *plugin, double rate, uint32_t min_frames, uint32_t max_frames)
{
    (void)plugin;
    (void)rate;
    (void)min_frames;
    (void)max_frames;
    return true;
}

static void plugin_deactivate(const clap_plugin_t *plugin)
{
    (void)plugin;
}

static bool plugin_start_processing(const clap_plugin_t *plugin)
{
    atomic_store(&((fake_t *)plugin->plugin_data)->processing, 1);
    return true;
}

static void plugin_stop_processing(const clap_plugin_t *plugin)
{
    atomic_store(&((fake_t *)plugin->plugin_data)->processing, 0);
}

static void plugin_reset(const clap_plugin_t *plugin)
{
    (void)plugin;
}

static clap_process_status plugin_process(const clap_plugin_t *plugin, const clap_process_t *process)
{
    uint32_t c;

    take_events(plugin->plugin_data, process->in_events);
    for (c = 0; c < 2; c++)
        if (process->audio_outputs[0].data32[c] != process->audio_inputs[0].data32[c])
            memcpy(process->audio_outputs[0].data32[c], process->audio_inputs[0].data32[c], process->frames_count * sizeof(float));
    return CLAP_PROCESS_CONTINUE;
}

static const void *plugin_extension(const clap_plugin_t *plugin, const char *id)
{
    (void)plugin;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &g_ports;
    if (!strcmp(id, CLAP_EXT_PARAMS))
        return &g_params;
    if (!strcmp(id, CLAP_EXT_STATE))
        return &g_state;
    if (!strcmp(id, CLAP_EXT_PRESET_LOAD))
        return &g_preset;
    return NULL;
}

static void plugin_on_main_thread(const clap_plugin_t *plugin)
{
    (void)plugin;
}

static uint32_t factory_count(const clap_plugin_factory_t *factory)
{
    (void)factory;
    return 1;
}

static const clap_plugin_descriptor_t *factory_descriptor(const clap_plugin_factory_t *factory, uint32_t index)
{
    (void)factory;
    return index == 0 ? &g_descriptor : NULL;
}

static const clap_plugin_t *factory_create(const clap_plugin_factory_t *factory, const clap_host_t *host, const char *id)
{
    fake_t *fake;

    (void)factory;
    (void)host;
    if (strcmp(id, ID_STATE) || !(fake = calloc(1, sizeof(*fake))))
        return NULL;
    atomic_init(&fake->value, VALUE_DEFAULT);
    fake->plugin.desc = &g_descriptor;
    fake->plugin.plugin_data = fake;
    fake->plugin.init = plugin_init;
    fake->plugin.destroy = plugin_destroy;
    fake->plugin.activate = plugin_activate;
    fake->plugin.deactivate = plugin_deactivate;
    fake->plugin.start_processing = plugin_start_processing;
    fake->plugin.stop_processing = plugin_stop_processing;
    fake->plugin.reset = plugin_reset;
    fake->plugin.process = plugin_process;
    fake->plugin.get_extension = plugin_extension;
    fake->plugin.on_main_thread = plugin_on_main_thread;
    return &fake->plugin;
}

static const clap_plugin_factory_t g_factory = { factory_count, factory_descriptor, factory_create };

static bool entry_init(const char *path)
{
    (void)path;
    return true;
}

static void entry_deinit(void)
{
}

static const void *entry_factory(const char *id)
{
    return !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &g_factory : NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, entry_init, entry_deinit, entry_factory };
