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
* mix_host_backend.c: one URID map per process, guarded for the main and the worker threads; a log:log sink that writes
* nothing; and which providers are on, which is the configured list's say. The options and the worker schedule are each
* instance's own (lv2_instance.c). This table says how each feature is built, never which are on.
*
************************************************************************************************************************
*/


/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <pthread.h>
#include <stdarg.h>
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

const char *const lv2_core_provided[] =
{
    LV2_URID__map, LV2_URID__unmap, LV2_OPTIONS__options, LV2_BUF_SIZE__boundedBlockLength, LV2_WORKER__schedule,
    LV2_STATE__loadDefaultState, LV2_LOG__log, NULL
};


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

/* urid:map and urid:unmap: ONE table for the process. A URID is the index + 1 (0 is "no URID"); a string is kept for
 * the life of the process, so a URID never changes meaning. */
static struct
{
    pthread_mutex_t lock;
    char **uris;
    uint32_t n, cap;
} g_urid = { PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0 };


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS: THE PROVIDERS
************************************************************************************************************************
*/

static LV2_URID urid_map(LV2_URID_Map_Handle h, const char *uri)
{
    LV2_URID id = 0;
    uint32_t i;

    (void)h;
    if (!uri)
        return 0;
    pthread_mutex_lock(&g_urid.lock);
    for (i = 0; i < g_urid.n && !id; i++)
        if (strcmp(g_urid.uris[i], uri) == 0)
            id = i + 1;
    if (!id)
    {
        char *copy;

        if (g_urid.n == g_urid.cap)
        {
            const uint32_t cap = g_urid.cap ? 2u * g_urid.cap : 64u;
            char **grown = realloc(g_urid.uris, cap * sizeof(*grown));

            if (grown)
            {
                g_urid.uris = grown;
                g_urid.cap = cap;
            }
        }
        copy = g_urid.n < g_urid.cap ? strdup(uri) : NULL;
        if (copy)
        {
            g_urid.uris[g_urid.n++] = copy;
            id = g_urid.n;
        }
    }
    pthread_mutex_unlock(&g_urid.lock);
    return id;
}

static const char *urid_unmap(LV2_URID_Unmap_Handle h, LV2_URID id)
{
    const char *uri;

    (void)h;
    pthread_mutex_lock(&g_urid.lock);
    uri = id >= 1 && id <= g_urid.n ? g_urid.uris[id - 1] : NULL;
    pthread_mutex_unlock(&g_urid.lock);
    return uri;
}

/* log:log formats nothing and writes nothing: a line from run() must never reach a system call */
static int log_vprintf(LV2_Log_Handle h, LV2_URID type, const char *fmt, va_list ap)
{
    (void)h;
    (void)type;
    (void)fmt;
    (void)ap;
    return 0;
}

static int log_printf(LV2_Log_Handle h, LV2_URID type, const char *fmt, ...)
{
    (void)h;
    (void)type;
    (void)fmt;
    return 0;
}

LV2_URID_Map lv2_urid_map = { NULL, urid_map };
LV2_URID_Unmap lv2_urid_unmap = { NULL, urid_unmap };
LV2_Log_Log lv2_log = { NULL, log_printf, log_vprintf };

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
