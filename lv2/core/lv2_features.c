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
* The configuration and the feature providers of the LV2 host (see lv2_core.h): one URID table per process, lock-free for
* a URI already mapped, and which providers are on, which is the configured list's say. The map, the log, the options and
* the worker schedule an instance is handed are its own structures over that table (lv2_instance.c), so a call names the
* instance it came from.
*
* The feature options (omx_lv2_features.h) decide, within the configured list, which features an instance is offered,
* whether a plugin requiring one that is not offered is refused, the buf-size declarations and block lengths reported,
* and where log:log lines go. The same array builder serves a host of its own through omx_lv2_features_build.
*
************************************************************************************************************************
*/


/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <lv2/atom/atom.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
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
*           LOCAL DEFINES
************************************************************************************************************************
*/

/* the console's behaviour: every feature offered, strict, the activation's lengths declared bounded, a silent log */
#define LV2_FEATURE_OPTIONS_DEFAULT \
    { OMX_LV2_FEATURES_ALL, 1, { OMX_LV2_BLOCK_BOUNDED, 0u, 0u, 0u }, { OMX_LV2_LOG_SILENT, NULL, NULL, NULL, 0u } }

/* the buf-size declarations, OMX_LV2_BLOCK_* bit d the URI d */
#define BLOCK_DECLARATIONS              3u

/* the longest log line a sink or a capture is handed, its NUL included: a longer one is cut */
#define LOG_LINE_MAX                    512


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
    char *state_root_given;     // the state root as the configuration named it, for a second configure to repeat
    char *state_root;           // its real path, made by the configure
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

/* the feature options of the plugins opened and the instances made from now on */
static struct omx_lv2_feature_options g_options = LV2_FEATURE_OPTIONS_DEFAULT;

static const char *const g_declared_uri[BLOCK_DECLARATIONS] =
{
    LV2_BUF_SIZE__boundedBlockLength, LV2_BUF_SIZE__fixedBlockLength, LV2_BUF_SIZE__powerOf2BlockLength,
};

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
        || c->atom_buffer_bytes != g.config.atom_buffer_bytes || !same_text(c->lilv_soname, g.lilv_soname)
        || !same_text(c->state_root, g.state_root_given))
        return 0;
    for (i = 0; c->features && c->features[i]; i++)
        if (!g.features[i] || strcmp(c->features[i], g.features[i]) != 0)
            return 0;
    return g.features[i] == NULL;
}


/* every directory of the absolute `path`, made when missing: 0, or -1 */
int lv2_mkdirs(const char *path)
{
    char buf[PATH_MAX];
    size_t i, n = path ? strlen(path) : 0;

    if (n == 0 || n >= sizeof(buf) || path[0] != '/')
        return -1;
    memcpy(buf, path, n + 1u);
    for (i = 1; i <= n; i++)
    {
        struct stat st;

        if (buf[i] != '/' && buf[i] != '\0')
            continue;
        buf[i] = '\0';
        if (mkdir(buf, 0755) != 0 && (errno != EEXIST || stat(buf, &st) != 0 || !S_ISDIR(st.st_mode)))
            return -1;
        buf[i] = path[i];
    }
    return 0;
}

/* the state root made and taken by its real path: NULL when it is not absolute or cannot be made */
static char *state_root_make(const char *root)
{
    char real[PATH_MAX];

    if (!root || root[0] != '/' || lv2_mkdirs(root) != 0 || !realpath(root, real))
        return NULL;
    return strdup(real);
}


/*
************************************************************************************************************************
*           GLOBAL FUNCTIONS
************************************************************************************************************************
*/

int lv2_core_configure(const struct lv2_core_config *c, char why[LV2_CORE_WHY_MAX])
{
    uint32_t n = 0, provided = 0, i;
    char **features, *root = NULL;

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
        const int k = strcmp(c->features[n], LV2_STATE__makePath) == 0 ? (int)F_MAKE_PATH : provider_of(c->features[n]);

        if (k < 0)
        {
            lv2_why_set(why, LV2_CODE_FEATURES_MISSING);
            return -1;
        }
        provided |= 1u << k;
    }
    // state:makePath needs a root to make the instances' directories in: without one it is not offered
    if (c->state_root)
    {
        root = state_root_make(c->state_root);
        if (!root)
        {
            lv2_why_set(why, "lv2.configure.state-root");
            return -1;
        }
    }
    else
        provided &= ~(1u << F_MAKE_PATH);
    features = calloc(n + 1u, sizeof(*features));
    if (!features)
    {
        free(root);
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
            free(root);
            lv2_why_set(why, LV2_CODE_NO_REALISATION);
            return -1;
        }
    }
    g.features = features;
    g.lilv_soname = c->lilv_soname ? strdup(c->lilv_soname) : NULL;
    g.config = *c;
    g.config.features = (const char *const *)g.features;
    g.config.lilv_soname = g.lilv_soname;
    g.state_root = root;
    g.state_root_given = c->state_root ? strdup(c->state_root) : NULL;
    g.config.state_root = g.state_root;
    g.provided = provided;
    g.configured = 1;
    lv2_why_set(why, "");
    return 0;
}

