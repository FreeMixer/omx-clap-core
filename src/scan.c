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


/* Lists what CLAP plugin files hold: every plugin of every factory, with its
 * parameters, audio and note ports, note names and latency, read from an
 * initialised instance that is not activated; a plugin that offers
 * clap.voice-info is activated once, at 48 kHz, to read it, since the
 * extension answers only while active. Each file is scanned in a child
 * process so a plugin that crashes or hangs is reported and the scan goes on. */

/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <omxcontract/omx_vocabulary.h>

#include "clap_host.h"
#include "layout_pin.h"
#include "scan_fields.h"
#include "scan_panel.h"


/*
************************************************************************************************************************
*           LOCAL DEFINES
************************************************************************************************************************
*/

/* the one version of the tree, from the file VERSION through the Makefile */
#ifndef OMX_CLAP_VERSION
#error "build with make: the version is passed in OMX_CLAP_VERSION from the file VERSION"
#endif
#define VERSION             OMX_CLAP_VERSION
#define SCAN_TIMEOUT_S      30
#define REASON_SIZE         512
#define CLAP_SUFFIX         ".clap"


/*
************************************************************************************************************************
*           LOCAL DATA TYPES
************************************************************************************************************************
*/

typedef struct FLAG_NAME_T {
    uint32_t bit;
    const char *name;
} flag_name_t;

typedef struct LIST_T {
    char **items;
    size_t count;
} list_t;


/*
************************************************************************************************************************
*           LOCAL GLOBAL VARIABLES
************************************************************************************************************************
*/

static int g_json;
static int g_presets_listed;  ///< the presets the file being written has listed so far

static const flag_name_t g_preset_flags[] = {
    { CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT, "factory" },
    { CLAP_PRESET_DISCOVERY_IS_USER_CONTENT, "user" },
    { CLAP_PRESET_DISCOVERY_IS_DEMO_CONTENT, "demo" },
    { CLAP_PRESET_DISCOVERY_IS_FAVORITE, "favorite" },
};

static const flag_name_t g_param_flags[] = {
    { CLAP_PARAM_IS_STEPPED, "stepped" },
    { CLAP_PARAM_IS_PERIODIC, "periodic" },
    { CLAP_PARAM_IS_HIDDEN, "hidden" },
    { CLAP_PARAM_IS_READONLY, "readonly" },
    { CLAP_PARAM_IS_BYPASS, "bypass" },
    { CLAP_PARAM_IS_AUTOMATABLE, "automatable" },
    { CLAP_PARAM_IS_AUTOMATABLE_PER_NOTE_ID, "automatable_per_note_id" },
    { CLAP_PARAM_IS_AUTOMATABLE_PER_KEY, "automatable_per_key" },
    { CLAP_PARAM_IS_AUTOMATABLE_PER_CHANNEL, "automatable_per_channel" },
    { CLAP_PARAM_IS_AUTOMATABLE_PER_PORT, "automatable_per_port" },
    { CLAP_PARAM_IS_MODULATABLE, "modulatable" },
    { CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID, "modulatable_per_note_id" },
    { CLAP_PARAM_IS_MODULATABLE_PER_KEY, "modulatable_per_key" },
    { CLAP_PARAM_IS_MODULATABLE_PER_CHANNEL, "modulatable_per_channel" },
    { CLAP_PARAM_IS_MODULATABLE_PER_PORT, "modulatable_per_port" },
    { CLAP_PARAM_REQUIRES_PROCESS, "requires_process" },
    { CLAP_PARAM_IS_ENUM, "enum" },
};

#define PARAM_FLAG_COUNT (sizeof(g_param_flags) / sizeof(g_param_flags[0]))


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS
************************************************************************************************************************
*/

/* the length of the UTF-8 sequence at s, 0 when it is not a valid one */
static int utf8_length(const unsigned char *s)
{
    if (s[0] < 0x80)
        return 1;
    if (s[0] >= 0xC2 && s[0] <= 0xDF && (s[1] & 0xC0) == 0x80)
        return 2;
    if (s[0] >= 0xE0 && s[0] <= 0xEF && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80)
        return (s[0] == 0xE0 && s[1] < 0xA0) || (s[0] == 0xED && s[1] >= 0xA0) ? 0 : 3;
    if (s[0] >= 0xF0 && s[0] <= 0xF4 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80)
        return (s[0] == 0xF0 && s[1] < 0x90) || (s[0] == 0xF4 && s[1] >= 0x90) ? 0 : 4;
    return 0;
}

/* a string as a JSON string, or bare in the text listing; a byte that is no UTF-8 becomes ? */
static void put_string(FILE *out, const char *s)
{
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    int n;

    if (g_json)
        fputc('"', out);
    while (*p)
    {
        n = utf8_length(p);
        if (n == 0)
        {
            fputc('?', out);
            p++;
        }
        else if (n > 1)
        {
            fwrite(p, 1, (size_t)n, out);
            p += n;
        }
        else
        {
            if (*p < 0x20 || *p == 0x7F)
            {
                if (g_json)
                    fprintf(out, "\\u%04x", *p);
                else
                    fputc('?', out);
            }
            else if (g_json && (*p == '"' || *p == '\\'))
                fprintf(out, "\\%c", *p);
            else
                fputc(*p, out);
            p++;
        }
    }
    if (g_json)
        fputc('"', out);
}

/* JSON has no infinity: a bound the plugin leaves open is null */
static void put_number(FILE *out, double value)
{
    if (isfinite(value))
        fprintf(out, "%.17g", value);
    else
        fputs(g_json ? "null" : "inf", out);
}

static void put_flags(FILE *out, uint32_t flags)
{
    size_t i;
    int first = 1;

    if (g_json)
        fputc('[', out);
    for (i = 0; i < PARAM_FLAG_COUNT; i++)
    {
        if (!(flags & g_param_flags[i].bit))
            continue;
        if (g_json)
            fprintf(out, "%s\"%s\"", first ? "" : ",", g_param_flags[i].name);
        else
            fprintf(out, "%s%s", first ? "" : ",", g_param_flags[i].name);
        first = 0;
    }
    if (g_json)
        fputc(']', out);
}

static void put_features(FILE *out, const char *const *features)
{
    int first = 1;

    if (g_json)
        fputc('[', out);
    for (; features && *features; features++)
    {
        if (!first)
            fputs(g_json ? "," : " ", out);
        put_string(out, *features);
        first = 0;
    }
    if (g_json)
        fputc(']', out);
}

