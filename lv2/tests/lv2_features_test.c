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
 * lv2_features_test.c — the feature provider of a host of its own (omx_lv2_features.h), and each of its options as a
 * plugin observes it: the feature-probe fixture is opened through omx_lv2_open, instantiated with the array
 * omx_lv2_features_build makes, and read back through dlsym. No lilv: this is the path a host that reads the TTL itself
 * takes.
 *
 *   defaults  the options init fills are the console's: every feature in the kinds' order, the activation's lengths
 *             declared bounded, the nominal the maximum, a silent log, strict.
 *   features  each kind withheld alone is missing from what the plugin sees, and only it.
 *   strict    a requirement not offered refuses under strict and is admitted, named, without it.
 *   block     the declarations the plugin sees (bounded, fixed, power of two), and the lengths the options report.
 *   log       silent, routed to a sink, captured into a buffer: the line the plugin wrote, where the option sends it.
 *   build     what refuses to build, never a shorter array.
 *   urid      the one table, and the kinds through their URIs.
 *   open      the door's refusals, each with its step.
 *
 *   lv2_features_test <path of omx-feature-probe.so>
 */

#include <lv2/buf-size/buf-size.h>
#include <lv2/log/log.h>
#include <lv2/options/options.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#include "omx_lv2_features.h"
#include "lv2_rtmempool.h"
#include "fixtures/lv2_feature_probe.h"
#include "lv2_test_util.h"

#define RATE 48000.0

static const char *g_so;
static const LV2_Descriptor *g_desc;
static struct lv2_feature_probe *g_probe;

static LV2_Worker_Status no_work(LV2_Worker_Schedule_Handle h, uint32_t size, const void *data)
{
    (void)h;
    (void)size;
    (void)data;
    return LV2_WORKER_SUCCESS;
}

static LV2_Worker_Schedule g_schedule = { NULL, no_work };

/* the probe instantiated with what `o` builds over an activation of `min` to `max`: 0, or -1 when nothing was built */
static int observe(const struct omx_lv2_feature_options *o, uint32_t min, uint32_t max)
{
    static struct omx_lv2_feature_block b;
    const LV2_Feature *const *f = omx_lv2_features_build(&b, o, RATE, min, max, &g_schedule);
    LV2_Handle h;

    if (!f)
        return -1;
    h = g_desc->instantiate(g_desc, RATE, "", f);
    if (!h)
        return -1;
    g_desc->cleanup(h);
    return 0;
}

static const uint32_t g_probe_bit[OMX_LV2_FEATURE_KINDS] =
{
    [OMX_LV2_F_MAP] = LV2_FEATURE_PROBE_MAP,
    [OMX_LV2_F_UNMAP] = LV2_FEATURE_PROBE_UNMAP,
    [OMX_LV2_F_OPTIONS] = LV2_FEATURE_PROBE_OPTIONS,
    [OMX_LV2_F_BOUNDED] = LV2_FEATURE_PROBE_BOUNDED,
    [OMX_LV2_F_WORKER] = LV2_FEATURE_PROBE_WORKER,
    [OMX_LV2_F_DEFAULT_STATE] = LV2_FEATURE_PROBE_DEFAULT_STATE,
    [OMX_LV2_F_LOG] = LV2_FEATURE_PROBE_LOG,
    [OMX_LV2_F_RTMEMPOOL] = LV2_FEATURE_PROBE_RTMEMPOOL | LV2_FEATURE_PROBE_RTMEMPOOL_V1,
};

/* the eight kinds as the probe sees them: the pool under both its URIs */
#define SEVEN (LV2_FEATURE_PROBE_MAP | LV2_FEATURE_PROBE_UNMAP | LV2_FEATURE_PROBE_OPTIONS | LV2_FEATURE_PROBE_BOUNDED \
               | LV2_FEATURE_PROBE_WORKER | LV2_FEATURE_PROBE_DEFAULT_STATE | LV2_FEATURE_PROBE_LOG \
               | LV2_FEATURE_PROBE_RTMEMPOOL | LV2_FEATURE_PROBE_RTMEMPOOL_V1)
