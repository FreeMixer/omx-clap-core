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
 * @brief The OpenMixer CLAP extensions: meters and declaration. */

/*
************************************************************************************************************************
*
* The openmixer vendor extensions of a CLAP plugin, served by get_extension like any CLAP extension: a plugin that
* offers them stays a valid CLAP plugin for a host that never asks. Each is an exact C structure. What CLAP covers is
* read through CLAP: the gain adjustment of a dynamics plugin is clap.gain-adjustment-metering/0, and a plugin that
* reports it under both extensions publishes one number, with CLAP's sign.
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
*           org.openmixer.meters/1
************************************************************************************************************************
*/

/** The meters extension id. */
static const char OMX_CLAP_EXT_METERS[] = "org.openmixer.meters/1";

/** What a meter measures; the unit is implied by the kind, never a free string. */
enum omx_clap_meter_kind
{
    OMX_CLAP_METER_GAIN_REDUCTION_DB = 0,   ///< dB, CLAP's sign: 0 none, negative the reduction applied
    OMX_CLAP_METER_LEVEL_DBFS = 1,          ///< peak level in dBFS, -inf as -HUGE_VALF
};

/** One meter a plugin declares. */
typedef struct omx_clap_meter_info
{
    clap_id id;                 ///< stable across versions, append-only like the parameters
    char name[CLAP_NAME_SIZE];  ///< what a host derives the meter's symbol from
    uint32_t kind;              ///< enum omx_clap_meter_kind
    uint32_t channel_count;     ///< the values read fills: 1 (linked) or 2 (per leg)
} omx_clap_meter_info_t;

/** The org.openmixer.meters/1 extension a plugin offers. */
typedef struct omx_clap_plugin_meters
{
    /** [main-thread] The number of meters the plugin declares. */
    uint32_t(CLAP_ABI *count)(const clap_plugin_t *plugin);

    /** [main-thread] Fills `info` with the meter at `index`; false for an index out of range. */
    bool(CLAP_ABI *get_info)(const clap_plugin_t *plugin, uint32_t index, omx_clap_meter_info_t *info);

    /** [thread-safe] The values the audio thread published at the end of the last process(), relaxed atomics: fills
     * min(capacity, channel_count) floats, false for an unknown id or a plugin that has not processed since activate. */
    bool(CLAP_ABI *read)(const clap_plugin_t *plugin, clap_id id, float *values, uint32_t capacity);
} omx_clap_plugin_meters_t;


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


#ifdef __cplusplus
}
#endif


/*
************************************************************************************************************************
*           END HEADER
************************************************************************************************************************
*/

#endif