/* the most whole values of an enum parameter's range that are asked for a label */
#define MAX_ENUM_LABELS 1024u

/* an enum parameter's labels, the plugin's value_to_text at every whole value of its range: "labels":[{"value":v,
 * "label":"text"}], a value the plugin gives no text for left out, a range open or wider than MAX_ENUM_LABELS empty */
static void put_enum_labels(FILE *out, const struct omx_clap_instance *instance, const clap_param_info_t *info)
{
    const clap_plugin_params_t *params = instance->params;
    int first = 1;
    double v;

    fputs(",\"labels\":[", out);
    if (params->value_to_text && isfinite(info->min_value) && isfinite(info->max_value) &&
        info->max_value - info->min_value < MAX_ENUM_LABELS)
    {
        for (v = ceil(info->min_value); v <= info->max_value; v += 1.0)
        {
            char text[CLAP_NAME_SIZE];

            memset(text, 0, sizeof(text));
            if (!params->value_to_text(instance->plugin, info->id, v, text, sizeof(text)))
                continue;
            text[sizeof(text) - 1] = '\0';
            fprintf(out, "%s{\"value\":", first ? "" : ",");
            put_number(out, v);
            fputs(",\"label\":", out);
            put_string(out, text);
            fputc('}', out);
            first = 0;
        }
    }
    fputc(']', out);
}

/* the plugin's remote-control pages, JSON only: "remote_controls":[{"id","section","name","params":[eight ids, an empty
 * slot null],"forPreset"}], empty for a plugin without the extension */
static void put_remote_controls(FILE *out, const struct omx_clap_instance *instance)
{
    const clap_plugin_t *p = instance->plugin;
    const clap_plugin_remote_controls_t *rc = OMX_ABI_CALL(NULL, "get_extension", p->get_extension, p, CLAP_EXT_REMOTE_CONTROLS);
    uint32_t count, i, k;
    int first = 1;

    if (!rc)
        rc = OMX_ABI_CALL(NULL, "get_extension", p->get_extension, p, CLAP_EXT_REMOTE_CONTROLS_COMPAT);
    count = rc ? OMX_ABI_CALL(NULL, "rc.count", rc->count, p) : 0;
    fputs("\"remote_controls\":[", out);
    for (i = 0; i < count; i++)
    {
        clap_remote_controls_page_t page;

        memset(&page, 0, sizeof(page));
        if (!OMX_ABI_CALL(NULL, "rc.get", rc->get, p, i, &page))
            continue;
        page.section_name[sizeof(page.section_name) - 1] = '\0';
        page.page_name[sizeof(page.page_name) - 1] = '\0';
        fprintf(out, "%s{\"id\":%u,\"section\":", first ? "" : ",", page.page_id);
        put_string(out, page.section_name);
        fputs(",\"name\":", out);
        put_string(out, page.page_name);
        fputs(",\"params\":[", out);
        for (k = 0; k < CLAP_REMOTE_CONTROLS_COUNT; k++)
        {
            if (k)
                fputc(',', out);
            if (page.param_ids[k] == CLAP_INVALID_ID)
                fputs("null", out);
            else
                fprintf(out, "%u", page.param_ids[k]);
        }
        fprintf(out, "],\"forPreset\":%s}", page.is_for_preset ? "true" : "false");
        first = 0;
    }
    fputc(']', out);
}

static void put_params(FILE *out, const struct omx_clap_instance *instance)
{
    const clap_plugin_params_t *params = instance->params;
    clap_param_info_t info;
    uint32_t count = params ? params->count(instance->plugin) : 0;
    uint32_t i;
    int first = 1;

    if (g_json)
        fputs("\"params\":[", out);
    for (i = 0; i < count; i++)
    {
        memset(&info, 0, sizeof(info));
        if (!params->get_info(instance->plugin, i, &info))
            continue;
        info.name[sizeof(info.name) - 1] = '\0';
        info.module[sizeof(info.module) - 1] = '\0';
        if (g_json)
        {
            fprintf(out, "%s\n{\"id\":%u,\"name\":", first ? "" : ",", info.id);
            put_string(out, info.name);
            fputs(",\"module\":", out);
            put_string(out, info.module);
            fputs(",\"min\":", out);
            put_number(out, info.min_value);
            fputs(",\"max\":", out);
            put_number(out, info.max_value);
            fputs(",\"default\":", out);
            put_number(out, info.default_value);
            fputs(",\"flags\":", out);
            put_flags(out, info.flags);
            if (info.flags & CLAP_PARAM_IS_ENUM)
                put_enum_labels(out, instance, &info);
            fputc('}', out);
        }
        else
        {
            fprintf(out, "    param %u ", info.id);
            put_string(out, info.name);
            fputs(" [", out);
            put_number(out, info.min_value);
            fputs(" .. ", out);
            put_number(out, info.max_value);
            fputs("] default ", out);
            put_number(out, info.default_value);
            fputc(' ', out);
            put_flags(out, info.flags);
            fputc('\n', out);
        }
        first = 0;
    }
    if (g_json)
        fputs(first ? "]" : "\n]", out);
}

/* a port's kind as the description names it: its CLAP port type when the core knows it, else unknown */
static const char *kind_name(const char *port_type)
{
    static const char *const known[] = { CLAP_PORT_MONO, CLAP_PORT_STEREO, CLAP_PORT_SURROUND, CLAP_PORT_AMBISONIC };
    uint32_t i;

    for (i = 0; port_type && i < sizeof(known) / sizeof(known[0]); i++)
        if (!strcmp(port_type, known[i]))
            return known[i];
    return "unknown";
}

/* an ambisonic ordering and normalisation by name, as clap/ext/ambisonic.h spells them, lower case */
static const char *ordering_name(uint32_t ordering)
{
    return ordering == CLAP_AMBISONIC_ORDERING_FUMA ? "fuma" : ordering == CLAP_AMBISONIC_ORDERING_ACN ? "acn" : "unknown";
}

static const char *normalization_name(uint32_t normalization)
{
    static const char *const names[] = { "maxn", "sn3d", "n3d", "sn2d", "n2d" };

    return normalization < sizeof(names) / sizeof(names[0]) ? names[normalization] : "unknown";
}

