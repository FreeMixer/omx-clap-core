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


#ifdef __cplusplus
}
#endif


/*
************************************************************************************************************************
*           END HEADER
************************************************************************************************************************
*/

#endif
