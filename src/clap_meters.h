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
 * @brief clap.meters/1, the CLAP meters extension, for an SDK that does not ship it yet. */

/*
************************************************************************************************************************
*
* The CLAP SDK's clap/ext/draft/meters.h when the SDK has it; otherwise its id and structures, declared exactly as the
* draft declares them (free-audio/clap#521), so a host and a plugin built against either agree on the layout.
* docs/meters.md is why the host reads this extension and no other for meters.
*
************************************************************************************************************************
*/

#ifndef OMX_CLAP_METERS_H
#define OMX_CLAP_METERS_H

#if defined(__has_include)
#if __has_include(<clap/ext/draft/meters.h>)
#include <clap/ext/draft/meters.h>
#define OMX_CLAP_METERS_FROM_SDK 1
#endif
#endif

#ifndef OMX_CLAP_METERS_FROM_SDK

#include <stdbool.h>
#include <stdint.h>

#include <clap/plugin.h>
#include <clap/string-sizes.h>

#ifdef __cplusplus
extern "C" {
#endif

static CLAP_CONSTEXPR const char CLAP_EXT_METERS[] = "clap.meters/1";

/** What a meter measures. */
enum clap_meter_kind
{
    CLAP_METER_KIND_OTHER = 0,              ///< no unit: the host relies on the display range alone
    CLAP_METER_KIND_GAIN_REDUCTION = 1,     ///< dB, the sign of clap.gain-adjustment-metering: 0 none, negative a reduction
    CLAP_METER_KIND_LEVEL = 2,              ///< dBFS, silence -infinity or any value below min_value
};

enum clap_meter_flags
{
    CLAP_METER_IS_COHERENT = 1 << 0,        ///< every value of one get_values() comes from the same processed block
};

/** One meter a plugin declares. */
typedef struct clap_meter_info
{
    clap_id id;                 ///< stable, it never changes
    char name[CLAP_NAME_SIZE];  ///< displayable name
    uint32_t kind;              ///< enum clap_meter_kind
    uint32_t flags;             ///< bitmask of enum clap_meter_flags
    uint32_t channel_count;     ///< the values the meter has: 1 for a linked gain reduction, 2 for a stereo level
    double min_value;           ///< the display range the plugin suggests, in the meter's unit
    double max_value;
} clap_meter_info_t;

/** The clap.meters/1 extension a plugin offers. */
typedef struct clap_plugin_meters
{
    /** [main-thread] The number of meters. */
    uint32_t(CLAP_ABI *count)(const clap_plugin_t *plugin);

    /** [main-thread] Copies the meter at `index` to `meter_info`; true on success. */
    bool(CLAP_ABI *get_info)(const clap_plugin_t *plugin, uint32_t index, clap_meter_info_t *meter_info);

    /** [thread-safe] Copies the latest values of `meter_id`, one per channel, up to `capacity`, and returns how many it
     * wrote: 0 for an unknown id, or a plugin that is not active or has not processed since it was activated.
     * Lock-free and wait-free. */
    uint32_t(CLAP_ABI *get_values)(const clap_plugin_t *plugin, clap_id meter_id, double *values, uint32_t capacity);
} clap_plugin_meters_t;

/** The host side of clap.meters/1. */
typedef struct clap_host_meters
{
    /** [main-thread & !active] The list of meters or their info changed. */
    void(CLAP_ABI *changed)(const clap_host_t *host);
} clap_host_meters_t;

#ifdef __cplusplus
}
#endif

#endif /* OMX_CLAP_METERS_FROM_SDK */

#endif /* OMX_CLAP_METERS_H */