/*
 * What a surround or an ambisonic port's channels are, as the description reads them, into `t` (its kind too, and
 * `map` for a surround port): a surround port's map from clap.surround (the ids as the plugin names them, as many as it
 * stores), in index order without it; an ambisonic port's ordering and normalisation from clap.ambisonic, ACN and SN3D
 * marked assumed without it or when its get_config answers false. For a plugin the core admits, put_audio_ports takes
 * `assumed` from the description (its ambisonic_assumed). Nothing for any other port, nor for a surround port of more
 * than CLAP_HOST_PORT_CHANNELS channels, which the core refuses port-too-wide; a map is as long as the plugin says it
 * is, up to CLAP_HOST_PORT_CHANNELS ids.
 */
static void port_layout(const struct omx_clap_instance *instance, const clap_audio_port_info_t *info, bool is_input, uint32_t index,
                        uint8_t map[CLAP_HOST_PORT_CHANNELS], struct layout_port_text *t)
{
    t->kind = kind_name(info->port_type);
    if (!strcmp(t->kind, CLAP_PORT_SURROUND) && info->channel_count <= CLAP_HOST_PORT_CHANNELS)
    {
        uint32_t n = info->channel_count, c;

        if (instance->surround && instance->surround->get_channel_map)
            n = instance->surround->get_channel_map(instance->plugin, is_input, index, map, CLAP_HOST_PORT_CHANNELS);
        else
            for (c = 0; c < n; c++)
                map[c] = (uint8_t)c;
        t->map = map;
        t->map_n = n > CLAP_HOST_PORT_CHANNELS ? CLAP_HOST_PORT_CHANNELS : n;
    }
    else if (!strcmp(t->kind, CLAP_PORT_AMBISONIC))
    {
        clap_ambisonic_config_t config = { CLAP_AMBISONIC_ORDERING_ACN, CLAP_AMBISONIC_NORMALIZATION_SN3D };

        t->assumed = !instance->ambisonic || !instance->ambisonic->get_config
                     || !instance->ambisonic->get_config(instance->plugin, is_input, index, &config);
        if (t->assumed)
            config = (clap_ambisonic_config_t){ CLAP_AMBISONIC_ORDERING_ACN, CLAP_AMBISONIC_NORMALIZATION_SN3D };
        t->ordering = ordering_name(config.ordering);
        t->normalization = normalization_name(config.normalization);
    }
}

/* one port in the JSON: its fields, its layout, and the JACK ports omx-clap-host registers for it when it admits the
 * plugin (`t->jack`) */
static void put_port_json(FILE *out, const clap_audio_port_info_t *info, const struct layout_port_text *t)
{
    char name[LAYOUT_PORT_NAME_SIZE];
    uint32_t k;

    fprintf(out, "{\"id\":%u,\"name\":", info->id);
    put_string(out, info->name);
    fprintf(out, ",\"role\":\"%s\",\"channels\":%u,\"kind\":\"%s\"", t->aux == LAYOUT_PORT_MAIN ? "main" : "aux", info->channel_count, t->kind);
    if (t->map)
    {
        fputs(",\"map\":[", out);
        for (k = 0; k < t->map_n; k++)
            fprintf(out, "%s%u", k ? "," : "", t->map[k]);
        fputc(']', out);
    }
    else if (t->ordering)
        fprintf(out, ",\"ordering\":\"%s\",\"normalization\":\"%s\"%s", t->ordering, t->normalization, t->assumed ? ",\"assumed\":true" : "");
    if (t->jack)
    {
        fputs(",\"jack\":[", out);
        for (k = 0; k < info->channel_count; k++)
        {
            layout_port_name(name, t->is_input, t->aux, k);
            fprintf(out, "%s\"%s\"", k ? "," : "", name);
        }
        fputc(']', out);
    }
    fputc('}', out);
}

/* every audio port of each side, the JSON's or a line each in the text (layout_port_text); the JACK names only when the
 * core admits the plugin (`accepted`), the main port named apart and the others in their order */
static void put_audio_ports(FILE *out, const struct omx_clap_instance *instance, bool accepted)
{
    const clap_plugin_audio_ports_t *ports = instance->audio_ports;
    clap_audio_port_info_t info;
    uint32_t count, i;
    int dir;

    if (g_json)
        fputs("\"audio_ports\":{", out);
    for (dir = 0; dir < 2; dir++)
    {
        const bool is_input = dir == 0;
        int first = 1;
        uint32_t aux = 0;

        count = ports ? ports->count(instance->plugin, is_input) : 0;
        if (g_json)
            fprintf(out, "%s\"%s\":[", dir ? "," : "", is_input ? "inputs" : "outputs");
        for (i = 0; i < count; i++)
        {
            uint8_t map[CLAP_HOST_PORT_CHANNELS];
            struct layout_port_text t;

            memset(&info, 0, sizeof(info));
            if (!ports->get(instance->plugin, i, is_input, &info))
                continue;
            info.name[sizeof(info.name) - 1] = '\0';
            memset(&t, 0, sizeof(t));
            t.is_input = is_input;
            t.aux = (info.flags & CLAP_AUDIO_PORT_IS_MAIN) ? LAYOUT_PORT_MAIN : aux++;
            t.channels = info.channel_count;
            t.name = info.name;
            t.jack = accepted;
            port_layout(instance, &info, is_input, i, map, &t);
            // an admitted plugin's description says whether its ambisonic configuration is the host's assumption
            if (accepted && t.ordering)
            {
                const struct omx_clap_port *d = t.aux == LAYOUT_PORT_MAIN ? (is_input ? &instance->main_in : &instance->main_out)
                                              : is_input ? &instance->aux_in[t.aux] : &instance->aux_out[t.aux];

                t.assumed = d->ambisonic_assumed != 0;
            }
            if (g_json)
            {
                if (!first)
                    fputc(',', out);
                put_port_json(out, &info, &t);
            }
            else
            {
                fputs("    ", out);
                layout_port_text(out, &t, 0);
                fputc('\n', out);
            }
            first = 0;
        }
        if (g_json)
            fputc(']', out);
    }
    // a tool with no audio port at all is hosted on the host's placement, the stereo pair named for it (#270)
    if (accepted && instance->passthrough)
    {
        if (g_json)
            fputs(",\"passthrough\":\"host\"", out);
        else
            fputs("    passthrough: host (no audio port: the strip's stereo pair passes through untouched)\n", out);
    }
    if (g_json)
        fputc('}', out);
}