/* the array's length under the defaults: every kind, and the pool's deprecated URI after the declarations */
#define SLOTS_DEFAULT (OMX_LV2_FEATURE_KINDS + 1u)

static void t_defaults(void)
{
    struct omx_lv2_feature_options o;
    int k, in_order = 1;

    memset(&o, 0x5a, sizeof(o));
    omx_lv2_feature_options_init(&o);
    CHECK(o.features == OMX_LV2_FEATURES_ALL, "defaults: every feature offered");
    CHECK(o.strict == 1, "defaults: strict");
    CHECK(o.block.declares == OMX_LV2_BLOCK_BOUNDED, "defaults: the block declared bounded, and only bounded");
    CHECK(o.block.min == 0 && o.block.max == 0 && o.block.nominal == 0, "defaults: the activation's lengths");
    CHECK(o.log.mode == OMX_LV2_LOG_SILENT && !o.log.sink && !o.log.ctx && !o.log.capture && o.log.capture_size == 0,
          "defaults: a silent log, no sink, no buffer");
    CHECK(omx_lv2_feature_options_check(&o) == 0, "defaults: they check");

    // what the plugin sees under them, through init and through NULL alike
    CHECK(observe(&o, 16, 256) == 0, "defaults: the probe instantiates");
    CHECK(g_probe->seen == SEVEN && g_probe->n_features == SLOTS_DEFAULT,
          "defaults: the probe sees the eight features, the pool twice, and nothing else (0x%x, %u)", g_probe->seen, g_probe->n_features);
    CHECK(!strcmp(g_probe->order[OMX_LV2_FEATURE_KINDS], LV2_RTSAFE_MEMORY_POOL_DEPRECATED_URI),
          "defaults: the pool's deprecated URI after the kinds");
    CHECK(g_probe->pool_round_trip == 1 && g_probe->pool_v1_round_trip == 1,
          "defaults: the probe makes a pool, takes and returns a chunk and destroys it, through both structs (%d, %d)",
          g_probe->pool_round_trip, g_probe->pool_v1_round_trip);
    for (k = 0; k < OMX_LV2_FEATURE_KINDS; k++)
        in_order &= strcmp(g_probe->order[k], omx_lv2_feature_uris[k]) == 0;
    CHECK(in_order, "defaults: in the kinds' order");
    CHECK(g_probe->has_rate && g_probe->rate == (float)RATE, "defaults: options carry the rate (%g)", g_probe->rate);
    CHECK(g_probe->has_min && g_probe->min_block == 16, "defaults: the minimum is the activation's (%d)", g_probe->min_block);
    CHECK(g_probe->has_max && g_probe->max_block == 256, "defaults: the maximum is the activation's (%d)", g_probe->max_block);
    CHECK(g_probe->has_nominal && g_probe->nominal_block == 256, "defaults: the nominal is the maximum (%d)", g_probe->nominal_block);
    CHECK(g_probe->unmap_round_trip, "defaults: unmap gives back what map was handed");
    CHECK(g_probe->log_answer == 0, "defaults: the log answers 0, formatting nothing (%d)", g_probe->log_answer);
    CHECK(observe(NULL, 16, 256) == 0 && g_probe->seen == SEVEN && g_probe->max_block == 256,
          "defaults: NULL options are the defaults");
}

