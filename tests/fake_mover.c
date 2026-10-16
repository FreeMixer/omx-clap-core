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

/* A .clap whose one plugin moves a parameter of its own, as a plugin does when its user turns its own knob:
 *   org.omx-clap-host.test.mover   stereo in to stereo out, passed through. Parameters:
 *     0 level        0..1, default 0.5, automatable and modulatable; its text is a percentage ("75%")
 *     1 modulation   read-only: the last CLAP_EVENT_PARAM_MOD amount the host sent for level
 *     2 gestures     read-only: the host's gesture events on level, begin and end, counted
 *     3 trim         0..1, default 0, automatable, not modulatable
 * On its MOVE_AT-th process() (FAKE_MOVER_AT, 8 without it) it pushes, in this order, a gesture begin on level, the value
 * 0.75, and the gesture's end, and takes 0.75 as its own; then it asks for a main-thread callback, in which FAKE_MOVER
 * names what else it does: "info" renames level to "gain" and calls params.rescan(CLAP_PARAM_RESCAN_INFO), "all" calls
 * params.rescan(CLAP_PARAM_RESCAN_ALL) although it is active, "clear" calls params.clear(level, CLAP_PARAM_CLEAR_ALL).
 * FAKE_MOVER_FLOOD=<n> makes the same process() push n values of trim after them (0.001 apart, the last n / 1000).
 * Every environment variable is read at create, never on the audio thread. FAKE_LOG names a file each instance appends
 * "<id> host gesture begin|end <param>" to as the host's gestures arrive, and "<id> mod <param> <amount>". */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <clap/clap.h>

#define ID_MOVER        "org.omx-clap-host.test.mover"
#define MOVE_AT         8u
#define MOVED_VALUE     0.75
#define LOG_LINES       16

typedef struct MOVER_T {
    clap_plugin_t plugin;
    const clap_host_t *host;
    const clap_host_params_t *host_params;
    double level, modulation, trim;
    uint32_t gestures;
    uint32_t blocks;
    uint32_t move_at;
    uint32_t flood;
    int renamed;
    char action[16];
    char log_path[512];
    // what the audio thread saw, written to the log on the main thread: never a file from process()
    char lines[LOG_LINES][64];
    uint32_t n_lines;
} mover_t;

static const char *const g_features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL };

static const clap_plugin_descriptor_t g_descriptor = {
    CLAP_VERSION_INIT, ID_MOVER, "mover", "omx-clap-host", "", "", "", "0", "mover", g_features
};

static void note(mover_t *m, const char *line)
{
    if (m->n_lines < LOG_LINES)
        snprintf(m->lines[m->n_lines++], sizeof(m->lines[0]), "%s", line);
}

static void log_flush(mover_t *m)
{
    FILE *f;
    uint32_t i;

    if (m->log_path[0] && m->n_lines && (f = fopen(m->log_path, "a")))
    {
        for (i = 0; i < m->n_lines; i++)
            fprintf(f, "%s %s\n", ID_MOVER, m->lines[i]);
        fclose(f);
    }
    m->n_lines = 0;
}

static uint32_t audio_ports_count(const clap_plugin_t *plugin, bool is_input)
{
    (void)plugin;
    (void)is_input;
    return 1;
}

