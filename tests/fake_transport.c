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


/* A transport probe for the tests, the CLAP twin of lv2_time_probe: org.omx-clap-host.test.transport-probe, a stereo
 * effect that passes its input through and keeps the transport its last process() was handed as read-only
 * parameters, which param_get reads:
 *   0 a transport was handed (1) or NULL (0)     4 the time signature's denominator
 *   1 the tempo, under HAS_TEMPO                5 the song position in beats, under HAS_BEATS_TIMELINE
 *   2 playing (IS_PLAYING)                      6 the song position in seconds, under HAS_SECONDS_TIMELINE
 *   3 the time signature's numerator            7 the bar number, under HAS_BEATS_TIMELINE
 *   8 the beats minus the seconds times the tempo over 60, both taken from the one transport of one process(): 0
 *     while the two timelines run in step, whatever the position they started from
 * A field whose flag is not set reads -1; so does 8 when the transport lacks the tempo or either timeline. */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <clap/clap.h>

#define ID_PROBE        "org.omx-clap-host.test.transport-probe"
#define PROBE_PARAMS    9u

typedef struct PROBE_T {
    clap_plugin_t plugin;
    _Atomic uint64_t seen[PROBE_PARAMS];    // each read-only parameter's value, as the bits of a double
} probe_t;

static const char *const g_names[PROBE_PARAMS] = {
    "has transport", "tempo", "playing", "tsig num", "tsig denom", "beats", "seconds", "bar", "beats vs seconds"
};

static void keep(probe_t *p, uint32_t id, double v)
{
    uint64_t bits;

    memcpy(&bits, &v, sizeof bits);
    atomic_store_explicit(&p->seen[id], bits, memory_order_relaxed);
}

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
    return PROBE_PARAMS;
}

static bool params_get_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info)
{
    (void)plugin;
    if (index >= PROBE_PARAMS)
        return false;
    memset(info, 0, sizeof(*info));
    info->id = index;
    info->flags = CLAP_PARAM_IS_READONLY;
    info->min_value = -1e9;
    info->max_value = 1e9;
    info->default_value = -1.0;
    snprintf(info->name, sizeof(info->name), "%s", g_names[index]);
    return true;
}

static bool params_get_value(const clap_plugin_t *plugin, clap_id id, double *value)
{
    probe_t *p = plugin->plugin_data;
    uint64_t bits;

    if (id >= PROBE_PARAMS)
        return false;
    bits = atomic_load_explicit(&p->seen[id], memory_order_relaxed);
    memcpy(value, &bits, sizeof(*value));
    return true;
}

static bool params_value_to_text(const clap_plugin_t *plugin, clap_id id, double value, char *out, uint32_t size)
{
    (void)plugin;
    (void)id;
    snprintf(out, size, "%g", value);
    return true;
}

static bool params_text_to_value(const clap_plugin_t *plugin, clap_id id, const char *text, double *value)
{
    (void)plugin;
    (void)id;
    *value = atof(text);
    return true;
}

static void params_flush(const clap_plugin_t *plugin, const clap_input_events_t *in, const clap_output_events_t *out)
{
    (void)plugin;
    (void)in;
    (void)out;
}

static const clap_plugin_params_t g_params = {
    params_count, params_get_info, params_get_value, params_value_to_text, params_text_to_value, params_flush
};

static bool plugin_init(const clap_plugin_t *plugin)
{
    probe_t *p = plugin->plugin_data;

    for (uint32_t i = 0; i < PROBE_PARAMS; i++)
        keep(p, i, -1.0);
    keep(p, 0, 0.0);
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

static void plugin_nop(const clap_plugin_t *plugin)
{
    (void)plugin;
}

static bool plugin_start(const clap_plugin_t *plugin)
{
    (void)plugin;
    return true;
}

static clap_process_status plugin_process(const clap_plugin_t *plugin, const clap_process_t *process)
{
    probe_t *p = plugin->plugin_data;
    const clap_event_transport_t *t = process->transport;

    for (uint32_t c = 0; c < 2; c++)
        memcpy(process->audio_outputs[0].data32[c], process->audio_inputs[0].data32[c], process->frames_count * sizeof(float));
    keep(p, 0, t ? 1.0 : 0.0);
    keep(p, 1, t && (t->flags & CLAP_TRANSPORT_HAS_TEMPO) ? t->tempo : -1.0);
    keep(p, 2, t ? ((t->flags & CLAP_TRANSPORT_IS_PLAYING) ? 1.0 : 0.0) : -1.0);
    keep(p, 3, t && (t->flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE) ? t->tsig_num : -1.0);
    keep(p, 4, t && (t->flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE) ? t->tsig_denom : -1.0);
    keep(p, 5, t && (t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) ? (double)t->song_pos_beats / (double)CLAP_BEATTIME_FACTOR : -1.0);
    keep(p, 6, t && (t->flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE) ? (double)t->song_pos_seconds / (double)CLAP_SECTIME_FACTOR : -1.0);
    keep(p, 7, t && (t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) ? t->bar_number : -1.0);
    keep(p, 8, t && (t->flags & CLAP_TRANSPORT_HAS_TEMPO) && (t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) &&
               (t->flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE)
                   ? (double)t->song_pos_beats / (double)CLAP_BEATTIME_FACTOR -
                         (double)t->song_pos_seconds / (double)CLAP_SECTIME_FACTOR * t->tempo / 60.0
                   : -1.0);
    return CLAP_PROCESS_CONTINUE;
}

static const void *plugin_get_extension(const clap_plugin_t *plugin, const char *id)
{
    (void)plugin;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &g_ports;
    if (!strcmp(id, CLAP_EXT_PARAMS))
        return &g_params;
    return NULL;
}

static const char *const g_features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL };
static const clap_plugin_descriptor_t g_descriptor = {
    CLAP_VERSION_INIT, ID_PROBE, "transport probe", "omx-clap-host", "", "", "", "0", "the transport it is handed, as parameters", g_features
};

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

static const clap_plugin_t *factory_create(const clap_plugin_factory_t *factory, const clap_host_t *host, const char *plugin_id)
{
    probe_t *p;

    (void)factory;
    (void)host;
    if (strcmp(plugin_id, ID_PROBE) != 0)
        return NULL;
    p = calloc(1, sizeof(*p));
    p->plugin.desc = &g_descriptor;
    p->plugin.plugin_data = p;
    p->plugin.init = plugin_init;
    p->plugin.destroy = plugin_destroy;
    p->plugin.activate = plugin_activate;
    p->plugin.deactivate = plugin_nop;
    p->plugin.start_processing = plugin_start;
    p->plugin.stop_processing = plugin_nop;
    p->plugin.reset = plugin_nop;
    p->plugin.process = plugin_process;
    p->plugin.get_extension = plugin_get_extension;
    p->plugin.on_main_thread = plugin_nop;
    return &p->plugin;
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

static const void *entry_get_factory(const char *factory_id)
{
    return !strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) ? &g_factory : NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory };
