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

/*
************************************************************************************************************************
*
* The configuration and the feature providers of the LV2 host (see lv2_core.h), moved from the console's
* mix_host_backend.c: one URID table per process, lock-free for a URI already mapped, and which providers are on,
* which is the configured list's say. The map, the log, the options and the worker schedule an instance is handed are
* its own structures over that table (lv2_instance.c), so a call names the instance it came from.
*
************************************************************************************************************************
*/


/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <lv2/buf-size/buf-size.h>
#include <lv2/options/options.h>
#include <lv2/state/state.h>
#include <lv2/worker/worker.h>

#include "lv2_core_internal.h"


/*
************************************************************************************************************************
*           GLOBAL VARIABLES
************************************************************************************************************************
*/

const char *const lv2_feature_uri[LV2_FEATURE_KINDS] =
{
    [F_MAP] = LV2_URID__map,
    [F_UNMAP] = LV2_URID__unmap,
    [F_OPTIONS] = LV2_OPTIONS__options,
    [F_BOUNDED] = LV2_BUF_SIZE__boundedBlockLength,
    [F_WORKER] = LV2_WORKER__schedule,
    [F_DEFAULT_STATE] = LV2_STATE__loadDefaultState,
    [F_LOG] = LV2_LOG__log,
};

const char *const lv2_core_provided[] = LV2_CORE_PROVIDED_INIT;


/*
************************************************************************************************************************
*           LOCAL GLOBAL VARIABLES
************************************************************************************************************************
*/

static struct
{
    int configured;
    struct lv2_core_config config;
    char **features;            // the configured list, copied, NULL-terminated
    char *lilv_soname;
    uint32_t provided;          // bit k: lv2_feature_uri[k] is on the configured list
} g;

/* urid:map and urid:unmap: ONE table for the process, append-only. A URID is the index + 1 (0 is "no URID"); a string is
 * kept for the life of the process, so a URID never changes meaning. The entries live in segments that never move, the
 * segment k holding URID_SEGMENT0 << k of them, and the count is published with a release store after the entry is
 * written: a lookup of a URI already mapped takes no lock, allocates nothing and copies nothing. Only an append takes
 * the writer's lock. */
#define URID_SEGMENT0                   64u
#define URID_SEGMENTS                   24u

static struct
{
    pthread_mutex_t writer;
    _Atomic(const char *) *segment[URID_SEGMENTS];
    _Atomic uint32_t count;
} g_urid = { PTHREAD_MUTEX_INITIALIZER, { NULL }, 0 };

/*
************************************************************************************************************************
*           LOCAL FUNCTIONS: THE PROVIDERS
************************************************************************************************************************
*/

/* the slot of the URID index `i`: segment k starts at URID_SEGMENT0 * (2^k - 1) */
static _Atomic(const char *) *urid_slot(uint32_t i, int create)
{
    uint32_t k = 0, base = 0;

    while (k < URID_SEGMENTS && i >= base + (URID_SEGMENT0 << k))
    {
        base += URID_SEGMENT0 << k;
        k++;
    }
    if (k == URID_SEGMENTS)
        return NULL;
    if (!g_urid.segment[k] && create)
        g_urid.segment[k] = calloc(URID_SEGMENT0 << k, sizeof(*g_urid.segment[k]));
    return g_urid.segment[k] ? &g_urid.segment[k][i - base] : NULL;
}

/* the URID of `uri` among the first `n`, or 0 */
static LV2_URID urid_find(const char *uri, uint32_t from, uint32_t n)
{
    uint32_t i;

    for (i = from; i < n; i++)
    {
        const char *s = atomic_load_explicit(urid_slot(i, 0), memory_order_relaxed);

        if (strcmp(s, uri) == 0)
            return i + 1;
    }
    return 0;
}