static void put_note_ports(FILE *out, const struct omx_clap_instance *instance)
{
    const clap_plugin_note_ports_t *ports = instance->note_ports;
    const uint32_t inputs = ports ? ports->count(instance->plugin, true) : 0;
    const uint32_t outputs = ports ? ports->count(instance->plugin, false) : 0;

    if (g_json)
        fprintf(out, "\"note_ports\":{\"inputs\":%u,\"outputs\":%u}", inputs, outputs);
    else
        fprintf(out, "    note ports %u in, %u out\n", inputs, outputs);
}

/* the names a drum machine gives its keys (clap.note-name), each with the port, key and channel it names (-1: every
 * one); left out when the plugin names none */
static void put_note_names(FILE *out, struct omx_clap_instance *instance)
{
    const uint32_t count = omx_clap_host_note_name_count(instance);
    uint32_t i, put = 0;

    for (i = 0; i < count; i++)
    {
        clap_note_name_t name;

        if (omx_clap_host_note_name(instance, i, &name) != 0)
            continue;
        if (g_json)
        {
            fputs(put++ ? "," : ",\"note_names\":[", out);
            fputs("{\"name\":", out);
            put_string(out, name.name);
            fprintf(out, ",\"port\":%d,\"key\":%d,\"channel\":%d}", name.port, name.key, name.channel);
        }
        else
        {
            fputs("    note name \"", out);
            put_string(out, name.name);
            fprintf(out, "\" port %d key %d channel %d\n", name.port, name.key, name.channel);
        }
    }
    if (g_json && put)
        fputc(']', out);
}

/* the scan's one activation, for clap.voice-info alone (the extension answers only while active): the voices the patch
 * uses, the voices allocated and the flags; left out when the plugin offers none, or answers outside its own rule */
#define SCAN_RATE       48000.0
#define SCAN_MAX_FRAMES 1024u
static void put_voice_info(FILE *out, struct omx_clap_instance *instance)
{
    const clap_plugin_t *plugin = instance->plugin;
    const clap_plugin_voice_info_t *ext = OMX_ABI_CALL(NULL, "get_extension", plugin->get_extension, plugin, CLAP_EXT_VOICE_INFO);
    clap_voice_info_t info;
    bool read;

    if (!ext || !ext->get || !OMX_ABI_BOOL(omx_abi_gd(NULL, "activate", (const void *)plugin->activate, OMX_ABI_A(plugin), 1,
                                                                   SCAN_MAX_FRAMES, 0, SCAN_RATE)))
        return;
    memset(&info, 0, sizeof(info));
    read = OMX_ABI_CALL(NULL, "ext.get", ext->get, plugin, &info) && info.voice_count >= 1 && info.voice_count <= info.voice_capacity;
    omx_abi_g(NULL, "deactivate", (const void *)plugin->deactivate, OMX_ABI_A(plugin), 0, 0, 0, 0, 0);
    if (!read)
        return;
    if (g_json)
        fprintf(out, ",\"voice_info\":{\"count\":%u,\"capacity\":%u,\"overlapping_notes\":%s}", info.voice_count,
                info.voice_capacity, (info.flags & CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES) ? "true" : "false");
    else
        fprintf(out, "    voices %u of %u%s\n", info.voice_count, info.voice_capacity,
                (info.flags & CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES) ? ", overlapping notes" : "");
}

/* the resource hints the plugin declares (org.openmixer.resource-hints.draft/0), as the core read them at create: every
 * hint by omx-contract's word with its unit and its figure, null when unknown, never 0; and the code when the core refused
 * the declaration. Left out when the plugin states none. */
static void put_resource_hints(FILE *out, const struct omx_clap_instance *instance)
{
    omx_resource_hints_t hints;
    const char *why = NULL;
    const int rc = omx_clap_host_resource_hints(instance, &hints, &why);
    uint32_t i, put = 0;

    if (rc == 0)
        return;
    if (g_json)
    {
        fprintf(out, ",\"resource_hints\":{\"declared\":%u", hints.declared);
        if (why)
            fprintf(out, ",\"refused\":\"%s\"", why);
    }
    else if (why)
        fprintf(out, "    resource hints refused: %s\n", why);
    else
        fputs("    resource hints", out);
    for (i = 0; i < OMX_RESOURCE_HINT_COUNT; i++)
    {
        const char *unit = omx_clap_resource_hint_unit(i);
        const int flag = !strcmp(unit, OMX_RESOURCE_UNITS_FLAG);
        uint64_t figure;
        const int known = omx_clap_resource_hint_figure(&hints, i, &figure) == 1;

        if (g_json)
        {
            fprintf(out, ",\"%s\":{\"unit\":\"%s\",\"figure\":", omx_clap_resource_hint_word(i), unit);
            if (!known)
                fputs("null}", out);
            else if (flag)
                fputs(figure ? "true}" : "false}", out);
            else
                fprintf(out, "%llu}", (unsigned long long)figure);
        }
        else if (known && !why)
        {
            if (flag)
                fprintf(out, "%s %s %s", put++ ? "," : "", omx_clap_resource_hint_word(i), figure ? "yes" : "no");
            else
                fprintf(out, "%s %s %llu %s", put++ ? "," : "", omx_clap_resource_hint_word(i), (unsigned long long)figure, unit);
        }
    }
    if (g_json)
        fputc('}', out);
    else if (!why)
        fputc('\n', out);
}

/* a parameter or meter id of the panel description, null for none */
static void put_panel_id(FILE *out, uint32_t id)
{
    if (id == CLAP_INVALID_ID)
        fputs("null", out);
    else
        fprintf(out, "%u", id);
}

/* a code of the panel description by omx-plugins' word for it, as its number when the extension names none */
static void put_panel_code(FILE *out, const char *word, uint32_t code)
{
    if (word)
        fprintf(out, "\"%s\"", word);
    else
        fprintf(out, "%u", code);
}

static void put_panel_text(FILE *out, const char *key, const char *value)
{
    fprintf(out, ",\"%s\":", key);
    put_string(out, value);
}

/* a table of the panel description opened under its key: its entries' array, or for a table the scan refused the count
 * the plugin answered and the maximum it is past, in place of the array */
static void put_panel_open(FILE *out, const struct scan_panel *p, uint32_t table, const char *key)
{
    fprintf(out, ",\"%s\":", key);
    if (p->table[table].refused)
        fprintf(out, "{\"refused\":\"too-many\",\"count\":%u,\"max\":%u}", p->table[table].refused,
                OMX_CLAP_PANEL_DESCRIPTION_MAX_ENTRIES);
    else
        fputc('[', out);
}

