/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * This file is part of omx-clap-host.
 *
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 */

/* The panel description a plugin publishes (org.openmixer.panel-description/1), read once for omx-clap-scan's listing
 * and its field dump: the family and the fingerprint as the plugin answers them, and each of the five tables walked by
 * count and get in its published order. Nothing is filled in: a table the plugin cannot count is empty, an entry its get
 * refuses is left out, and every text is cut at its field's end. A table counted past
 * OMX_CLAP_PANEL_DESCRIPTION_MAX_ENTRIES is refused whole and the other tables are still read. */

#include <stdlib.h>
#include <string.h>

#include "scan_panel.h"

/* the size of each table's structure */
static const size_t g_row_size[SCAN_PANEL_TABLE_COUNT] = {
    [SCAN_PANEL_SECTIONS] = sizeof(omx_clap_panel_description_section_t),
    [SCAN_PANEL_CONTROLS] = sizeof(omx_clap_panel_description_control_t),
    [SCAN_PANEL_ROLES] = sizeof(omx_clap_panel_description_role_t),
    [SCAN_PANEL_DISPLAYS] = sizeof(omx_clap_panel_description_display_t),
    [SCAN_PANEL_PLACEMENT_DEFAULTS] = sizeof(omx_clap_panel_description_default_t),
};

#define TERMINATE(field) ((field)[sizeof(field) - 1] = '\0')

/* the texts of a row the plugin filled, each ended within its field */
static void terminate(uint32_t table, void *row)
{
    switch (table)
    {
    case SCAN_PANEL_SECTIONS:
    {
        omx_clap_panel_description_section_t *s = row;
        TERMINATE(s->key);
        TERMINATE(s->label_key);
        TERMINATE(s->label);
        break;
    }
    case SCAN_PANEL_CONTROLS:
    {
        omx_clap_panel_description_control_t *c = row;
        TERMINATE(c->label_key);
        TERMINATE(c->label);
        break;
    }
    case SCAN_PANEL_ROLES:
        TERMINATE(((omx_clap_panel_description_role_t *)row)->role);
        break;
    case SCAN_PANEL_DISPLAYS:
    {
        omx_clap_panel_description_display_t *d = row;
        TERMINATE(d->label_key);
        TERMINATE(d->label);
        break;
    }
    default:
        break;
    }
}

/* the ids a structure holds when the plugin leaves them unset: none */
static void blank(uint32_t table, void *row)
{
    memset(row, 0, g_row_size[table]);
    if (table == SCAN_PANEL_CONTROLS)
        ((omx_clap_panel_description_control_t *)row)->visible_param_id = CLAP_INVALID_ID;
    else if (table == SCAN_PANEL_DISPLAYS)
    {
        omx_clap_panel_description_display_t *d = row;
        d->meter_ids[0] = d->meter_ids[1] = CLAP_INVALID_ID;
        d->floor_param_id = CLAP_INVALID_ID;
    }
}

static int read_table(const clap_plugin_t *plugin, const omx_clap_plugin_panel_description_ext_t *ext, uint32_t table,
                      struct scan_panel_rows *out)
{
    const uint32_t count = ext->count && ext->get ? ext->count(plugin, table) : 0;
    const size_t size = g_row_size[table];
    char *rows;
    uint32_t i;

    if (count == 0)
        return 0;
    // the count is the plugin's: past the maximum the table is refused before anything is reserved for it
    if (count > OMX_CLAP_PANEL_DESCRIPTION_MAX_ENTRIES)
    {
        out->refused = count;
        return 0;
    }
    rows = malloc((size_t)count * size);
    out->index = malloc((size_t)count * sizeof(uint32_t));
    if (!rows || !out->index)
    {
        free(rows);
        return -1;
    }
    out->rows = rows;
    for (i = 0; i < count; i++)
    {
        char *row = rows + (size_t)out->n * size;

        blank(table, row);
        if (!ext->get(plugin, table, i, row))
            continue;
        terminate(table, row);
        out->index[out->n++] = i;
    }
    return 0;
}