const struct lv2_core_config *lv2_core_config(void)
{
    return g.configured ? &g.config : NULL;
}

int lv2_feature_on(enum lv2_feature_kind kind)
{
    return (g.provided & (1u << kind)) != 0;
}

uint32_t lv2_features_configured(void)
{
    return g.provided;
}

int lv2_core_feature_options_set(const struct omx_lv2_feature_options *o, char why[LV2_CORE_WHY_MAX])
{
    if (o && omx_lv2_feature_options_check(o) != 0)
    {
        lv2_why_set(why, "lv2.configure.options");
        return -1;
    }
    if (o)
        g_options = *o;
    else
        omx_lv2_feature_options_init(&g_options);
    lv2_why_set(why, "");
    return 0;
}

const struct omx_lv2_feature_options *lv2_core_feature_options(void)
{
    return &g_options;
}

int lv2_feature_offered_within(const struct omx_lv2_feature_options *o, uint32_t allowed, const char *uri)
{
    const uint32_t on = o->features & allowed;
    const int k = uri ? provider_of(uri) : -1;
    uint32_t d;

    if (uri && strcmp(uri, LV2_STATE__makePath) == 0)
        return (allowed & (1u << F_MAKE_PATH)) != 0;
    if (k == F_BOUNDED)
        return (on & OMX_LV2_FEATURE_BIT(F_BOUNDED)) && (o->block.declares & OMX_LV2_BLOCK_BOUNDED);
    if (k >= 0)
        return (on & OMX_LV2_FEATURE_BIT(k)) != 0;
    for (d = 0; uri && d < BLOCK_DECLARATIONS; d++)
        if (strcmp(g_declared_uri[d], uri) == 0)
            return (on & OMX_LV2_FEATURE_BIT(F_BOUNDED)) && (o->block.declares & (1u << d));
    return 0;
}

uint32_t lv2_features_fill(LV2_Feature *features, const LV2_Feature **ptrs, const struct omx_lv2_feature_options *o,
                           uint32_t allowed, void *const data[LV2_FEATURE_KINDS + 1])
{
    const uint32_t on = o->features & allowed;
    uint32_t n = 0, d;
    int k;

    for (k = 0; k < LV2_FEATURE_KINDS; k++)
    {
        if (!(on & OMX_LV2_FEATURE_BIT(k)))
            continue;
        // the buf-size item stands in the kinds' order as bounded when bounded is declared: flags, the presence the promise
        if (k == F_BOUNDED && !(o->block.declares & OMX_LV2_BLOCK_BOUNDED))
            continue;
        features[n].URI = lv2_feature_uri[k];
        features[n].data = k == F_BOUNDED || k == F_DEFAULT_STATE ? NULL : data[k];
        ptrs[n] = &features[n];
        n++;
    }
    // the declarations beyond bounded, after every kind
    for (d = 1; d < BLOCK_DECLARATIONS && (on & OMX_LV2_FEATURE_BIT(F_BOUNDED)); d++)
    {
        if (!(o->block.declares & (1u << d)))
            continue;
        features[n].URI = g_declared_uri[d];
        features[n].data = NULL;
        ptrs[n] = &features[n];
        n++;
    }
    // state:makePath, last: the configuration's alone
    if ((allowed & (1u << F_MAKE_PATH)) && data[F_MAKE_PATH])
    {
        features[n].URI = LV2_STATE__makePath;
        features[n].data = data[F_MAKE_PATH];
        ptrs[n] = &features[n];
        n++;
    }
    ptrs[n] = NULL;
    return n;
}

int lv2_block_lengths(const struct omx_lv2_feature_options *o, uint32_t min_frames, uint32_t max_frames, int32_t *min,
                      int32_t *max, int32_t *nominal)
{
    const uint32_t hi = o->block.max ? o->block.max : max_frames;
    // fixed declared: every run gets the maximum, so that is the minimum and the nominal the plugin is told too,
    // whatever the options name (a fixed length is one number)
    const int fixed = (o->block.declares & OMX_LV2_BLOCK_FIXED) != 0;
    const uint32_t lo = fixed ? hi : (o->block.min ? o->block.min : min_frames);
    const uint32_t nom = fixed ? hi : (o->block.nominal ? o->block.nominal : hi);

    if (lo > hi || hi > (uint32_t)INT32_MAX || nom > (uint32_t)INT32_MAX)
        return -1;
    *min = (int32_t)lo;
    *max = (int32_t)hi;
    *nominal = (int32_t)nom;
    return 0;
}