static void t_feature_set(void)
{
    struct omx_lv2_feature_options o;
    int k;

    for (k = 0; k < OMX_LV2_FEATURE_KINDS; k++)
    {
        omx_lv2_feature_options_init(&o);
        o.features &= ~OMX_LV2_FEATURE_BIT(k);
        CHECK(observe(&o, 16, 256) == 0 && g_probe->seen == (SEVEN & ~g_probe_bit[k]),
              "features: %s withheld, the probe sees the other seven (0x%x)", omx_lv2_feature_uris[k], g_probe->seen);
        CHECK(!omx_lv2_feature_offered(&o, omx_lv2_feature_uris[k]), "features: %s is not offered", omx_lv2_feature_uris[k]);
    }
    omx_lv2_feature_options_init(&o);
    o.features = 0;
    CHECK(observe(&o, 16, 256) == 0 && g_probe->seen == 0 && g_probe->n_features == 0,
          "features: none offered, the probe sees an empty array");
    CHECK(g_probe->log_answer == -1 && !g_probe->has_rate, "features: and degrades: no log, no options read");
    o.features = OMX_LV2_FEATURE_BIT(OMX_LV2_F_MAP) | OMX_LV2_FEATURE_BIT(OMX_LV2_F_OPTIONS);
    CHECK(observe(&o, 16, 256) == 0 && g_probe->has_max && g_probe->max_block == 256 && !g_probe->unmap_round_trip,
          "features: options without unmap: the lengths read, no round trip");
}

static void t_strict(void)
{
    const char *const need_worker[] = { LV2_URID__map, LV2_WORKER__schedule, NULL };
    const char *const need_path[] = { LV2_STATE__mapPath, NULL };
    struct omx_lv2_feature_options o;
    const char *missing = "untouched";

    omx_lv2_feature_options_init(&o);
    CHECK(omx_lv2_features_admit(&o, need_worker, &missing) == 0 && missing == NULL,
          "strict: every requirement offered is admitted, nothing missing");
    CHECK(omx_lv2_features_admit(NULL, need_path, &missing) == -1 && missing && !strcmp(missing, LV2_STATE__mapPath),
          "strict: by default a requirement no provider serves refuses, named");
    o.features &= ~OMX_LV2_FEATURE_BIT(OMX_LV2_F_WORKER);
    CHECK(omx_lv2_features_admit(&o, need_worker, &missing) == -1 && missing && !strcmp(missing, LV2_WORKER__schedule),
          "strict: the worker withheld refuses a plugin requiring it, named");
    o.strict = 0;
    missing = NULL;
    CHECK(omx_lv2_features_admit(&o, need_worker, &missing) == 0 && missing && !strcmp(missing, LV2_WORKER__schedule),
          "strict off: the same plugin is admitted, the missing feature still named");
    CHECK(omx_lv2_features_admit(&o, NULL, &missing) == 0 && missing == NULL, "strict: no requirement, nothing missing");
    {
        const char *const need_inplace[] = { LV2_CORE__inPlaceBroken, NULL };

        missing = "untouched";
        CHECK(omx_lv2_feature_offered(NULL, LV2_CORE__inPlaceBroken)
              && omx_lv2_features_admit(NULL, need_inplace, &missing) == 0 && missing == NULL,
              "strict: lv2:inPlaceBroken required is offered by construction and admitted under the defaults");
    }
    // admitted degraded, the plugin is handed no worker: what it instantiates with is what it sees
    CHECK(observe(&o, 16, 256) == 0 && !(g_probe->seen & LV2_FEATURE_PROBE_WORKER),
          "strict off: the admitted plugin instantiates without the worker");
}

