/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * A tool with no audio port at all and one note input, which the host places as a pass-through (#270): omx-clap-scan
 * lists it as "passthrough: host", and --json as audio_ports.passthrough, which tests/scan-json-check.py holds to one another.
 *
 *   org.omx-clap-host.test.tool   no audio port, one note input
 */

#include <stdlib.h>
#include <string.h>
#include <clap/clap.h>

#define ID_TOOL "org.omx-clap-host.test.tool"

static const char *const g_features[] = { CLAP_PLUGIN_FEATURE_NOTE_EFFECT, NULL };
static const clap_plugin_descriptor_t g_descriptor = { CLAP_VERSION_INIT, ID_TOOL, "tool", "omx-clap-host", "", "", "", "0", "", g_features };

static uint32_t note_ports_count(const clap_plugin_t *plugin, bool is_input)
{
    (void)plugin;
    return is_input ? 1 : 0;
}

static bool note_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_note_port_info_t *info)
{
    (void)plugin;
    if (!is_input || index != 0)
        return false;
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    strcpy(info->name, "notes");
    return true;
}

static const clap_plugin_note_ports_t g_note_ports = { note_ports_count, note_ports_get };

static bool plugin_init(const clap_plugin_t *plugin)
{
    (void)plugin;
    return true;
}

static void plugin_destroy(const clap_plugin_t *plugin)
{
    free((void *)plugin);
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
    (void)plugin;
    (void)process;
    return CLAP_PROCESS_CONTINUE;
}

static const void *plugin_extension(const clap_plugin_t *plugin, const char *id)
{
    (void)plugin;
    return !strcmp(id, CLAP_EXT_NOTE_PORTS) ? &g_note_ports : NULL;
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
    clap_plugin_t *plugin;

    (void)factory;
    (void)host;
    if (strcmp(id, ID_TOOL) || !(plugin = calloc(1, sizeof(*plugin))))
        return NULL;
    plugin->desc = &g_descriptor;
    plugin->init = plugin_init;
    plugin->destroy = plugin_destroy;
    plugin->activate = plugin_activate;
    plugin->deactivate = plugin_deactivate;
    plugin->start_processing = plugin_start_processing;
    plugin->stop_processing = plugin_stop_processing;
    plugin->reset = plugin_reset;
    plugin->process = plugin_process;
    plugin->get_extension = plugin_extension;
    plugin->on_main_thread = plugin_on_main_thread;
    return plugin;
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
