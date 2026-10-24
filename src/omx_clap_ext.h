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

/** @file
 * @brief The OpenMixer CLAP extension: declaration. */

/*
************************************************************************************************************************
*
* The openmixer vendor extension of a CLAP plugin, served by get_extension like any CLAP extension: a plugin that
* offers it stays a valid CLAP plugin for a host that never asks. It is an exact C structure. What CLAP covers is read
* through CLAP: a plugin's meters are clap.meters/1 (docs/meters.md) and the gain adjustment of a dynamics plugin is
* clap.gain-adjustment-metering/0.
*
* Header only: nothing here is exported by libomx-clap-core, which declares it for the plugins and hosts that use it.
*
************************************************************************************************************************
*/

#ifndef OMX_CLAP_EXT_H
#define OMX_CLAP_EXT_H


/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <stdbool.h>
#include <stdint.h>

#include <clap/plugin.h>
#include <clap/string-sizes.h>

#ifdef __cplusplus
extern "C" {
#endif


/*
************************************************************************************************************************
*           org.openmixer.declaration/1
************************************************************************************************************************
*/

/** The declaration extension id. */
static const char OMX_CLAP_EXT_DECLARATION[] = "org.openmixer.declaration/1";

/** The org.openmixer.declaration/1 extension a plugin offers. */
typedef struct omx_clap_plugin_declaration
{
    /** [thread-safe] The expression of the openmixer core the parameters were generated from. */
    const char *(CLAP_ABI *source)(const clap_plugin_t *plugin);

    /** [thread-safe] Lowercase hex SHA-256 of the resolved parameter list (symbol, unit, min, max, default, flags, in id
     * order): a host compares it with the one it runs and refuses a plugin whose parameters are stale. */
    const char *(CLAP_ABI *digest)(const clap_plugin_t *plugin);
} omx_clap_plugin_declaration_t;


/*
************************************************************************************************************************
*           org.openmixer.param-contract/1
************************************************************************************************************************
*/

/*
 * What a plugin says about each parameter beyond clap_param_info: a symbol stable across versions, the unit of its plain
 * value, how the value maps to a control's travel and the quantum the plugin resolves, then the contract's version and
 * its migration steps. omx-plugins' faces serve it from their include/omx_param_contract.h, whose structures have this
 * layout under other names, so a face may include both.
 *
 * The canonical serialisation, whose lowercase hex SHA-256 is the fingerprint: UTF-8, an LF after every line; the
 * extension id on the first line, then one line per parameter by ascending id, TAB-separated:
 *   id symbol unit scale min max default step flags
 * id in decimal; unit and scale by name; min, max and default as get_info gives them and step as the contract does, each
 * the shortest decimal that reads back to the same double, in ECMAScript's Number.prototype.toString form, -0 written 0;
 * flags in lowercase hex, no prefix, of get_info's flags & (IS_STEPPED | IS_PERIODIC | IS_READONLY | IS_BYPASS |
 * IS_ENUM). The name and the module are display text, not serialised. The plugin's own bypass is a parameter like any
 * other: symbol bypass, unit none, scale stepped.
 *
 * A host trusts a contract whole or not at all: every id clap.params lists has exactly one entry and no two entries share
 * a symbol (R1); symbols, units, scales, bounds and steps are well formed (R2); the contract is the same after
 * params.rescan (R3); the fingerprint the host computes is the one stated (R4); the migration steps are well formed
 * (R5). libomx-clap-core's omx_clap_host_param_contract says which rule a plugin broke.
 */

/** The param-contract extension id. */
static const char OMX_CLAP_EXT_PARAM_CONTRACT[] = "org.openmixer.param-contract/1";

/** A contract symbol: [A-Za-z_][A-Za-z0-9_]*, at most 63 bytes and the NUL. */
#define OMX_CLAP_PARAM_CONTRACT_SYMBOL_SIZE 64

/** The units, X(code, name): 1..24 are LV2's units one to one, named by their local name in units.ttl; 0x100 on is the
 * vendor block, append only. The name is the word the canonical serialisation and plugin-hostd's param_info write. */
#define OMX_CLAP_PARAM_UNITS(X) \
    X(0, "none") X(1, "s") X(2, "ms") X(3, "min") X(4, "bar") X(5, "beat") X(6, "frame") X(7, "m") X(8, "cm") \
    X(9, "mm") X(10, "km") X(11, "inch") X(12, "mile") X(13, "db") X(14, "pc") X(15, "coef") X(16, "hz") X(17, "khz") \
    X(18, "mhz") X(19, "bpm") X(20, "oct") X(21, "cent") X(22, "semitone12TET") X(23, "degree") X(24, "midiNote") \
    X(0x100, "dbfs") X(0x101, "dbu") X(0x102, "ratio") X(0x103, "lufs")

/** The scales, X(code, name): linear (the value is linear in position), log (geometric in position, both bounds
 * positive), stepped (the whole numbers from min to max, CLAP_PARAM_IS_STEPPED, step 1). */
#define OMX_CLAP_PARAM_SCALES(X) X(0, "linear") X(1, "log") X(2, "stepped")

/** One parameter's contract, as the plugin's get fills it. */
typedef struct omx_clap_param_contract_entry
{
    clap_id param_id;  ///< the params id this entry describes
    char symbol[OMX_CLAP_PARAM_CONTRACT_SYMBOL_SIZE];  ///< stable across versions; survives id and name changes
    uint32_t unit;  ///< an OMX_CLAP_PARAM_UNITS code
    uint32_t scale;  ///< an OMX_CLAP_PARAM_SCALES code
    double step;  ///< the plain-unit quantum the plugin resolves: 0 continuous, 1 for a stepped scale
} omx_clap_param_contract_entry_t;

/** One migration step: contract version from_version to from_version + 1, one symbol. With no value map the new value
 * is factor * convert(from_unit, to_unit, v) + offset; with one, an enum reorder of (old, new) pairs. An empty
 * to_symbol is a removal. */
typedef struct omx_clap_param_contract_step
{
    uint32_t from_version;  ///< the version this step leaves
    char from_symbol[OMX_CLAP_PARAM_CONTRACT_SYMBOL_SIZE];  ///< the symbol at from_version
    char to_symbol[OMX_CLAP_PARAM_CONTRACT_SYMBOL_SIZE];  ///< the symbol at from_version + 1, "" when removed
    uint32_t from_unit;  ///< the unit at from_version
    uint32_t to_unit;  ///< the unit at from_version + 1
    double factor;  ///< 1 when not rescaled
    double offset;  ///< 0 when not rescaled
    uint32_t value_map_count;  ///< the (old, new) pairs in value_map, 0 unless an enum reorder
    const double *value_map;  ///< the pairs, static storage; NULL when the count is 0
} omx_clap_param_contract_step_t;

/** The org.openmixer.param-contract/1 extension a plugin offers. */
typedef struct omx_clap_plugin_param_contract_ext
{
    /** [thread-safe] The contract version: 1 at first publication, + 1 per contract change. */
    uint32_t (CLAP_ABI *version)(const clap_plugin_t *plugin);

    /** [thread-safe] Lowercase hex SHA-256 of the canonical serialisation, 64 characters and the NUL. */
    const char *(CLAP_ABI *fingerprint)(const clap_plugin_t *plugin);

    /** [main-thread] Fills `*entry` for a parameter id clap.params lists; false for an id it does not know. */
    bool (CLAP_ABI *get)(const clap_plugin_t *plugin, clap_id param_id, omx_clap_param_contract_entry_t *entry);

    /** [main-thread] The migration steps of versions 1 to version() - 1. */
    uint32_t (CLAP_ABI *migration_count)(const clap_plugin_t *plugin);

    /** [main-thread] Fills `*step`, by from_version ascending; false past the end. */
    bool (CLAP_ABI *get_migration)(const clap_plugin_t *plugin, uint32_t index, omx_clap_param_contract_step_t *step);
} omx_clap_plugin_param_contract_ext_t;


/*
************************************************************************************************************************
*           org.openmixer.panel-description/1
************************************************************************************************************************
*/

/*
 * The panel a plugin publishes: its family, its sections in order, each control's parameter, section, widget, label and
 * visibility rule, the roles its parameters play in the family, the displays drawn over a section and the per-placement
 * defaults. Units, travels, scales and enumeration labels are the parameters' own and are not restated. omx-plugins'
 * faces serve it from their include/omx_panel_description.h, which is the ABI: the structures below have its layout
 * under other names, so a face may include both, and tests/panel_abi_test.c holds them to it, size and offset. The
 * fingerprint is the lowercase hex SHA-256 of the canonical serialisation omx-plugins' tools/panel-description.mjs writes.
 *
 * The host walks five tables, each by count and get, in their published order.
 */

/** The panel-description extension id. */
static const char OMX_CLAP_EXT_PANEL_DESCRIPTION[] = "org.openmixer.panel-description/1";

/** A key (a section key, a label key, a role): [a-z][a-z0-9_]*(\.[a-z0-9_]+)*, at most 63 bytes and the NUL. */
#define OMX_CLAP_PANEL_DESCRIPTION_KEY_SIZE 64
/** A fallback label, CLAP_NAME_SIZE. */
#define OMX_CLAP_PANEL_DESCRIPTION_NAME_SIZE CLAP_NAME_SIZE

/*
 * The codes, X(code, name, enumerator): the name is omx-plugins' word for the code (tools/panel-description.mjs), the
 * enumerator the one omx_panel_description.h gives it after its prefix (OMX_CLAP_PANEL_, OMX_CLAP_FAMILY_,
 * OMX_CLAP_WIDGET_, OMX_CLAP_DISPLAY_, OMX_CLAP_DISPLAY_FROM_, OMX_CLAP_PLACEMENT_).
 */

/** The tables, in the order a host walks them. */
#define OMX_CLAP_PANEL_TABLES(X) \
    X(0, "sections", SECTIONS) X(1, "controls", CONTROLS) X(2, "roles", ROLES) X(3, "displays", DISPLAYS) \
    X(4, "placement_defaults", PLACEMENT_DEFAULTS)

/** The families: which house view can draw the plugin and which roles it may bind. Append only. */
#define OMX_CLAP_PANEL_FAMILIES(X) \
    X(0, "none", NONE) X(1, "gate", GATE) X(2, "dynamics", DYNAMICS) X(3, "limiter", LIMITER) X(4, "deesser", DEESSER) \
    X(5, "eq", EQ) X(6, "filter", FILTER) X(7, "drive", DRIVE) X(8, "modulation", MODULATION) X(9, "delay", DELAY) \
    X(10, "reverb", REVERB) X(11, "pitch", PITCH)

/** The widgets: auto leaves the widget to the host, from the parameter. */
#define OMX_CLAP_PANEL_WIDGETS(X) \
    X(0, "auto", AUTO) X(1, "knob", KNOB) X(2, "fader", FADER) X(3, "switch", SWITCH) X(4, "selector", SELECTOR)

/** The displays. Append only. */
#define OMX_CLAP_PANEL_DISPLAY_KINDS(X) \
    X(0, "gain_reduction_bar", GAIN_REDUCTION_BAR) X(1, "transfer_curve", TRANSFER_CURVE) X(2, "eq_response", EQ_RESPONSE) \
    X(3, "meter", METER)

/** Where a display's data comes from. */
#define OMX_CLAP_PANEL_DISPLAY_SOURCES(X) \
    X(0, "family", FAMILY) X(1, "meters", METERS) X(2, "gain_adjustment", GAIN_ADJUSTMENT) X(3, "curve", CURVE)

/** Where an instance sits: the clap.track-info/1 flags read as one value. */
#define OMX_CLAP_PANEL_PLACEMENTS(X) X(0, "insert", INSERT) X(1, "return", RETURN) X(2, "bus", BUS) X(3, "master", MASTER)

/** A section, in the order the panel draws them. */
typedef struct omx_clap_panel_description_section
{
    char key[OMX_CLAP_PANEL_DESCRIPTION_KEY_SIZE];  ///< the section's key
    char label_key[OMX_CLAP_PANEL_DESCRIPTION_KEY_SIZE];  ///< the key its label is translated by
    char label[OMX_CLAP_PANEL_DESCRIPTION_NAME_SIZE];  ///< the fallback text, not fingerprinted
} omx_clap_panel_description_section_t;

/** A control: one parameter drawn in one section. */
typedef struct omx_clap_panel_description_control
{
    clap_id param_id;  ///< the params id it draws
    uint32_t section;  ///< an index into the sections
    uint32_t widget;  ///< an OMX_CLAP_PANEL_WIDGETS code
    char label_key[OMX_CLAP_PANEL_DESCRIPTION_KEY_SIZE];  ///< the key its label is translated by
    char label[OMX_CLAP_PANEL_DESCRIPTION_NAME_SIZE];  ///< the fallback text
    clap_id visible_param_id;  ///< the parameter its visibility follows, CLAP_INVALID_ID when always visible
    uint64_t visible_mask;  ///< bit v set: shown while visible_param_id's value is v
} omx_clap_panel_description_control_t;

/** A role a parameter plays in the family. */
typedef struct omx_clap_panel_description_role
{
    char role[OMX_CLAP_PANEL_DESCRIPTION_KEY_SIZE];  ///< the role's name
    uint32_t index;  ///< 0 unless the role repeats (an EQ band)
    clap_id param_id;  ///< the parameter that plays it
} omx_clap_panel_description_role_t;

/** A display drawn over a section. */
typedef struct omx_clap_panel_description_display
{
    uint32_t kind;  ///< an OMX_CLAP_PANEL_DISPLAY_KINDS code
    uint32_t section;  ///< an index into the sections
    uint32_t source;  ///< an OMX_CLAP_PANEL_DISPLAY_SOURCES code
    uint32_t meter_ids[2];  ///< the clap.meters/1 ids it reads, CLAP_INVALID_ID for none
    uint32_t curve_index;  ///< the clap.mini-curve-display/3 curve it reads
    clap_id floor_param_id;  ///< the parameter that sets its floor, CLAP_INVALID_ID for none
    double floor_db;  ///< its floor when no parameter sets it
    char label_key[OMX_CLAP_PANEL_DESCRIPTION_KEY_SIZE];  ///< the key its label is translated by
    char label[OMX_CLAP_PANEL_DESCRIPTION_NAME_SIZE];  ///< the fallback text
} omx_clap_panel_description_display_t;

/** A parameter's come-up value in a placement other than insert. */
typedef struct omx_clap_panel_description_default
{
    uint32_t placement;  ///< an OMX_CLAP_PANEL_PLACEMENTS code, never insert
    clap_id param_id;  ///< the parameter
    double value;  ///< its plain value, in the parameter's unit
} omx_clap_panel_description_default_t;

/** The org.openmixer.panel-description/1 extension a plugin offers. */
typedef struct omx_clap_plugin_panel_description_ext
{
    /** [thread-safe] Lowercase hex SHA-256 of the canonical serialisation, 64 characters and the NUL. */
    const char *(CLAP_ABI *fingerprint)(const clap_plugin_t *plugin);

    /** [main-thread] An OMX_CLAP_PANEL_FAMILIES code. */
    uint32_t (CLAP_ABI *family)(const clap_plugin_t *plugin);

    /** [main-thread] The entries of one table (an OMX_CLAP_PANEL_TABLES code); 0 for an unknown table. */
    uint32_t (CLAP_ABI *count)(const clap_plugin_t *plugin, uint32_t table);

    /** [main-thread] Fills `*out`, the structure of that table, for index < count; false past the end or for an unknown
     * table. */
    bool (CLAP_ABI *get)(const clap_plugin_t *plugin, uint32_t table, uint32_t index, void *out);
} omx_clap_plugin_panel_description_ext_t;


#ifdef __cplusplus
}
#endif


/*
************************************************************************************************************************
*           END HEADER
************************************************************************************************************************
*/

#endif