static void t_block(void)
{
    struct omx_lv2_feature_options o;

    omx_lv2_feature_options_init(&o);
    o.block.declares = OMX_LV2_BLOCK_ALL;
    CHECK(observe(&o, 256, 256) == 0
          && (g_probe->seen & (LV2_FEATURE_PROBE_BOUNDED | LV2_FEATURE_PROBE_FIXED | LV2_FEATURE_PROBE_POW2))
             == (LV2_FEATURE_PROBE_BOUNDED | LV2_FEATURE_PROBE_FIXED | LV2_FEATURE_PROBE_POW2),
          "block: bounded, fixed and power of two declared, the probe sees all three");
    CHECK(g_probe->n_features == OMX_LV2_FEATURE_KINDS + 3u
          && !strcmp(g_probe->order[OMX_LV2_FEATURE_KINDS], LV2_BUF_SIZE__fixedBlockLength)
          && !strcmp(g_probe->order[OMX_LV2_FEATURE_KINDS + 1], LV2_BUF_SIZE__powerOf2BlockLength)
          && !strcmp(g_probe->order[OMX_LV2_FEATURE_KINDS + 2], LV2_RTSAFE_MEMORY_POOL_DEPRECATED_URI),
          "block: the declarations beyond bounded come after every kind, the pool's deprecated URI after them");
    CHECK(omx_lv2_feature_offered(&o, LV2_BUF_SIZE__fixedBlockLength)
          && omx_lv2_feature_offered(&o, LV2_BUF_SIZE__powerOf2BlockLength), "block: and are offered");
    CHECK(!omx_lv2_feature_offered(NULL, LV2_BUF_SIZE__fixedBlockLength), "block: fixed is not offered by default");
    o.block.declares = OMX_LV2_BLOCK_FIXED;
    CHECK(observe(&o, 256, 256) == 0
          && (g_probe->seen & (LV2_FEATURE_PROBE_BOUNDED | LV2_FEATURE_PROBE_FIXED | LV2_FEATURE_PROBE_POW2)) == LV2_FEATURE_PROBE_FIXED,
          "block: fixed alone, the probe sees fixed and not bounded (0x%x)", g_probe->seen);
    o.features &= ~OMX_LV2_FEATURE_BIT(OMX_LV2_F_BOUNDED);
    o.block.declares = OMX_LV2_BLOCK_ALL;
    CHECK(observe(&o, 256, 256) == 0
          && !(g_probe->seen & (LV2_FEATURE_PROBE_BOUNDED | LV2_FEATURE_PROBE_FIXED | LV2_FEATURE_PROBE_POW2)),
          "block: the buf-size item withheld, no declaration at all");
    CHECK(!omx_lv2_feature_offered(&o, LV2_BUF_SIZE__powerOf2BlockLength), "block: and none is offered");

    omx_lv2_feature_options_init(&o);
    o.block.declares |= OMX_LV2_BLOCK_FIXED;
    CHECK(observe(&o, 64, 512) == 0 && g_probe->min_block == 512 && g_probe->max_block == 512 && g_probe->nominal_block == 512,
          "block: fixed declared, the minimum reported is the maximum, whatever the activation's minimum (%d %d %d)",
          g_probe->min_block, g_probe->max_block, g_probe->nominal_block);
    omx_lv2_feature_options_init(&o);
    o.block.min = 1;
    CHECK(observe(&o, 64, 512) == 0 && g_probe->min_block == 1 && g_probe->max_block == 512 && g_probe->nominal_block == 512,
          "block: a minimum of 1 reported over the activation's 64 (%d %d %d)", g_probe->min_block, g_probe->max_block,
          g_probe->nominal_block);
    o.block.max = 1024;
    o.block.nominal = 128;
    CHECK(observe(&o, 64, 512) == 0 && g_probe->min_block == 1 && g_probe->max_block == 1024 && g_probe->nominal_block == 128,
          "block: maximum and nominal named, the probe reads them (%d %d %d)", g_probe->min_block, g_probe->max_block,
          g_probe->nominal_block);
    omx_lv2_feature_options_init(&o);
    o.block.min = 300;
    CHECK(observe(&o, 64, 256) == -1, "block: a minimum above the activation's maximum builds nothing");
    o.block.max = 200;
    CHECK(omx_lv2_feature_options_check(&o) == -1, "block: a minimum above the maximum does not check");
}

struct sink
{
    unsigned calls;
    char type[96];
    char line[256];
};

static void sink(void *ctx, const char *type, const char *line)
{
    struct sink *s = ctx;

    s->calls++;
    snprintf(s->type, sizeof(s->type), "%s", type ? type : "(null)");
    snprintf(s->line, sizeof(s->line), "%s", line);
}

static void t_log(void)
{
    struct omx_lv2_feature_options o;
    struct sink s;
    char want[96], buf[256], small[16];

    snprintf(want, sizeof(want), LV2_FEATURE_PROBE_LINE, (int)RATE);

    omx_lv2_feature_options_init(&o);
    memset(&s, 0, sizeof(s));
    o.log.sink = sink;
    o.log.ctx = &s;
    CHECK(observe(&o, 16, 256) == 0 && s.calls == 0 && g_probe->log_answer == 0,
          "log: silent, a sink named is never called");

    o.log.mode = OMX_LV2_LOG_ROUTE;
    CHECK(observe(&o, 16, 256) == 0 && s.calls == 1, "log: routed, the sink is called once (%u)", s.calls);
    CHECK(!strcmp(s.line, want), "log: with the line the plugin wrote");
    CHECK(!strcmp(s.type, LV2_LOG__Note), "log: and its type (%s)", s.type);
    CHECK(g_probe->log_answer == (int)strlen(want), "log: printf answers the line's length (%d)", g_probe->log_answer);

    memset(buf, 0, sizeof(buf));
    o.log.mode = OMX_LV2_LOG_CAPTURE;
    o.log.capture = buf;
    o.log.capture_size = sizeof(buf);
    s.calls = 0;
    CHECK(observe(&o, 16, 256) == 0 && !strcmp(buf, want) && s.calls == 0, "log: captured, the buffer holds the line");
    CHECK(observe(&o, 16, 256) == 0 && strlen(buf) == 2 * strlen(want) && !strncmp(buf + strlen(want), want, strlen(want)),
          "log: a second line is appended");
    memset(small, 0, sizeof(small));
    o.log.capture = small;
    o.log.capture_size = sizeof(small);
    CHECK(observe(&o, 16, 256) == 0 && strlen(small) == sizeof(small) - 1u && !strncmp(small, want, sizeof(small) - 1u),
          "log: a full buffer is cut short and stays terminated");

    o.log.capture = NULL;
    CHECK(omx_lv2_feature_options_check(&o) == -1 && observe(&o, 16, 256) == -1, "log: capture without a buffer refuses");
    o.log.mode = OMX_LV2_LOG_ROUTE;
    o.log.sink = NULL;
    CHECK(omx_lv2_feature_options_check(&o) == -1, "log: route without a sink refuses");
    o.log.mode = (enum omx_lv2_log_mode)7;
    CHECK(omx_lv2_feature_options_check(&o) == -1, "log: a mode that is none of the three refuses");
}

static void t_build(void)
{
    struct omx_lv2_feature_options o;
    struct omx_lv2_feature_block b;
    const LV2_Feature *const *f;

    omx_lv2_feature_options_init(&o);
    CHECK(omx_lv2_features_build(&b, &o, RATE, 16, 256, NULL) == NULL, "build: the worker offered with no schedule refuses");
    o.features &= ~OMX_LV2_FEATURE_BIT(OMX_LV2_F_WORKER);
    f = omx_lv2_features_build(&b, &o, RATE, 16, 256, NULL);
    CHECK(f && f[OMX_LV2_FEATURE_KINDS] == NULL, "build: without the worker, seven features (the pool twice) and the NULL");
    o.features = OMX_LV2_FEATURES_ALL << 1;
    CHECK(omx_lv2_features_build(&b, &o, RATE, 16, 256, &g_schedule) == NULL, "build: a bit past the kinds refuses");
    omx_lv2_feature_options_init(&o);
    o.block.declares = OMX_LV2_BLOCK_ALL << 1;
    CHECK(omx_lv2_features_build(&b, &o, RATE, 16, 256, &g_schedule) == NULL, "build: a declaration past the three refuses");
    CHECK(omx_lv2_features_build(&b, NULL, RATE, 300, 256, &g_schedule) == NULL, "build: an activation's minimum above its maximum refuses");
    f = omx_lv2_features_build(&b, NULL, RATE, 16, 256, &g_schedule);
    CHECK(f && f[OMX_LV2_F_WORKER]->data == &g_schedule, "build: worker:schedule carries the caller's schedule");
    CHECK(f && f[OMX_LV2_F_OPTIONS]->data == b.opts, "build: options:options points into the block");
    CHECK(f && f[OMX_LV2_F_BOUNDED]->data == NULL && f[OMX_LV2_F_DEFAULT_STATE]->data == NULL, "build: the flags carry no data");
    CHECK(f && f[OMX_LV2_F_RTMEMPOOL]->data != NULL && !strcmp(f[OMX_LV2_F_RTMEMPOOL]->URI, LV2_RTSAFE_MEMORY_POOL__Pool)
          && !strcmp(f[OMX_LV2_FEATURE_KINDS]->URI, LV2_RTSAFE_MEMORY_POOL_DEPRECATED_URI) && f[OMX_LV2_FEATURE_KINDS]->data != NULL
          && f[OMX_LV2_FEATURE_KINDS]->data != f[OMX_LV2_F_RTMEMPOOL]->data,
          "build: the pool carries its provider, and its deprecated URI the deprecated struct, after the kinds");
}

