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

/* The hints of omx-contract's RESOURCE_HINTS by index, the one table the core's CLAP face and the LV2 adapter's reading
 * of the omxres properties share: bit 1u << index of `declared`, the hint's word and the word of its unit, as
 * omx_vocabulary.h spells them, and its member of omx_resource_hints_t. omx_resource_hints.h lays the bits and the
 * members out in the order of RESOURCE_HINTS. Internal and header-only: never installed. */

#ifndef RESOURCE_HINTS_H
#define RESOURCE_HINTS_H

#include <stdbool.h>
#include <stdint.h>

#include <omxcontract/omx_resource_hints.h>
#include <omxcontract/omx_vocabulary.h>

/** Every bit RESOURCE_HINTS has a hint for. */
#define RESOURCE_HINTS_ALL      ((1u << OMX_RESOURCE_HINT_COUNT) - 1u)

static const struct resource_hint
{
    uint32_t bit;
    const char *word;
    const char *unit;
} resource_hints_table[] = {
    { OMX_RESOURCE_HINT_FOOTPRINT_AT_REST, OMX_RESOURCE_HINTS_FOOTPRINT_AT_REST, OMX_RESOURCE_UNITS_BYTES },
    { OMX_RESOURCE_HINT_FOOTPRINT_PER_VOICE, OMX_RESOURCE_HINTS_FOOTPRINT_PER_VOICE, OMX_RESOURCE_UNITS_BYTES },
    { OMX_RESOURCE_HINT_SHARED_LOAD, OMX_RESOURCE_HINTS_SHARED_LOAD, OMX_RESOURCE_UNITS_BYTES },
    { OMX_RESOURCE_HINT_THREADS_SPAWNED, OMX_RESOURCE_HINTS_THREADS_SPAWNED, OMX_RESOURCE_UNITS_PLAIN_COUNT },
    { OMX_RESOURCE_HINT_STREAMS_FROM_DISK, OMX_RESOURCE_HINTS_STREAMS_FROM_DISK, OMX_RESOURCE_UNITS_FLAG },
    { OMX_RESOURCE_HINT_OPENS_SOCKETS, OMX_RESOURCE_HINTS_OPENS_SOCKETS, OMX_RESOURCE_UNITS_FLAG },
    { OMX_RESOURCE_HINT_ACTIVATE_TIME, OMX_RESOURCE_HINTS_ACTIVATE_TIME, OMX_RESOURCE_UNITS_MS },
};
_Static_assert(sizeof(resource_hints_table) / sizeof(resource_hints_table[0]) == OMX_RESOURCE_HINT_COUNT &&
                   OMX_RESOURCE_HINT_COUNT == OMX_RESOURCE_HINTS_COUNT,
               "one row per hint of RESOURCE_HINTS");

/* the largest figure hint `index`'s member holds: a uint64_t for bytes, a uint32_t for a count or a time, 1 for a flag */
static inline uint64_t resource_hint_max(uint32_t index)
{
    switch (resource_hints_table[index].bit)
    {
    case OMX_RESOURCE_HINT_THREADS_SPAWNED:
    case OMX_RESOURCE_HINT_ACTIVATE_TIME:
        return UINT32_MAX;
    case OMX_RESOURCE_HINT_STREAMS_FROM_DISK:
    case OMX_RESOURCE_HINT_OPENS_SOCKETS:
        return 1u;
    default:
        return UINT64_MAX;
    }
}

/* hint `index`'s member as a figure, a flag 0 or 1 */
static inline uint64_t resource_hint_get(const omx_resource_hints_t *h, uint32_t index)
{
    switch (resource_hints_table[index].bit)
    {
    case OMX_RESOURCE_HINT_FOOTPRINT_AT_REST: return h->footprint_at_rest;
    case OMX_RESOURCE_HINT_FOOTPRINT_PER_VOICE: return h->footprint_per_voice;
    case OMX_RESOURCE_HINT_SHARED_LOAD: return h->shared_load;
    case OMX_RESOURCE_HINT_THREADS_SPAWNED: return h->threads_spawned;
    case OMX_RESOURCE_HINT_STREAMS_FROM_DISK: return h->streams_from_disk ? 1u : 0u;
    case OMX_RESOURCE_HINT_OPENS_SOCKETS: return h->opens_sockets ? 1u : 0u;
    default: return h->activate_time;
    }
}

/* hint `index`'s member set to `figure`, at most resource_hint_max(index) */
static inline void resource_hint_set(omx_resource_hints_t *h, uint32_t index, uint64_t figure)
{
    switch (resource_hints_table[index].bit)
    {
    case OMX_RESOURCE_HINT_FOOTPRINT_AT_REST: h->footprint_at_rest = figure; break;
    case OMX_RESOURCE_HINT_FOOTPRINT_PER_VOICE: h->footprint_per_voice = figure; break;
    case OMX_RESOURCE_HINT_SHARED_LOAD: h->shared_load = figure; break;
    case OMX_RESOURCE_HINT_THREADS_SPAWNED: h->threads_spawned = (uint32_t)figure; break;
    case OMX_RESOURCE_HINT_STREAMS_FROM_DISK: h->streams_from_disk = figure != 0; break;
    case OMX_RESOURCE_HINT_OPENS_SOCKETS: h->opens_sockets = figure != 0; break;
    default: h->activate_time = (uint32_t)figure; break;
    }
}

#endif