int scan_panel_read(const clap_plugin_t *plugin, struct scan_panel *panel)
{
    const omx_clap_plugin_panel_description_ext_t *ext = plugin->get_extension(plugin, OMX_CLAP_EXT_PANEL_DESCRIPTION);
    const char *fingerprint;
    uint32_t t;

    memset(panel, 0, sizeof(*panel));
    if (!ext)
        return 0;
    fingerprint = ext->fingerprint ? ext->fingerprint(plugin) : NULL;
    if (fingerprint)
    {
        panel->has_fingerprint = true;
        strncpy(panel->fingerprint, fingerprint, sizeof(panel->fingerprint) - 1);
    }
    if (ext->family)
    {
        panel->has_family = true;
        panel->family = ext->family(plugin);
    }
    for (t = 0; t < SCAN_PANEL_TABLE_COUNT; t++)
        if (read_table(plugin, ext, t, &panel->table[t]) != 0)
        {
            scan_panel_free(panel);
            return -1;
        }
    return 1;
}

void scan_panel_free(struct scan_panel *panel)
{
    uint32_t t;

    for (t = 0; t < SCAN_PANEL_TABLE_COUNT; t++)
    {
        free(panel->table[t].rows);
        free(panel->table[t].index);
    }
    memset(panel, 0, sizeof(*panel));
}

/* each code's word and enumerator, from omx_clap_ext.h's lists */
#define WORD(code, name, id) [code] = name,
#define WORDS(list, fn)                                                                                             \
    static const char *const g_##fn##_words[] = { list(WORD) };                                                     \
    const char *scan_panel_##fn##_word(uint32_t code)                                                               \
    {                                                                                                               \
        return code < sizeof(g_##fn##_words) / sizeof(g_##fn##_words[0]) ? g_##fn##_words[code] : NULL;            \
    }

WORDS(OMX_CLAP_PANEL_FAMILIES, family)
WORDS(OMX_CLAP_PANEL_WIDGETS, widget)
WORDS(OMX_CLAP_PANEL_DISPLAY_KINDS, display_kind)
WORDS(OMX_CLAP_PANEL_DISPLAY_SOURCES, display_source)
WORDS(OMX_CLAP_PANEL_PLACEMENTS, placement)

#define FAMILY_ENUM(code, name, id) [code] = "OMX_CLAP_FAMILY_" #id,
#define WIDGET_ENUM(code, name, id) [code] = "OMX_CLAP_WIDGET_" #id,
#define DISPLAY_KIND_ENUM(code, name, id) [code] = "OMX_CLAP_DISPLAY_" #id,
#define DISPLAY_SOURCE_ENUM(code, name, id) [code] = "OMX_CLAP_DISPLAY_FROM_" #id,
#define PLACEMENT_ENUM(code, name, id) [code] = "OMX_CLAP_PLACEMENT_" #id,
#define ENUMS(list, x, fn)                                                                                          \
    static const char *const g_##fn##_enums[] = { list(x) };                                                        \
    const char *scan_panel_##fn##_enum(uint32_t code)                                                               \
    {                                                                                                               \
        return code < sizeof(g_##fn##_enums) / sizeof(g_##fn##_enums[0]) ? g_##fn##_enums[code] : NULL;            \
    }

ENUMS(OMX_CLAP_PANEL_FAMILIES, FAMILY_ENUM, family)
ENUMS(OMX_CLAP_PANEL_WIDGETS, WIDGET_ENUM, widget)
ENUMS(OMX_CLAP_PANEL_DISPLAY_KINDS, DISPLAY_KIND_ENUM, display_kind)
ENUMS(OMX_CLAP_PANEL_DISPLAY_SOURCES, DISPLAY_SOURCE_ENUM, display_source)
ENUMS(OMX_CLAP_PANEL_PLACEMENTS, PLACEMENT_ENUM, placement)
