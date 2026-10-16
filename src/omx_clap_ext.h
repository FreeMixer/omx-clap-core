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


#ifdef __cplusplus
}
#endif


/*
************************************************************************************************************************
*           END HEADER
************************************************************************************************************************
*/

#endif