/* rtmempool#Pool through the built array: the chunks preallocated, the atomic path that never allocates, the sleepy one
 * that does and tops the pool up, the refusals */
static void t_rtmempool(void)
{
    struct omx_lv2_feature_block b;
    const LV2_Feature *const *f = omx_lv2_features_build(&b, NULL, RATE, 16, 256, &g_schedule);
    const LV2_RtMemPool_Pool *pool = f ? f[OMX_LV2_F_RTMEMPOOL]->data : NULL;
    const LV2_RtMemPool_Pool_Deprecated *v1 = f ? f[OMX_LV2_FEATURE_KINDS]->data : NULL;
    LV2_RtMemPool_Handle h = NULL, h2 = NULL;
    void *a, *c, *d, *e;

    CHECK(pool && v1, "rtmempool: both structs in the array");
    if (!pool || !v1)
        return;
    CHECK(!pool->create(NULL, "x", 32, 1, 2) && !pool->create(&h, "x", 0, 1, 2) && !pool->create(&h, "x", 32, 3, 2) && h == NULL,
          "rtmempool: no handle, no chunk size or a minimum above the maximum refuses");
    CHECK(pool->create(&h, "test", 48, 2, 4) && h != NULL, "rtmempool: a pool of 48-byte chunks, two preallocated");
    a = pool->allocate_atomic(h);
    c = pool->allocate_atomic(h);
    d = pool->allocate_atomic(h);
    CHECK(a && c && a != c && d == NULL, "rtmempool: the two preallocated chunks come atomically, the third does not (never an allocation)");
    CHECK(((uintptr_t)a & 15u) == 0 && ((uintptr_t)c & 15u) == 0, "rtmempool: chunks on a 16-byte boundary");
    if (a)
        memset(a, 1, 48);
    if (c)
        memset(c, 2, 48);
    d = pool->allocate_sleepy(h);
    CHECK(d != NULL && d != a && d != c, "rtmempool: the sleepy path allocates one when the pool is empty");
    e = pool->allocate_atomic(h);
    CHECK(e != NULL, "rtmempool: and tops the pool back up to its minimum, so the next atomic take succeeds");
    pool->deallocate(h, a);
    pool->deallocate(h, c);
    pool->deallocate(h, d);
    pool->deallocate(h, NULL);
    a = pool->allocate_atomic(h);
    c = pool->allocate_atomic(h);
    d = pool->allocate_atomic(h);
    CHECK(a && c && d, "rtmempool: chunks given back are taken again atomically, NULL given back is nothing");
    pool->destroy(h);       // e, a, c and d still out: destroy frees them too
    CHECK(pool->allocate_atomic(NULL) == NULL && pool->allocate_sleepy(NULL) == NULL, "rtmempool: a NULL pool answers nothing");
    pool->deallocate(NULL, NULL);
    pool->destroy(NULL);
    CHECK(v1->create("v1", 32, 1, 1, &h2) == 1u && h2 != NULL, "rtmempool: the deprecated struct's create, the handle last, answers 1");
    a = v1->allocate_atomic(h2);
    CHECK(a != NULL && v1->allocate_atomic(h2) == NULL, "rtmempool: the same pool behind it");
    v1->deallocate(h2, a);
    v1->destroy(h2);
    CHECK(v1->create("v1", 32, 2, 1, &h2) == 0u, "rtmempool: the deprecated create answers 0 for a refusal");
}

