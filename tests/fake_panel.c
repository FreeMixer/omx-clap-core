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
 * Two plugins built for the scanner's reading of org.openmixer.panel-description/1, each a stereo passthrough with
 * three parameters (0 threshold, 1 mode, 2 knee):
 *  - org.omx-clap-host.test.panel serves a known description: family dynamics; two sections, main and detail; three
 *    controls, threshold a fader in main, mode a switch in detail and knee a knob in detail shown while mode is 1 or 2
 *    (visible_param_id 1, mask 6); one role, threshold; one display, a gain reduction bar over main from meter 7 with a
 *    -24 dB floor; and no placement default, its table empty;
 *  - org.omx-clap-host.test.panel-none serves none;
 *  - org.omx-clap-host.test.panel-hostile serves the same description but counts its controls UINT32_MAX and its roles
 *    one past OMX_CLAP_PANEL_DESCRIPTION_MAX_ENTRIES, and its get fills a row for any index of those two tables, so a
 *    host that believes the counts reserves and walks every one.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <clap/clap.h>

#include "omx_clap_ext.h"

#define ID_PANEL        "org.omx-clap-host.test.panel"
#define ID_NONE         "org.omx-clap-host.test.panel-none"
#define ID_HOSTILE      "org.omx-clap-host.test.panel-hostile"
#define PARAM_COUNT     3u
#define FINGERPRINT     "5f2c1e0a9b8d7c6e5f4a3b2c1d0e9f8a7b6c5d4e3f2a1b0c9d8e7f6a5b4c3d2e"

static const char *const g_features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL };
static const clap_plugin_descriptor_t g_descs[] = {
    { CLAP_VERSION_INIT, ID_PANEL, "panel", "omx-clap-host", "", "", "", "0", "", g_features },
    { CLAP_VERSION_INIT, ID_NONE, "panel-none", "omx-clap-host", "", "", "", "0", "", g_features },
    { CLAP_VERSION_INIT, ID_HOSTILE, "panel-hostile", "omx-clap-host", "", "", "", "0", "", g_features },
};
#define DESC_COUNT (sizeof(g_descs) / sizeof(g_descs[0]))

static const omx_clap_panel_description_section_t g_sections[] = {
    { "main", "panel.section.main", "Main" },
    { "detail", "panel.section.detail", "Detail" },
};

static const omx_clap_panel_description_control_t g_controls[] = {
    { 0, 0, 2, "panel.threshold", "Threshold", CLAP_INVALID_ID, 0 },
    { 1, 1, 3, "panel.mode", "Mode", CLAP_INVALID_ID, 0 },
    { 2, 1, 1, "panel.knee", "Knee", 1, 0x6 },
};

static const omx_clap_panel_description_role_t g_roles[] = {
    { "threshold", 0, 0 },
};

static const omx_clap_panel_description_display_t g_displays[] = {
    { 0, 0, 1, { 7, CLAP_INVALID_ID }, 0, CLAP_INVALID_ID, -24.0, "panel.display.gain_reduction", "Gain reduction" },
};

/* each table's rows and their size, by its OMX_CLAP_PANEL_TABLES code; the placement defaults have none */
static const struct { const void *rows; uint32_t count; size_t size; } g_tables[] = {
    { g_sections, sizeof(g_sections) / sizeof(g_sections[0]), sizeof(g_sections[0]) },
    { g_controls, sizeof(g_controls) / sizeof(g_controls[0]), sizeof(g_controls[0]) },
    { g_roles, sizeof(g_roles) / sizeof(g_roles[0]), sizeof(g_roles[0]) },
    { g_displays, sizeof(g_displays) / sizeof(g_displays[0]), sizeof(g_displays[0]) },
    { NULL, 0, sizeof(omx_clap_panel_description_default_t) },
};
#define TABLE_COUNT (sizeof(g_tables) / sizeof(g_tables[0]))

typedef struct FAKE_PANEL_T {
    clap_plugin_t plugin;
    const clap_host_t *host;
    double values[PARAM_COUNT];
} fake_panel_t;

static bool plugin_init(const clap_plugin_t *plugin) { (void)plugin; return true; }
static void plugin_destroy(const clap_plugin_t *plugin) { free((void *)plugin->plugin_data); }

static bool plugin_activate(const clap_plugin_t *plugin, double rate, uint32_t min_frames, uint32_t max_frames)
{
    (void)plugin;
    (void)rate;
    (void)min_frames;
    (void)max_frames;
    return true;
}

static void plugin_deactivate(const clap_plugin_t *plugin) { (void)plugin; }
static bool plugin_start_processing(const clap_plugin_t *plugin) { (void)plugin; return true; }
static void plugin_stop_processing(const clap_plugin_t *plugin) { (void)plugin; }
static void plugin_reset(const clap_plugin_t *plugin) { (void)plugin; }

static clap_process_status plugin_process(const clap_plugin_t *plugin, const clap_process_t *process)
{
    uint32_t c;

    (void)plugin;
    for (c = 0; c < 2; c++)
        memcpy(process->audio_outputs[0].data32[c], process->audio_inputs[0].data32[c], process->frames_count * sizeof(float));
    return CLAP_PROCESS_CONTINUE;
}

static uint32_t params_count(const clap_plugin_t *plugin) { (void)plugin; return PARAM_COUNT; }

static bool params_get_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info)
{
    static const struct { const char *name; double min, max, def; clap_param_info_flags flags; } params[PARAM_COUNT] = {
        { "threshold", -60.0, 0.0, -20.0, CLAP_PARAM_IS_AUTOMATABLE },
        { "mode", 0.0, 2.0, 0.0, CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_AUTOMATABLE },
        { "knee", 0.0, 12.0, 6.0, CLAP_PARAM_IS_AUTOMATABLE },
    };

    (void)plugin;
    if (index >= PARAM_COUNT)
        return false;
    memset(info, 0, sizeof(*info));
    info->id = index;
    info->flags = params[index].flags;
    snprintf(info->name, sizeof(info->name), "%s", params[index].name);
    info->min_value = params[index].min;
    info->max_value = params[index].max;
    info->default_value = params[index].def;
    return true;
}