static void put_panel_close(FILE *out, const struct scan_panel *p, uint32_t table)
{
    if (!p->table[table].refused)
        fputc(']', out);
}

/* the listing's count of one table, its refusal when the scan refused it */
static void put_panel_count(FILE *out, const struct scan_panel *p, uint32_t table, const char *name)
{
    if (p->table[table].refused)
        fprintf(out, ", %s refused (%u past %u)", name, p->table[table].refused, OMX_CLAP_PANEL_DESCRIPTION_MAX_ENTRIES);
    else
        fprintf(out, ", %u %s", p->table[table].n, name);
}

/* the panel description the plugin publishes (org.openmixer.panel-description/1), in the extension's shape: its family,
 * its fingerprint and its five tables, each entry as its get filled it; null when the plugin serves none. The listing for
 * a person counts the tables. */
static void put_panel(FILE *out, const struct omx_clap_instance *instance)
{
    struct scan_panel p;
    const int rc = scan_panel_read(instance->plugin, &p);
    uint32_t i;

    if (!g_json)
    {
        if (rc != 1)
            return;
        fprintf(out, "    panel %s",
                p.has_family && scan_panel_family_word(p.family) ? scan_panel_family_word(p.family) : "of no family");
        put_panel_count(out, &p, SCAN_PANEL_SECTIONS, "sections");
        put_panel_count(out, &p, SCAN_PANEL_CONTROLS, "controls");
        put_panel_count(out, &p, SCAN_PANEL_ROLES, "roles");
        put_panel_count(out, &p, SCAN_PANEL_DISPLAYS, "displays");
        put_panel_count(out, &p, SCAN_PANEL_PLACEMENT_DEFAULTS, "placement defaults");
        fputc('\n', out);
        scan_panel_free(&p);
        return;
    }
    if (rc != 1)
    {
        fputs(rc == 0 ? ",\"panel\":null" : ",\"panel\":{\"error\":\"out of memory\"}", out);
        return;
    }
    fputs(",\"panel\":{\"family\":", out);
    if (p.has_family)
        put_panel_code(out, scan_panel_family_word(p.family), p.family);
    else
        fputs("null", out);
    fputs(",\"fingerprint\":", out);
    if (p.has_fingerprint)
        put_string(out, p.fingerprint);
    else
        fputs("null", out);
    put_panel_open(out, &p, SCAN_PANEL_SECTIONS, "sections");
    for (i = 0; i < p.table[SCAN_PANEL_SECTIONS].n; i++)
    {
        const omx_clap_panel_description_section_t *s = SCAN_PANEL_ROW(&p, SCAN_PANEL_SECTIONS, omx_clap_panel_description_section_t, i);

        fprintf(out, "%s{\"key\":", i ? "," : "");
        put_string(out, s->key);
        put_panel_text(out, "labelKey", s->label_key);
        put_panel_text(out, "label", s->label);
        fputc('}', out);
    }
    put_panel_close(out, &p, SCAN_PANEL_SECTIONS);
    put_panel_open(out, &p, SCAN_PANEL_CONTROLS, "controls");
    for (i = 0; i < p.table[SCAN_PANEL_CONTROLS].n; i++)
    {
        const omx_clap_panel_description_control_t *c = SCAN_PANEL_ROW(&p, SCAN_PANEL_CONTROLS, omx_clap_panel_description_control_t, i);

        fprintf(out, "%s{\"paramId\":", i ? "," : "");
        put_panel_id(out, c->param_id);
        fprintf(out, ",\"section\":%u,\"widget\":", c->section);
        put_panel_code(out, scan_panel_widget_word(c->widget), c->widget);
        put_panel_text(out, "labelKey", c->label_key);
        put_panel_text(out, "label", c->label);
        fputs(",\"visibleParamId\":", out);
        put_panel_id(out, c->visible_param_id);
        fprintf(out, ",\"visibleMask\":\"%llx\"}", (unsigned long long)c->visible_mask);
    }
    put_panel_close(out, &p, SCAN_PANEL_CONTROLS);
    put_panel_open(out, &p, SCAN_PANEL_ROLES, "roles");
    for (i = 0; i < p.table[SCAN_PANEL_ROLES].n; i++)
    {
        const omx_clap_panel_description_role_t *r = SCAN_PANEL_ROW(&p, SCAN_PANEL_ROLES, omx_clap_panel_description_role_t, i);

        fprintf(out, "%s{\"role\":", i ? "," : "");
        put_string(out, r->role);
        fprintf(out, ",\"index\":%u,\"paramId\":", r->index);
        put_panel_id(out, r->param_id);
        fputc('}', out);
    }
    put_panel_close(out, &p, SCAN_PANEL_ROLES);
    put_panel_open(out, &p, SCAN_PANEL_DISPLAYS, "displays");
    for (i = 0; i < p.table[SCAN_PANEL_DISPLAYS].n; i++)
    {
        const omx_clap_panel_description_display_t *d = SCAN_PANEL_ROW(&p, SCAN_PANEL_DISPLAYS, omx_clap_panel_description_display_t, i);

        fprintf(out, "%s{\"kind\":", i ? "," : "");
        put_panel_code(out, scan_panel_display_kind_word(d->kind), d->kind);
        fprintf(out, ",\"section\":%u,\"source\":", d->section);
        put_panel_code(out, scan_panel_display_source_word(d->source), d->source);
        fputs(",\"meterIds\":[", out);
        put_panel_id(out, d->meter_ids[0]);
        fputc(',', out);
        put_panel_id(out, d->meter_ids[1]);
        fprintf(out, "],\"curveIndex\":%u,\"floorParamId\":", d->curve_index);
        put_panel_id(out, d->floor_param_id);
        fputs(",\"floorDb\":", out);
        put_number(out, d->floor_db);
        put_panel_text(out, "labelKey", d->label_key);
        put_panel_text(out, "label", d->label);
        fputc('}', out);
    }
    put_panel_close(out, &p, SCAN_PANEL_DISPLAYS);
    put_panel_open(out, &p, SCAN_PANEL_PLACEMENT_DEFAULTS, "placementDefaults");
    for (i = 0; i < p.table[SCAN_PANEL_PLACEMENT_DEFAULTS].n; i++)
    {
        const omx_clap_panel_description_default_t *d = SCAN_PANEL_ROW(&p, SCAN_PANEL_PLACEMENT_DEFAULTS, omx_clap_panel_description_default_t, i);

        fprintf(out, "%s{\"placement\":", i ? "," : "");
        put_panel_code(out, scan_panel_placement_word(d->placement), d->placement);
        fputs(",\"paramId\":", out);
        put_panel_id(out, d->param_id);
        fputs(",\"value\":", out);
        put_number(out, d->value);
        fputc('}', out);
    }
    put_panel_close(out, &p, SCAN_PANEL_PLACEMENT_DEFAULTS);
    fputc('}', out);
    scan_panel_free(&p);
}