static bool audio_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_audio_port_info_t *info)
{
    (void)plugin;
    if (index > 0)
        return false;
    memset(info, 0, sizeof(*info));
    info->id = 0;
    snprintf(info->name, sizeof(info->name), "%s", is_input ? "in" : "out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t g_audio_ports = { audio_ports_count, audio_ports_get };

static uint32_t params_count(const clap_plugin_t *plugin)
{
    (void)plugin;
    return 4;
}

static bool params_get_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info)
{
    const mover_t *m = plugin->plugin_data;
    static const char *const names[] = { "level", "modulation", "gestures", "trim" };

    if (index > 3)
        return false;
    memset(info, 0, sizeof(*info));
    info->id = index;
    info->flags = index == 0 ? CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE
                : index == 3 ? CLAP_PARAM_IS_AUTOMATABLE : CLAP_PARAM_IS_READONLY;
    info->min_value = index == 0 ? 0.0 : index == 1 ? -1.0 : 0.0;
    info->max_value = index == 2 ? 1e9 : 1.0;
    info->default_value = index == 0 ? 0.5 : 0.0;
    snprintf(info->name, sizeof(info->name), "%s", index == 0 && m->renamed ? "gain" : names[index]);
    return true;
}

static bool params_get_value(const clap_plugin_t *plugin, clap_id id, double *value)
{
    const mover_t *m = plugin->plugin_data;

    if (id > 3)
        return false;
    *value = id == 0 ? m->level : id == 1 ? m->modulation : id == 2 ? (double)m->gestures : m->trim;
    return true;
}

static bool params_value_to_text(const clap_plugin_t *plugin, clap_id id, double value, char *out, uint32_t size)
{
    (void)plugin;
    if (id != 0)
        return false;
    snprintf(out, size, "%.0f%%", value * 100.0);
    return true;
}

static bool params_text_to_value(const clap_plugin_t *plugin, clap_id id, const char *text, double *value)
{
    char *end;
    double v;

    (void)plugin;
    if (id != 0)
        return false;
    v = strtod(text, &end);
    if (end == text || strcmp(end, "%") != 0)
        return false;
    *value = v / 100.0;
    return true;
}

/* the host's events: a value, a modulation and a gesture on level; trim's value */
static void take_events(mover_t *m, const clap_input_events_t *in)
{
    uint32_t i, n = in->size(in);
    char line[64];

    for (i = 0; i < n; i++)
    {
        const clap_event_header_t *h = in->get(in, i);

        if (h->space_id != CLAP_CORE_EVENT_SPACE_ID)
            continue;
        if (h->type == CLAP_EVENT_PARAM_VALUE)
        {
            const clap_event_param_value_t *e = (const clap_event_param_value_t *)h;

            if (e->param_id == 0)
                m->level = e->value;
            else if (e->param_id == 3)
                m->trim = e->value;
        }
        else if (h->type == CLAP_EVENT_PARAM_MOD)
        {
            const clap_event_param_mod_t *e = (const clap_event_param_mod_t *)h;

            m->modulation = e->amount;
            snprintf(line, sizeof(line), "mod %u %g", e->param_id, e->amount);
            note(m, line);
        }
        else if (h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN || h->type == CLAP_EVENT_PARAM_GESTURE_END)
        {
            const clap_event_param_gesture_t *e = (const clap_event_param_gesture_t *)h;

            m->gestures++;
            snprintf(line, sizeof(line), "host gesture %s %u", h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN ? "begin" : "end",
                     e->param_id);
            note(m, line);
        }
    }
}

static void push_gesture(const clap_output_events_t *out, uint16_t type, clap_id id)
{
    clap_event_param_gesture_t e;

    memset(&e, 0, sizeof(e));
    e.header.size = sizeof(e);
    e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e.header.type = type;
    e.param_id = id;
    out->try_push(out, &e.header);
}

static void push_value(const clap_output_events_t *out, clap_id id, double value)
{
    clap_event_param_value_t e;

    memset(&e, 0, sizeof(e));
    e.header.size = sizeof(e);
    e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e.header.type = CLAP_EVENT_PARAM_VALUE;
    e.param_id = id;
    e.note_id = -1;
    e.port_index = -1;
    e.channel = -1;
    e.key = -1;
    e.value = value;
    out->try_push(out, &e.header);
}

static void params_flush(const clap_plugin_t *plugin, const clap_input_events_t *in, const clap_output_events_t *out)
{
    (void)out;
    take_events(plugin->plugin_data, in);
}

static const clap_plugin_params_t g_params = {
    params_count, params_get_info, params_get_value, params_value_to_text, params_text_to_value, params_flush
};

static bool plugin_init(const clap_plugin_t *plugin)
{
    mover_t *m = plugin->plugin_data;

    m->host_params = m->host->get_extension(m->host, CLAP_EXT_PARAMS);
    return true;
}

static void plugin_destroy(const clap_plugin_t *plugin)
{
    log_flush(plugin->plugin_data);
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
    mover_t *m = plugin->plugin_data;
    uint32_t c, i;

    take_events(m, process->in_events);
    if (++m->blocks == m->move_at)
    {
        push_gesture(process->out_events, CLAP_EVENT_PARAM_GESTURE_BEGIN, 0);
        push_value(process->out_events, 0, MOVED_VALUE);
        m->level = MOVED_VALUE;
        push_gesture(process->out_events, CLAP_EVENT_PARAM_GESTURE_END, 0);
        for (i = 1; i <= m->flood; i++)
            push_value(process->out_events, 3, (double)i / 1000.0);
        if (m->flood)
            m->trim = (double)m->flood / 1000.0;
        m->host->request_callback(m->host);
    }
    for (c = 0; c < 2; c++)
        memcpy(process->audio_outputs[0].data32[c], process->audio_inputs[0].data32[c], sizeof(float) * process->frames_count);
    return CLAP_PROCESS_CONTINUE;
}

static const void *plugin_get_extension(const clap_plugin_t *plugin, const char *id)
{
    (void)plugin;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &g_audio_ports;
    if (!strcmp(id, CLAP_EXT_PARAMS))
        return &g_params;
    return NULL;
}

/* after the move: what FAKE_MOVER asks of the host's params, and the log */
static void plugin_on_main_thread(const clap_plugin_t *plugin)
{
    mover_t *m = plugin->plugin_data;

    if (m->host_params && !strcmp(m->action, "info"))
    {
        m->renamed = 1;
        m->host_params->rescan(m->host, CLAP_PARAM_RESCAN_INFO);
    }
    else if (m->host_params && !strcmp(m->action, "all"))
        m->host_params->rescan(m->host, CLAP_PARAM_RESCAN_ALL);
    else if (m->host_params && !strcmp(m->action, "clear"))
        m->host_params->clear(m->host, 0, CLAP_PARAM_CLEAR_ALL);
    log_flush(m);
}

static uint32_t factory_get_plugin_count(const clap_plugin_factory_t *factory)
{
    (void)factory;
    return 1;
}

static const clap_plugin_descriptor_t *factory_get_plugin_descriptor(const clap_plugin_factory_t *factory, uint32_t index)
{
    (void)factory;
    return index == 0 ? &g_descriptor : NULL;
}

static const clap_plugin_t *factory_create_plugin(const clap_plugin_factory_t *factory, const clap_host_t *host, const char *plugin_id)
{
    const char *at = getenv("FAKE_MOVER_AT"), *action = getenv("FAKE_MOVER"), *flood = getenv("FAKE_MOVER_FLOOD");
    const char *log = getenv("FAKE_LOG");
    mover_t *m;

    (void)factory;
    if (strcmp(plugin_id, ID_MOVER) != 0 || !(m = calloc(1, sizeof(*m))))
        return NULL;
    m->host = host;
    m->level = 0.5;
    m->move_at = at && *at ? (uint32_t)atoi(at) : MOVE_AT;
    m->flood = flood && *flood ? (uint32_t)atoi(flood) : 0;
    snprintf(m->action, sizeof(m->action), "%s", action ? action : "");
    snprintf(m->log_path, sizeof(m->log_path), "%s", log ? log : "");
    m->plugin.desc = &g_descriptor;
    m->plugin.plugin_data = m;
    m->plugin.init = plugin_init;
    m->plugin.destroy = plugin_destroy;
    m->plugin.activate = plugin_activate;
    m->plugin.deactivate = plugin_deactivate;
    m->plugin.start_processing = plugin_start_processing;
    m->plugin.stop_processing = plugin_stop_processing;
    m->plugin.reset = plugin_reset;
    m->plugin.process = plugin_process;
    m->plugin.get_extension = plugin_get_extension;
    m->plugin.on_main_thread = plugin_on_main_thread;
    return &m->plugin;
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