static void t_urid(void)
{
    const char *const all[] = { LV2_URID__map, LV2_URID__unmap, LV2_OPTIONS__options, LV2_BUF_SIZE__boundedBlockLength,
                                LV2_WORKER__schedule, LV2_STATE__loadDefaultState, LV2_LOG__log, LV2_RTSAFE_MEMORY_POOL__Pool, NULL };
    const char *const v1[] = { LV2_RTSAFE_MEMORY_POOL_DEPRECATED_URI, NULL };
    const char *const foreign[] = { LV2_URID__map, LV2_BUF_SIZE__fixedBlockLength, NULL };
    char text[64];
    LV2_URID id;
    uint32_t m = 7;
    int k;

    snprintf(text, sizeof(text), "urn:test:transient#%d", 7);
    id = omx_lv2_urid_map(text);
    memset(text, 'x', sizeof(text) - 1);
    CHECK(id != 0 && !strcmp(omx_lv2_urid_unmap(id), "urn:test:transient#7"), "urid: the table keeps its own copy");
    CHECK(omx_lv2_urid_map("urn:test:transient#7") == id, "urid: the same text maps to the same id");
    CHECK(omx_lv2_urid_map(NULL) == 0 && omx_lv2_urid_unmap(0) == NULL, "urid: NULL maps to 0, 0 unmaps to nothing");
    CHECK(omx_lv2_feature_mask(all, &m) == 0 && m == OMX_LV2_FEATURES_ALL, "urid: the eight URIs are every kind");
    m = 0;
    CHECK(omx_lv2_feature_mask(v1, &m) == 0 && m == OMX_LV2_FEATURE_BIT(OMX_LV2_F_RTMEMPOOL)
          && omx_lv2_feature_kind(LV2_RTSAFE_MEMORY_POOL_DEPRECATED_URI) == OMX_LV2_F_RTMEMPOOL,
          "urid: the pool's deprecated URI is the pool's kind");
    CHECK(omx_lv2_feature_mask(NULL, &m) == 0 && m == 0, "urid: the empty list is the empty mask");
    m = 7;
    CHECK(omx_lv2_feature_mask(foreign, &m) == -1 && m == 7, "urid: a URI no kind serves refuses, the mask untouched");
    for (k = 0; k < OMX_LV2_FEATURE_KINDS; k++)
        CHECK(omx_lv2_feature_kind(omx_lv2_feature_uris[k]) == k, "urid: kind %d round-trips through its URI", k);
    CHECK(omx_lv2_feature_kind(NULL) == -1, "urid: NULL has no kind");
}

static void t_open(void)
{
    const char *why = NULL;
    void *so = (void *)1;

    CHECK(omx_lv2_open(&so, "/nonexistent/x.so", LV2_FEATURE_PROBE_URI, &why) == NULL && so == NULL && !strcmp(why, "dlopen"),
          "open: no binary refuses at dlopen");
    CHECK(omx_lv2_open(&so, g_so, "urn:openmixer:test:no-such", &why) == NULL && so == NULL && !strcmp(why, "uri"),
          "open: a URI the binary does not hold refuses at uri");
}

int main(int argc, char **argv)
{
    const char *why = NULL;
    void *so = NULL;

    if (argc < 2)
    {
        printf("FAIL usage: lv2_features_test <omx-feature-probe.so>\n");
        return 1;
    }
    g_so = argv[1];
    g_desc = omx_lv2_open(&so, g_so, LV2_FEATURE_PROBE_URI, &why);
    g_probe = so ? dlsym(so, "lv2_feature_probe") : NULL;
    if (!g_desc || !g_probe)
    {
        printf("FAIL the probe does not open (%s)\n", why ? why : "no probe symbol");
        return 1;
    }
    t_defaults();
    t_feature_set();
    t_strict();
    t_block();
    t_log();
    t_build();
    t_urid();
    t_rtmempool();
    t_open();
    dlclose(so);
    if (g_failures)
    {
        printf("lv2 features test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("lv2 features test ok\n");
    return 0;
}