/* what a plugin that is only initialised answers; a plugin not yet activated may say 0, so 0 is left out as unknown */
static void put_latency(FILE *out, const struct omx_clap_instance *instance)
{
    uint32_t frames;

    if (!instance->latency || instance->passthrough)   // a pass-through tool's latency applies to its notes: the host reads 0
        return;
    frames = instance->latency->get(instance->plugin);
    if (frames == 0)
        return;
    if (g_json)
        fprintf(out, ",\"latency\":%u", frames);
    else
        fprintf(out, "    latency %u frames\n", frames);
}

static void put_descriptor(FILE *out, const clap_plugin_descriptor_t *desc)
{
    if (g_json)
    {
        fputs("{\"id\":", out);
        put_string(out, desc->id);
        fputs(",\"name\":", out);
        put_string(out, desc->name);
        fputs(",\"vendor\":", out);
        put_string(out, desc->vendor);
        fputs(",\"version\":", out);
        put_string(out, desc->version);
        fputs(",\"description\":", out);
        put_string(out, desc->description);
        fputs(",\"url\":", out);
        put_string(out, desc->url);
        fputs(",\"features\":", out);
        put_features(out, desc->features);
    }
    else
    {
        fputs("  plugin ", out);
        put_string(out, desc->id);
        fputc(' ', out);
        layout_port_quoted(out, desc->name);
        fputs(" vendor ", out);
        put_string(out, desc->vendor);
        fputs(" version ", out);
        put_string(out, desc->version);
        fputs("\n    features ", out);
        put_features(out, desc->features);
        fputc('\n', out);
    }
}

/* a plugin's entry: from an instance the core opened, whose layout it admits, or, when it refuses the plugin, from one
 * only created, whose ports are listed with no JACK name, since omx-clap-host would register none */
static void put_plugin(FILE *out, const char *path, struct omx_clap_binary *binary, const clap_plugin_descriptor_t *desc)
{
    struct omx_clap_instance *instance;
    char why[OMX_CLAP_WHY_MAX];
    bool accepted;

    put_descriptor(out, desc);
    accepted = omx_clap_host_open(path, desc->id, &instance, why) == 0 && instance;
    if (!accepted && omx_clap_host_create(binary, desc, &instance) != 0)
    {
        if (g_json)
            fputs(",\"error\":\"plugin failed to create or init\"}", out);
        else
            fputs("    error: plugin failed to create or init\n", out);
        return;
    }

    if (g_json)
        fputc(',', out);
    put_params(out, instance);
    if (g_json)
    {
        fputc(',', out);
        put_remote_controls(out, instance);
        fputc(',', out);
    }
    put_audio_ports(out, instance, accepted);
    if (g_json)
        fputc(',', out);
    put_note_ports(out, instance);
    put_note_names(out, instance);
    put_latency(out, instance);
    put_voice_info(out, instance);
    put_resource_hints(out, instance);
    put_panel(out, instance);
    if (g_json)
        fputc('}', out);
    omx_clap_host_close(instance);
}

/* one file's entry, written from the child */
static void put_preset_flags(FILE *out, uint32_t flags)
{
    size_t i;
    int first = 1;

    for (i = 0; i < sizeof(g_preset_flags) / sizeof(g_preset_flags[0]); i++)
        if (flags & g_preset_flags[i].bit)
        {
            fprintf(out, "%s\"%s\"", first ? "" : ",", g_preset_flags[i].name);
            first = 0;
        }
}

/* one preset of the file's preset-discovery factory, as a person or the JSON reads it */
static int put_preset(void *ctx, const struct omx_clap_preset *p)
{
    FILE *out = ctx;
    const char *kind = p->location_kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN ? "plugin" : "file";
    uint32_t i;

    if (g_json)
    {
        fputs(g_presets_listed++ ? ",\n{\"name\":" : "\n{\"name\":", out);
        put_string(out, p->name);
        fprintf(out, ",\"location_kind\":\"%s\",\"location\":", kind);
        if (p->location)
            put_string(out, p->location);
        else
            fputs("null", out);
        fputs(",\"load_key\":", out);
        if (p->load_key)
            put_string(out, p->load_key);
        else
            fputs("null", out);
        fputs(",\"provider\":", out);
        put_string(out, p->provider_id);
        fputs(",\"plugins\":[", out);
        for (i = 0; i < p->plugin_count; i++)
        {
            if (i)
                fputc(',', out);
            put_string(out, p->plugin_ids[i]);
        }
        fputs("],\"flags\":[", out);
        put_preset_flags(out, p->flags);
        fputs("]}", out);
    }
    else
    {
        g_presets_listed++;
        fputs("  preset \"", out);
        put_string(out, p->name);
        fprintf(out, "\" %s ", kind);
        put_string(out, p->location ? p->location : "-");
        fputs(" key ", out);
        put_string(out, p->load_key ? p->load_key : "-");
        for (i = 0; i < p->plugin_count; i++)
        {
            fputs(i ? "," : " for ", out);
            put_string(out, p->plugin_ids[i]);
        }
        fputc('\n', out);
    }
    return 0;
}

/* the file's presets, when it has a preset-discovery factory: the key is absent when it has none */
static void put_presets(FILE *out, struct omx_clap_binary *binary)
{
    g_presets_listed = 0;
    if (g_json)
    {
        // a file with no factory has no key at all, which is known only after the listing: it is written to a buffer
        char *buf = NULL;
        size_t len = 0;
        FILE *mem = open_memstream(&buf, &len);
        int listed;

        if (!mem)
            return;
        listed = omx_clap_host_presets(binary, put_preset, mem);
        fclose(mem);
        if (listed >= 0)
            fprintf(out, ",\"presets\":[%s%s]", buf ? buf : "", g_presets_listed ? "\n" : "");
        free(buf);
        return;
    }
    omx_clap_host_presets(binary, put_preset, out);
}

static void put_file(FILE *out, const char *path)
{
    char reason[REASON_SIZE], local[PATH_MAX];
    const char *opened = path;
    struct omx_clap_binary *binary;
    uint32_t count, i;
    int first = 1;

    if (g_json)
    {
        fputs("{\"path\":", out);
        put_string(out, path);
    }
    else
    {
        fputs("file ", out);
        put_string(out, path);
        fputc('\n', out);
    }

    /* dlopen searches the library path for a name with no slash, and the file is here */
    if (!strchr(path, '/') && snprintf(local, sizeof(local), "./%s", path) < (int)sizeof(local))
        opened = local;
    binary = omx_clap_host_binary_open(opened, reason, sizeof(reason));
    if (!binary)
    {
        if (g_json)
        {
            // dlopen as the core opens it tells the loader's refusal from the entry's; the child ends after this file
            void *so = dlopen(strchr(path, '/') ? path : local, RTLD_NOW | RTLD_LOCAL);

            fputs(",\"code\":", out);
            put_string(out, so ? "entry" : "dlopen");
            fputs(",\"error\":", out);
            put_string(out, reason);
            fputc('}', out);
        }
        else
        {
            fputs("  error: ", out);
            put_string(out, reason);
            fputc('\n', out);
        }
        return;
    }

    if (g_json)
        fputs(",\"plugins\":[", out);
    count = omx_clap_host_binary_count(binary);
    for (i = 0; i < count; i++)
    {
        const clap_plugin_descriptor_t *desc = omx_clap_host_binary_descriptor(binary, i);
        if (!desc || !desc->id)
            continue;
        if (g_json)
            fputs(first ? "\n" : ",\n", out);
        put_plugin(out, opened, binary, desc);
        first = 0;
    }
    if (g_json)
        fputs(first ? "]" : "\n]", out);
    put_presets(out, binary);
    if (g_json)
        fputc('}', out);
    omx_clap_host_binary_close(binary);
}

/* a file that gave no listing: its reason for a person, and in JSON its code for a program (timeout, crash:<signal>,
 * dlopen, entry, unreadable, pipe, fork, no-result) */
static void put_failure(FILE *out, const char *path, const char *code, const char *reason)
{
    if (g_json)
    {
        fputs("{\"path\":", out);
        put_string(out, path);
        fputs(",\"code\":", out);
        put_string(out, code);
        fputs(",\"error\":", out);
        put_string(out, reason);
        fputc('}', out);
    }
    else
    {
        fputs("file ", out);
        put_string(out, path);
        fputs("\n  error: ", out);
        put_string(out, reason);
        fputc('\n', out);
    }
}

static int read_all(int fd, char **data, size_t *size)
{
    size_t capacity = 65536;
    ssize_t n;

    *data = malloc(capacity);
    *size = 0;
    for (;;)
    {
        if (*size == capacity)
        {
            capacity *= 2;
            *data = realloc(*data, capacity);
        }
        n = read(fd, *data + *size, capacity - *size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return n < 0 ? -1 : 0;
        *size += (size_t)n;
    }
}

/* the file scanned in a child, its entry written to out; a child that dies or hangs is the reason */
static void scan_file(FILE *out, const char *path)
{
    char reason[REASON_SIZE];
    int fds[2], status = 0;
    char *data;
    size_t size;
    pid_t pid;

    fflush(out);
    if (pipe(fds) != 0)
    {
        put_failure(out, path, "pipe", "no pipe for the scan");
        return;
    }
    pid = fork();
    if (pid < 0)
    {
        close(fds[0]);
        close(fds[1]);
        put_failure(out, path, "fork", "no process for the scan");
        return;
    }
    if (pid == 0)
    {
        FILE *pipe_out;

        close(fds[0]);
        dup2(STDERR_FILENO, STDOUT_FILENO);
        alarm(SCAN_TIMEOUT_S);
        pipe_out = fdopen(fds[1], "w");
        put_file(pipe_out, path);
        fflush(pipe_out);
        _exit(0);
    }
    close(fds[1]);
    read_all(fds[0], &data, &size);
    close(fds[0]);
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && size > 0)
        fwrite(data, 1, size, out);
    else if (WIFSIGNALED(status))
    {
        char code[32];

        if (WTERMSIG(status) == SIGALRM)
        {
            snprintf(code, sizeof(code), "timeout");
            snprintf(reason, sizeof(reason), "timed out after %i s", SCAN_TIMEOUT_S);
        }
        else
        {
            snprintf(code, sizeof(code), "crash:%i", WTERMSIG(status));
            snprintf(reason, sizeof(reason), "crashed with signal %i (%s)", WTERMSIG(status), strsignal(WTERMSIG(status)));
        }
        put_failure(out, path, code, reason);
    }
    else
        put_failure(out, path, "no-result", "the scan ended without a result");
    free(data);
}

static void list_add(list_t *list, char *item)
{
    list->items = realloc(list->items, (list->count + 1) * sizeof(char *));
    list->items[list->count++] = item;
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int has_suffix(const char *name, const char *suffix)
{
    const size_t n = strlen(name), m = strlen(suffix);
    return n > m && strcmp(name + n - m, suffix) == 0;
}

/* the .clap files under a directory, in name order; a directory that can't be read goes on the failures list */
static int walk(const char *dir, list_t *files, list_t *failures)
{
    list_t names = { NULL, 0 };
    struct dirent *entry;
    struct stat st;
    DIR *handle = opendir(dir);
    size_t i;
    unsigned here = 0;

    if (!handle)
    {
        char *failure;
        if (asprintf(&failure, "%s\tunreadable\t%s", dir, strerror(errno)) > 0)
            list_add(failures, failure);
        return 0;
    }
    while ((entry = readdir(handle)) != NULL)
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
            list_add(&names, strdup(entry->d_name));
    closedir(handle);
    qsort(names.items, names.count, sizeof(char *), compare_names);

    for (i = 0; i < names.count; i++)
    {
        char *path;
        if (asprintf(&path, "%s/%s", dir, names.items[i]) < 0)
            continue;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            walk(path, files, failures);
        else if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && has_suffix(names.items[i], CLAP_SUFFIX))
        {
            list_add(files, path);
            here++;
        }
        else
            free(path);
        free(names.items[i]);
    }
    free(names.items);
    // a .clap directory with no .clap file directly inside is a bundle whose one object carries the packager's name
    // (nebula-cluster.clap/libnebula_cluster.so, #259): listed by that object; two or more objects are named as a
    // failure with the code bundle; none is a directory of other things, walked above
    if (here == 0 && has_suffix(dir, CLAP_SUFFIX))
    {
        char object[PATH_MAX], reason[256];
        const int n = omx_clap_host_bundle_object(dir, object, sizeof(object), reason, sizeof(reason));

        if (n == 1)
            list_add(files, strdup(object));
        else if (n > 1)
        {
            char *failure;
            if (asprintf(&failure, "%s\tbundle\t%s", dir, reason) > 0)
                list_add(failures, failure);
        }
    }
    return 1;
}