static bool params_get_value(const clap_plugin_t *plugin, clap_id id, double *value)
{
    const fake_panel_t *p = plugin->plugin_data;

    if (id >= PARAM_COUNT)
        return false;
    *value = p->values[id];
    return true;
}

static bool params_value_to_text(const clap_plugin_t *plugin, clap_id id, double value, char *text, uint32_t size)
{
    (void)plugin;
    if (id >= PARAM_COUNT)
        return false;
    snprintf(text, size, "%g", value);
    return true;
}

static bool params_text_to_value(const clap_plugin_t *plugin, clap_id id, const char *text, double *value)
{
    (void)plugin;
    if (id >= PARAM_COUNT)
        return false;
    return sscanf(text, "%lf", value) == 1;
}

static void params_flush(const clap_plugin_t *plugin, const clap_input_events_t *in, const clap_output_events_t *out)
{
    (void)plugin;
    (void)in;
    (void)out;
}

static uint32_t ports_count(const clap_plugin_t *plugin, bool is_input) { (void)plugin; (void)is_input; return 1; }

static bool ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_audio_port_info_t *info)
{
    (void)plugin;
    if (index != 0)
        return false;
    memset(info, 0, sizeof(*info));
    snprintf(info->name, sizeof(info->name), "%s 0", is_input ? "in" : "out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const char *panel_fingerprint(const clap_plugin_t *plugin) { (void)plugin; return FINGERPRINT; }
static uint32_t panel_family(const clap_plugin_t *plugin) { (void)plugin; return 2; }

static uint32_t panel_count(const clap_plugin_t *plugin, uint32_t table)
{
    (void)plugin;
    return table < TABLE_COUNT ? g_tables[table].count : 0;
}

/* the hostile plugin's counts: its controls and its roles past what a host reads */
static uint32_t hostile_count(const clap_plugin_t *plugin, uint32_t table)
{
    if (table == 1)
        return UINT32_MAX;
    if (table == 2)
        return OMX_CLAP_PANEL_DESCRIPTION_MAX_ENTRIES + 1;
    return panel_count(plugin, table);
}

static bool panel_get(const clap_plugin_t *plugin, uint32_t table, uint32_t index, void *out)
{
    (void)plugin;
    if (!out || table >= TABLE_COUNT || index >= g_tables[table].count)
        return false;
    memcpy(out, (const char *)g_tables[table].rows + (size_t)index * g_tables[table].size, g_tables[table].size);
    return true;
}

/* any index of the hostile tables is the table's first row */
static bool hostile_get(const clap_plugin_t *plugin, uint32_t table, uint32_t index, void *out)
{
    if (out && (table == 1 || table == 2))
        index = 0;
    return panel_get(plugin, table, index, out);
}

static const clap_plugin_params_t g_params = { params_count, params_get_info, params_get_value, params_value_to_text,
                                               params_text_to_value, params_flush };
static const clap_plugin_audio_ports_t g_ports = { ports_count, ports_get };
static const omx_clap_plugin_panel_description_ext_t g_panel = { panel_fingerprint, panel_family, panel_count, panel_get };
static const omx_clap_plugin_panel_description_ext_t g_hostile = { panel_fingerprint, panel_family, hostile_count, hostile_get };

static const void *plugin_get_extension(const clap_plugin_t *plugin, const char *id)
{
    if (!strcmp(id, CLAP_EXT_PARAMS))
        return &g_params;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &g_ports;
    if (!strcmp(id, OMX_CLAP_EXT_PANEL_DESCRIPTION) && !strcmp(plugin->desc->id, ID_PANEL))
        return &g_panel;
    if (!strcmp(id, OMX_CLAP_EXT_PANEL_DESCRIPTION) && !strcmp(plugin->desc->id, ID_HOSTILE))
        return &g_hostile;
    return NULL;
}

static void plugin_on_main_thread(const clap_plugin_t *plugin) { (void)plugin; }

static uint32_t factory_count(const clap_plugin_factory_t *factory) { (void)factory; return DESC_COUNT; }

static const clap_plugin_descriptor_t *factory_descriptor(const clap_plugin_factory_t *factory, uint32_t index)
{
    (void)factory;
    return index < DESC_COUNT ? &g_descs[index] : NULL;
}

static const clap_plugin_t *factory_create(const clap_plugin_factory_t *factory, const clap_host_t *host, const char *id)
{
    fake_panel_t *p;
    uint32_t i;

    (void)factory;
    for (i = 0; i < DESC_COUNT && (!id || strcmp(id, g_descs[i].id) != 0); i++)
        ;
    if (i == DESC_COUNT || !(p = calloc(1, sizeof(*p))))
        return NULL;
    p->host = host;
    p->values[0] = -20.0;
    p->values[2] = 6.0;
    p->plugin = (clap_plugin_t){ &g_descs[i], p, plugin_init, plugin_destroy, plugin_activate, plugin_deactivate,
                                 plugin_start_processing, plugin_stop_processing, plugin_reset, plugin_process,
                                 plugin_get_extension, plugin_on_main_thread };
    return &p->plugin;
}

static const clap_plugin_factory_t g_factory = { factory_count, factory_descriptor, factory_create };

static bool entry_init(const char *path) { (void)path; return true; }
static void entry_deinit(void) {}
static const void *entry_get_factory(const char *id) { return !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &g_factory : NULL; }

CLAP_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory };
