/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * This file is part of omx-clap-host.
 *
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 */

/* org.openmixer.panel-description/1 as omx_clap_ext.h states it, held to the ABI, omx-plugins' omx_panel_description.h as
 * omx-plugins-devel installs it: the id, every code's value, the size and every member's offset of the five table
 * structures and of the extension, at compile time; then each table's row filled by omx-plugins' own omx_panel_get and
 * read back through this tree's structures, member by member. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <omx-plugins/omx_panel_description.h>

#include "omx_clap_ext.h"

#define SAME(theirs, ours) _Static_assert(sizeof(theirs) == sizeof(ours), "the size of " #ours " is " #theirs "'s")
#define AT(theirs, ours, member) \
    _Static_assert(offsetof(theirs, member) == offsetof(ours, member), #ours "." #member " is where " #theirs " has it")

SAME(omx_clap_panel_section_t, omx_clap_panel_description_section_t);
AT(omx_clap_panel_section_t, omx_clap_panel_description_section_t, key);
AT(omx_clap_panel_section_t, omx_clap_panel_description_section_t, label_key);
AT(omx_clap_panel_section_t, omx_clap_panel_description_section_t, label);

SAME(omx_clap_panel_control_t, omx_clap_panel_description_control_t);
AT(omx_clap_panel_control_t, omx_clap_panel_description_control_t, param_id);
AT(omx_clap_panel_control_t, omx_clap_panel_description_control_t, section);
AT(omx_clap_panel_control_t, omx_clap_panel_description_control_t, widget);
AT(omx_clap_panel_control_t, omx_clap_panel_description_control_t, label_key);
AT(omx_clap_panel_control_t, omx_clap_panel_description_control_t, label);
AT(omx_clap_panel_control_t, omx_clap_panel_description_control_t, visible_param_id);
AT(omx_clap_panel_control_t, omx_clap_panel_description_control_t, visible_mask);

SAME(omx_clap_panel_role_t, omx_clap_panel_description_role_t);
AT(omx_clap_panel_role_t, omx_clap_panel_description_role_t, role);
AT(omx_clap_panel_role_t, omx_clap_panel_description_role_t, index);
AT(omx_clap_panel_role_t, omx_clap_panel_description_role_t, param_id);

SAME(omx_clap_panel_display_t, omx_clap_panel_description_display_t);
AT(omx_clap_panel_display_t, omx_clap_panel_description_display_t, kind);
AT(omx_clap_panel_display_t, omx_clap_panel_description_display_t, section);
AT(omx_clap_panel_display_t, omx_clap_panel_description_display_t, source);
AT(omx_clap_panel_display_t, omx_clap_panel_description_display_t, meter_ids);
AT(omx_clap_panel_display_t, omx_clap_panel_description_display_t, curve_index);
AT(omx_clap_panel_display_t, omx_clap_panel_description_display_t, floor_param_id);
AT(omx_clap_panel_display_t, omx_clap_panel_description_display_t, floor_db);
AT(omx_clap_panel_display_t, omx_clap_panel_description_display_t, label_key);
AT(omx_clap_panel_display_t, omx_clap_panel_description_display_t, label);

SAME(omx_clap_panel_placement_default_t, omx_clap_panel_description_default_t);
AT(omx_clap_panel_placement_default_t, omx_clap_panel_description_default_t, placement);
AT(omx_clap_panel_placement_default_t, omx_clap_panel_description_default_t, param_id);
AT(omx_clap_panel_placement_default_t, omx_clap_panel_description_default_t, value);

SAME(omx_clap_plugin_panel_description_t, omx_clap_plugin_panel_description_ext_t);
AT(omx_clap_plugin_panel_description_t, omx_clap_plugin_panel_description_ext_t, fingerprint);
AT(omx_clap_plugin_panel_description_t, omx_clap_plugin_panel_description_ext_t, family);
AT(omx_clap_plugin_panel_description_t, omx_clap_plugin_panel_description_ext_t, count);
AT(omx_clap_plugin_panel_description_t, omx_clap_plugin_panel_description_ext_t, get);

_Static_assert(OMX_CLAP_PANEL_DESCRIPTION_KEY_SIZE == OMX_CLAP_PANEL_KEY_SIZE, "a key's size");
_Static_assert(OMX_CLAP_PANEL_DESCRIPTION_NAME_SIZE == OMX_CLAP_PANEL_NAME_SIZE, "a label's size");

#define CODE(prefix, code, id) _Static_assert(prefix##id == code, #prefix #id " is " #code);
#define TABLE(code, name, id) CODE(OMX_CLAP_PANEL_, code, id)
#define FAMILY(code, name, id) CODE(OMX_CLAP_FAMILY_, code, id)
#define WIDGET(code, name, id) CODE(OMX_CLAP_WIDGET_, code, id)
#define DISPLAY_KIND(code, name, id) CODE(OMX_CLAP_DISPLAY_, code, id)
#define DISPLAY_SOURCE(code, name, id) CODE(OMX_CLAP_DISPLAY_FROM_, code, id)
#define PLACEMENT(code, name, id) CODE(OMX_CLAP_PLACEMENT_, code, id)
OMX_CLAP_PANEL_TABLES(TABLE)
OMX_CLAP_PANEL_FAMILIES(FAMILY)
OMX_CLAP_PANEL_WIDGETS(WIDGET)
OMX_CLAP_PANEL_DISPLAY_KINDS(DISPLAY_KIND)
OMX_CLAP_PANEL_DISPLAY_SOURCES(DISPLAY_SOURCE)
OMX_CLAP_PANEL_PLACEMENTS(PLACEMENT)

static int g_failures;

#define CHECK(cond, ...) \
    do { \
        if (cond) { printf("ok   " __VA_ARGS__); printf("\n"); } \
        else { printf("FAIL " __VA_ARGS__); printf("\n"); g_failures++; } \
    } while (0)

static const struct omx_panel_section_row g_sections[] = { { "main", "panel.section.main", "Main" } };
static const struct omx_panel_control_row g_controls[] = { { 4, 0, OMX_CLAP_WIDGET_FADER, "panel.gain", "Gain", 3, 0x6 } };
static const struct omx_panel_role_row g_roles[] = { { "threshold", 2, 5 } };
static const struct omx_panel_display_row g_displays[] = {
    { OMX_CLAP_DISPLAY_TRANSFER_CURVE, 0, OMX_CLAP_DISPLAY_FROM_CURVE, { 7, OMX_CLAP_PANEL_INVALID_ID }, 1, 6, -48.0, "panel.curve", "Curve" },
};
static const struct omx_panel_default_row g_defaults[] = { { OMX_CLAP_PLACEMENT_RETURN, 4, -6.5 } };
static const struct omx_panel_rows g_rows = { "f", OMX_CLAP_FAMILY_DYNAMICS, g_sections, 1, g_controls, 1, g_roles, 1, g_displays, 1, g_defaults, 1 };

int main(void)
{
    omx_clap_panel_description_section_t s;
    omx_clap_panel_description_control_t c;
    omx_clap_panel_description_role_t r;
    omx_clap_panel_description_display_t d;
    omx_clap_panel_description_default_t p;

    CHECK(strcmp(OMX_CLAP_EXT_PANEL_DESCRIPTION, OMX_PANEL_DESCRIPTION_ID) == 0, "the extension id is the ABI's: %s", OMX_CLAP_EXT_PANEL_DESCRIPTION);
    CHECK(CLAP_INVALID_ID == OMX_CLAP_PANEL_INVALID_ID, "no parameter is CLAP_INVALID_ID");
    CHECK(omx_panel_get(&g_rows, OMX_CLAP_PANEL_SECTIONS, 0, &s, 0) && !strcmp(s.key, "main") && !strcmp(s.label_key, "panel.section.main")
          && !strcmp(s.label, "Main"), "a section omx_panel_get fills reads back member by member");
    CHECK(omx_panel_get(&g_rows, OMX_CLAP_PANEL_CONTROLS, 0, &c, 0) && c.param_id == 4 && c.section == 0 && c.widget == 2
          && !strcmp(c.label_key, "panel.gain") && !strcmp(c.label, "Gain") && c.visible_param_id == 3 && c.visible_mask == 0x6,
          "a control");
    CHECK(omx_panel_get(&g_rows, OMX_CLAP_PANEL_ROLES, 0, &r, 0) && !strcmp(r.role, "threshold") && r.index == 2 && r.param_id == 5, "a role");
    CHECK(omx_panel_get(&g_rows, OMX_CLAP_PANEL_DISPLAYS, 0, &d, 0) && d.kind == 1 && d.section == 0 && d.source == 3
          && d.meter_ids[0] == 7 && d.meter_ids[1] == CLAP_INVALID_ID && d.curve_index == 1 && d.floor_param_id == 6 && d.floor_db == -48.0
          && !strcmp(d.label_key, "panel.curve") && !strcmp(d.label, "Curve"), "a display");
    CHECK(omx_panel_get(&g_rows, OMX_CLAP_PANEL_PLACEMENT_DEFAULTS, 0, &p, 0) && p.placement == 1 && p.param_id == 4 && p.value == -6.5,
          "a placement default");
    printf("%s\n", g_failures == 0 ? "panel abi test ok" : "panel abi test FAILED");
    return g_failures == 0 ? 0 : 1;
}