LV2_URID lv2_urid_lookup(const char *uri)
{
    const uint32_t n = atomic_load_explicit(&g_urid.count, memory_order_acquire);
    uint32_t seen;
    LV2_URID id;

    if (!uri)
        return 0;
    id = urid_find(uri, 0, n);
    if (id)
        return id;
    pthread_mutex_lock(&g_urid.writer);
    seen = atomic_load_explicit(&g_urid.count, memory_order_relaxed);
    id = urid_find(uri, n, seen);      // another writer may have appended it meanwhile
    if (!id)
    {
        _Atomic(const char *) *slot = urid_slot(seen, 1);
        char *copy = slot ? strdup(uri) : NULL;

        if (copy)
        {
            atomic_store_explicit(slot, copy, memory_order_relaxed);
            atomic_store_explicit(&g_urid.count, seen + 1, memory_order_release);
            id = seen + 1;
        }
    }
    pthread_mutex_unlock(&g_urid.writer);
    return id;
}

const char *lv2_urid_unlookup(LV2_URID id)
{
    const uint32_t n = atomic_load_explicit(&g_urid.count, memory_order_acquire);

    if (id < 1 || id > n)
        return NULL;
    return atomic_load_explicit(urid_slot(id - 1, 0), memory_order_relaxed);
}

static int provider_of(const char *uri)
{
    int k;

    for (k = 0; k < LV2_FEATURE_KINDS; k++)
        if (strcmp(lv2_feature_uri[k], uri) == 0)
            return k;
    return -1;
}

static int same_text(const char *a, const char *b)
{
    return (!a && !b) || (a && b && strcmp(a, b) == 0);
}

/* what a second configure must repeat word for word */
static int same_config(const struct lv2_core_config *c)
{
    uint32_t i;

    if (c->worker_poll_us != g.config.worker_poll_us || c->worker_ring_bytes != g.config.worker_ring_bytes
        || c->atom_buffer_bytes != g.config.atom_buffer_bytes || !same_text(c->lilv_soname, g.lilv_soname))
        return 0;
    for (i = 0; c->features && c->features[i]; i++)
        if (!g.features[i] || strcmp(c->features[i], g.features[i]) != 0)
            return 0;
    return g.features[i] == NULL;
}


/*
************************************************************************************************************************
*           GLOBAL FUNCTIONS
************************************************************************************************************************
*/

int lv2_core_configure(const struct lv2_core_config *c, char why[LV2_CORE_WHY_MAX])
{
    uint32_t n = 0, provided = 0, i;
    char **features;

    if (!c)
    {
        lv2_why_set(why, LV2_CODE_NO_REALISATION);
        return -1;
    }
    if (g.configured)
    {
        if (same_config(c))
            return 0;
        lv2_why_set(why, "lv2.configure.once");
        return -1;
    }
    if (c->worker_ring_bytes < 16u || (c->worker_ring_bytes & (c->worker_ring_bytes - 1u)) != 0u || c->worker_poll_us == 0)
    {
        lv2_why_set(why, "lv2.configure.numbers");
        return -1;
    }
    for (n = 0; c->features && c->features[n]; n++)
    {
        const int k = provider_of(c->features[n]);

        if (k < 0)
        {
            lv2_why_set(why, LV2_CODE_FEATURES_MISSING);
            return -1;
        }
        provided |= 1u << k;
    }
    features = calloc(n + 1u, sizeof(*features));
    if (!features)
    {
        lv2_why_set(why, LV2_CODE_NO_REALISATION);
        return -1;
    }
    for (i = 0; i < n; i++)
    {
        features[i] = strdup(c->features[i]);
        if (!features[i])
        {
            while (i > 0)
                free(features[--i]);
            free(features);
            lv2_why_set(why, LV2_CODE_NO_REALISATION);
            return -1;
        }
    }
    g.features = features;
    g.lilv_soname = c->lilv_soname ? strdup(c->lilv_soname) : NULL;
    g.config = *c;
    g.config.features = (const char *const *)g.features;
    g.config.lilv_soname = g.lilv_soname;
    g.provided = provided;
    g.configured = 1;
    lv2_why_set(why, "");
    return 0;
}

const struct lv2_core_config *lv2_core_config(void)
{
    return g.configured ? &g.config : NULL;
}

int lv2_feature_configured(const char *uri)
{
    const int k = provider_of(uri);

    return k >= 0 && (g.provided & (1u << k)) != 0;
}

int lv2_feature_on(enum lv2_feature_kind kind)
{
    return (g.provided & (1u << kind)) != 0;
}