/* CLAP_PATH, then the per-user and system directories */
static void default_paths(list_t *paths)
{
    const char *env = getenv("CLAP_PATH");
    const char *home = getenv("HOME");
    char *copy, *save, *dir;

    if (env && *env)
    {
        copy = strdup(env);
        for (dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save))
            list_add(paths, strdup(dir));
        free(copy);
    }
    if (home && *home)
    {
        char *user;
        if (asprintf(&user, "%s/.clap", home) > 0)
            list_add(paths, user);
    }
    list_add(paths, strdup("/usr/lib64/clap"));
    list_add(paths, strdup("/usr/lib/clap"));
}

static int seen_before(const list_t *seen, const char *path)
{
    char resolved[PATH_MAX];
    size_t i;

    if (!realpath(path, resolved))
        return 0;
    for (i = 0; i < seen->count; i++)
        if (strcmp(seen->items[i], resolved) == 0)
            return 1;
    return 0;
}

static void remember(list_t *seen, const char *path)
{
    char resolved[PATH_MAX];

    if (realpath(path, resolved))
        list_add(seen, strdup(resolved));
}

static void usage(const char *name)
{
    fprintf(stderr,
            "usage: %s [--json] [<path>...]\n"
            "       %s --fields clap:<path>#<id> | lv2:<uri> | --list\n"
            "  <path> is a .clap file or a directory searched for .clap files;\n"
            "  none given: CLAP_PATH, then ~/.clap, /usr/lib64/clap and /usr/lib/clap\n"
            "  --fields writes one plugin's field dump, a record a line: <scope> TAB <field> TAB <value>\n",
            name, name);
}


/*
************************************************************************************************************************
*           MAIN
************************************************************************************************************************
*/

int main(int argc, char **argv)
{
    list_t roots = { NULL, 0 }, files = { NULL, 0 }, failures = { NULL, 0 }, seen = { NULL, 0 };
    int explicit_paths, readable = 0, first = 1, a;
    const char *fields = NULL;
    struct stat st;
    size_t i;

    for (a = 1; a < argc; a++)
    {
        if (strcmp(argv[a], "--json") == 0)
            g_json = 1;
        else if (strcmp(argv[a], "--fields") == 0 && a + 1 < argc && !fields)
            fields = argv[++a];
        else if (strcmp(argv[a], "-h") == 0 || strcmp(argv[a], "--help") == 0)
        {
            usage(argv[0]);
            return 0;
        }
        else if (argv[a][0] == '-' && argv[a][1] != '\0')
        {
            usage(argv[0]);
            return 2;
        }
        else
            list_add(&roots, strdup(argv[a]));
    }

    // the core judges a plugin as omx-clap-host does: an instrument's note input admitted, a main port of any width
    // carried, so the JACK names are printed for what that host would register
    {
        struct omx_clap_host_config config;

        omx_clap_host_config_default(&config);
        config.note_inputs = 1;
        config.lane_channels = 0;
        if (omx_clap_host_configure(&config) != 0)
        {
            fprintf(stderr, "can't configure the clap core\n");
            return 2;
        }
    }

    // one plugin's field dump, through the core configured as above (clap:) or the LV2 adapter's world (lv2:)
    if (fields)
    {
        if (g_json || roots.count > 0)
        {
            usage(argv[0]);
            return 2;
        }
        return strcmp(fields, "--list") == 0 ? scan_fields_list() : scan_fields(fields);
    }

    explicit_paths = roots.count > 0;
    if (!explicit_paths)
        default_paths(&roots);

    for (i = 0; i < roots.count; i++)
    {
        // a trailing slash (shell completion's) would hide a bundle directory's .clap suffix
        size_t n = strlen(roots.items[i]);
        while (n > 1 && roots.items[i][n - 1] == '/')
            roots.items[i][--n] = '\0';
        if (stat(roots.items[i], &st) != 0)
        {
            if (explicit_paths)
            {
                char *failure;
                if (asprintf(&failure, "%s\tunreadable\t%s", roots.items[i], strerror(errno)) > 0)
                    list_add(&failures, failure);
            }
            continue;
        }
        if (S_ISDIR(st.st_mode))
        {
            // a directory that could be opened was read, whatever it held: a bundle failure alone is a reading
            if (walk(roots.items[i], &files, &failures))
                readable++;
        }
        else
        {
            list_add(&files, strdup(roots.items[i]));
            readable++;
        }
    }

    if (g_json)
        printf("{\"scanner\":\"omx-clap-scan\",\"version\":\"%s\",\"files\":[", VERSION);
    for (i = 0; i < files.count; i++)
    {
        if (seen_before(&seen, files.items[i]))
            continue;
        remember(&seen, files.items[i]);
        if (g_json)
            fputs(first ? "\n" : ",\n", stdout);
        scan_file(stdout, files.items[i]);
        first = 0;
    }
    for (i = 0; i < failures.count; i++)
    {
        char *code = strchr(failures.items[i], '\t'), *reason;
        *code++ = '\0';
        reason = strchr(code, '\t');
        if (reason)
            *reason++ = '\0';
        else
        {
            // a row of path and reason only: the reading failed
            reason = code;
            code = "unreadable";
        }
        if (g_json)
            fputs(first ? "\n" : ",\n", stdout);
        put_failure(stdout, failures.items[i], code, reason);
        first = 0;
    }
    if (g_json)
        fputs(first ? "]}\n" : "\n]}\n", stdout);

    if (readable == 0)
    {
        fprintf(stderr, "%s: no path could be read\n", argv[0]);
        return 1;
    }
    return 0;
}