void lv2_options_fill(LV2_Options_Option opts[5], float *rate, int32_t *min, int32_t *max, int32_t *nominal)
{
    const LV2_URID t_int = lv2_urid_lookup(LV2_ATOM__Int), t_float = lv2_urid_lookup(LV2_ATOM__Float);
    const LV2_Options_Option o[5] =
    {
        { LV2_OPTIONS_INSTANCE, 0, lv2_urid_lookup(LV2_PARAMETERS__sampleRate), sizeof(float), t_float, rate },
        { LV2_OPTIONS_INSTANCE, 0, lv2_urid_lookup(LV2_BUF_SIZE__minBlockLength), sizeof(int32_t), t_int, min },
        { LV2_OPTIONS_INSTANCE, 0, lv2_urid_lookup(LV2_BUF_SIZE__maxBlockLength), sizeof(int32_t), t_int, max },
        { LV2_OPTIONS_INSTANCE, 0, lv2_urid_lookup(LV2_BUF_SIZE__nominalBlockLength), sizeof(int32_t), t_int, nominal },
        { LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, NULL },
    };

    memcpy(opts, o, sizeof(o));
}

int lv2_log_emit(const struct omx_lv2_feature_options *o, LV2_URID type, const char *fmt, va_list ap)
{
    char line[LOG_LINE_MAX];
    size_t used, room, len;
    int n;

    if (o->log.mode == OMX_LV2_LOG_SILENT || !fmt)
        return 0;
    n = vsnprintf(line, sizeof(line), fmt, ap);
    if (n < 0)
        return n;
    if (o->log.mode == OMX_LV2_LOG_ROUTE)
    {
        if (o->log.sink)
            o->log.sink(o->log.ctx, lv2_urid_unlookup(type), line);
        return n;
    }
    if (o->log.mode != OMX_LV2_LOG_CAPTURE || !o->log.capture || o->log.capture_size == 0)
        return n;
    // appended after what the buffer holds, a newline closing the line, cut short at the buffer's end
    used = strnlen(o->log.capture, o->log.capture_size - 1u);
    room = o->log.capture_size - 1u - used;
    len = strlen(line);
    if (len > room)
        len = room;
    memcpy(o->log.capture + used, line, len);
    used += len;
    if (used < o->log.capture_size - 1u && (len == 0 || line[len - 1] != '\n'))
        o->log.capture[used++] = '\n';
    o->log.capture[used] = '\0';
    return n;
}


/*
************************************************************************************************************************
*           GLOBAL FUNCTIONS: THE PROVIDER OF A HOST OF ITS OWN (omx_lv2_features.h)
************************************************************************************************************************
*/

const char *const omx_lv2_feature_uris[OMX_LV2_FEATURE_KINDS] =
{
    [OMX_LV2_F_MAP] = LV2_URID__map,
    [OMX_LV2_F_UNMAP] = LV2_URID__unmap,
    [OMX_LV2_F_OPTIONS] = LV2_OPTIONS__options,
    [OMX_LV2_F_BOUNDED] = LV2_BUF_SIZE__boundedBlockLength,
    [OMX_LV2_F_WORKER] = LV2_WORKER__schedule,
    [OMX_LV2_F_DEFAULT_STATE] = LV2_STATE__loadDefaultState,
    [OMX_LV2_F_LOG] = LV2_LOG__log,
};

void omx_lv2_feature_options_init(struct omx_lv2_feature_options *o)
{
    static const struct omx_lv2_feature_options defaults = LV2_FEATURE_OPTIONS_DEFAULT;

    if (o)
        *o = defaults;
}

int omx_lv2_feature_options_check(const struct omx_lv2_feature_options *o)
{
    if (!o || (o->features & ~OMX_LV2_FEATURES_ALL) != 0 || (o->block.declares & ~OMX_LV2_BLOCK_ALL) != 0)
        return -1;
    if (o->block.min && o->block.max && o->block.min > o->block.max)
        return -1;
    switch (o->log.mode)
    {
    case OMX_LV2_LOG_SILENT: return 0;
    case OMX_LV2_LOG_ROUTE: return o->log.sink ? 0 : -1;
    case OMX_LV2_LOG_CAPTURE: return o->log.capture && o->log.capture_size ? 0 : -1;
    }
    return -1;
}

int omx_lv2_feature_kind(const char *uri)
{
    return uri ? provider_of(uri) : -1;
}

int omx_lv2_feature_mask(const char *const *uris, uint32_t *mask)
{
    uint32_t m = 0, i;

    for (i = 0; uris && uris[i]; i++)
    {
        const int k = provider_of(uris[i]);

        if (k < 0)
            return -1;
        m |= OMX_LV2_FEATURE_BIT(k);
    }
    *mask = m;
    return 0;
}

int omx_lv2_feature_offered(const struct omx_lv2_feature_options *o, const char *uri)
{
    struct omx_lv2_feature_options defaults;

    if (!o)
    {
        omx_lv2_feature_options_init(&defaults);
        o = &defaults;
    }
    return lv2_feature_offered_within(o, OMX_LV2_FEATURES_ALL, uri);
}

int omx_lv2_features_admit(const struct omx_lv2_feature_options *o, const char *const *required, const char **missing)
{
    struct omx_lv2_feature_options defaults;
    const char *first = NULL;
    uint32_t i;

    if (!o)
    {
        omx_lv2_feature_options_init(&defaults);
        o = &defaults;
    }
    for (i = 0; required && required[i] && !first; i++)
        if (!lv2_feature_offered_within(o, OMX_LV2_FEATURES_ALL, required[i]))
            first = required[i];
    if (missing)
        *missing = first;
    return first && o->strict ? -1 : 0;
}

LV2_URID omx_lv2_urid_map(const char *uri)
{
    return lv2_urid_lookup(uri);
}

const char *omx_lv2_urid_unmap(LV2_URID id)
{
    return lv2_urid_unlookup(id);
}

static LV2_URID block_map(LV2_URID_Map_Handle h, const char *uri)
{
    (void)h;
    return lv2_urid_lookup(uri);
}

static const char *block_unmap(LV2_URID_Unmap_Handle h, LV2_URID id)
{
    (void)h;
    return lv2_urid_unlookup(id);
}

static int block_log_vprintf(LV2_Log_Handle h, LV2_URID type, const char *fmt, va_list ap)
{
    const struct omx_lv2_feature_block *b = h;

    return lv2_log_emit(&b->options, type, fmt, ap);
}

static int block_log_printf(LV2_Log_Handle h, LV2_URID type, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = block_log_vprintf(h, type, fmt, ap);
    va_end(ap);
    return n;
}

const LV2_Feature *const *omx_lv2_features_build(struct omx_lv2_feature_block *b, const struct omx_lv2_feature_options *o,
                                                 double rate, uint32_t min_frames, uint32_t max_frames,
                                                 LV2_Worker_Schedule *schedule)
{
    void *data[LV2_FEATURE_KINDS + 1] = { NULL };

    if (!b)
        return NULL;
    if (o)
        b->options = *o;
    else
        omx_lv2_feature_options_init(&b->options);
    if (omx_lv2_feature_options_check(&b->options) != 0
        || ((b->options.features & OMX_LV2_FEATURE_BIT(F_WORKER)) && !schedule)
        || lv2_block_lengths(&b->options, min_frames, max_frames, &b->min_block, &b->max_block, &b->nominal_block) != 0)
        return NULL;
    b->map.handle = NULL;
    b->map.map = block_map;
    b->unmap.handle = NULL;
    b->unmap.unmap = block_unmap;
    b->log.handle = b;
    b->log.printf = block_log_printf;
    b->log.vprintf = block_log_vprintf;
    b->rate = (float)rate;
    lv2_options_fill(b->opts, &b->rate, &b->min_block, &b->max_block, &b->nominal_block);
    data[F_MAP] = &b->map;
    data[F_UNMAP] = &b->unmap;
    data[F_OPTIONS] = b->opts;
    data[F_WORKER] = schedule;
    data[F_LOG] = &b->log;
    lv2_features_fill(b->features, b->ptrs, &b->options, OMX_LV2_FEATURES_ALL, data);
    return b->ptrs;
}

const LV2_Descriptor *omx_lv2_open(void **so, const char *path, const char *uri, const char **why)
{
    LV2_Descriptor_Function fn = NULL;
    const LV2_Descriptor *d = NULL;
    uint32_t i;

    *so = path && uri ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : NULL;
    if (!*so)
    {
        if (why)
            *why = "dlopen";
        return NULL;
    }
    *(void **)&fn = dlsym(*so, "lv2_descriptor");
    for (i = 0; fn && (d = fn(i)) != NULL; i++)
        if (strcmp(d->URI, uri) == 0)
            return d;
    dlclose(*so);
    *so = NULL;
    if (why)
        *why = fn ? "uri" : "lv2_descriptor";
    return NULL;
}
