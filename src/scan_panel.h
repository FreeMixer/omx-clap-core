/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * This file is part of omx-clap-host.
 *
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 */

#ifndef SCAN_PANEL_H
#define SCAN_PANEL_H

#include <stdbool.h>
#include <stdint.h>

#include <clap/plugin.h>

#include "omx_clap_ext.h"

/* The tables of org.openmixer.panel-description/1, SCAN_PANEL_SECTIONS and on, by their OMX_CLAP_PANEL_TABLES code. */
#define SCAN_PANEL_TABLE_CODE(code, name, id) SCAN_PANEL_##id = code,
enum scan_panel_table
{
    OMX_CLAP_PANEL_TABLES(SCAN_PANEL_TABLE_CODE)
    SCAN_PANEL_TABLE_COUNT
};
#undef SCAN_PANEL_TABLE_CODE

/* One table as the plugin answered it: the entries its get filled, each with its index in the table. An entry the get
 * refuses is left out; a table the plugin has no count or get for is empty. */
struct scan_panel_rows
{
    void *rows;  ///< `n` structures of the table's type
    uint32_t *index;  ///< each row's index in the plugin's table
    uint32_t n;
};

/* The panel description a plugin publishes, as omx-clap-scan prints it: nothing is filled in that the plugin did not
 * answer. */
struct scan_panel
{
    bool has_fingerprint;
    char fingerprint[257];  ///< as the plugin gives it, up to 256 bytes
    bool has_family;
    uint32_t family;  ///< an OMX_CLAP_PANEL_FAMILIES code, as the plugin answers it
    struct scan_panel_rows table[SCAN_PANEL_TABLE_COUNT];
};

/* Reads the plugin's org.openmixer.panel-description/1 into `*panel`, on the main thread: 1 read (free it with
 * scan_panel_free), 0 the plugin does not serve it, -1 out of memory. */
int scan_panel_read(const clap_plugin_t *plugin, struct scan_panel *panel);

void scan_panel_free(struct scan_panel *panel);

/* The rows of a table, typed. */
#define SCAN_PANEL_ROW(panel, t, type, i) (&((const type *)(panel)->table[t].rows)[i])

/* A code's word (omx-plugins' name for it, "fader") and enumerator (as omx_panel_description.h names it,
 * "OMX_CLAP_WIDGET_FADER"), NULL for a code the extension does not name. */
const char *scan_panel_family_word(uint32_t code);
const char *scan_panel_family_enum(uint32_t code);
const char *scan_panel_widget_word(uint32_t code);
const char *scan_panel_widget_enum(uint32_t code);
const char *scan_panel_display_kind_word(uint32_t code);
const char *scan_panel_display_kind_enum(uint32_t code);
const char *scan_panel_display_source_word(uint32_t code);
const char *scan_panel_display_source_enum(uint32_t code);
const char *scan_panel_placement_word(uint32_t code);
const char *scan_panel_placement_enum(uint32_t code);

#endif
