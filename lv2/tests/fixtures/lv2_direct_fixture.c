/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/*
 * The plugins of the no-copy adapter's tests (docs/fast-path.md, item 1), one binary, all stereo, gain 0.5:
 *   (base)   states lv2:hardRTCapable: the plugin the adapter connects to the host's buffers
 *   #plain   states nothing: stays on the instance's memory
 *   #worker  lv2:hardRTCapable with a worker interface: stays on the instance's memory
 *   #broken  lv2:hardRTCapable and lv2:inPlaceBroken: clears its output first, so in place it reads silence
 *   #empty   lv2:hardRTCapable, a run() that returns at once: what the host adds, for tests/hosted_cost_bench.c
 */
#include <stdlib.h>
#include <string.h>

#include <lv2/core/lv2.h>
#include <lv2/worker/worker.h>

#include "lv2_direct_fixture.h"

struct lv2_direct_fixture_witness lv2_direct_fixture_witness;

struct twin { float *in[2], *out[2]; };

static LV2_Handle instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *f)
{
    (void)d, (void)rate, (void)path, (void)f;
    return calloc(1, sizeof(struct twin));
}

static void connect_port(LV2_Handle h, uint32_t port, void *data)
{
    struct twin *t = h;

    if (port < 2) t->in[port] = data; else if (port < 4) t->out[port - 2] = data;
    lv2_direct_fixture_witness.connects++;
}

static void see(const struct twin *t)
{
    struct lv2_direct_fixture_witness *w = &lv2_direct_fixture_witness;

    w->runs++;
    for (int c = 0; c < 2; c++)
        w->in[c] = t->in[c], w->out[c] = t->out[c];
    if (t->in[0] == t->out[0] || t->in[1] == t->out[1] || t->in[0] == t->out[1] || t->in[1] == t->out[0])
        w->aliased++;
}

static void run(LV2_Handle h, uint32_t n)
{
    struct twin *t = h;

    see(t);
    for (uint32_t c = 0; c < 2; c++)
        for (uint32_t i = 0; i < n; i++)
            t->out[c][i] = t->in[c][i] * 0.5f;
}

/* the output is cleared before the input is read: in place, nothing is left to read */
static void run_broken(LV2_Handle h, uint32_t n)
{
    struct twin *t = h;

    see(t);
    for (uint32_t c = 0; c < 2; c++)
    {
        memset(t->out[c], 0, n * sizeof(float));
        for (uint32_t i = 0; i < n; i++)
            t->out[c][i] += t->in[c][i] * 0.5f;
    }
}

static void run_empty(LV2_Handle h, uint32_t n) { (void)h, (void)n; }
static void deactivate(LV2_Handle h) { lv2_direct_fixture_witness.deactivate_in = ((struct twin *)h)->in[0]; }
static void cleanup(LV2_Handle h) { free(h); }

static LV2_Worker_Status work(LV2_Handle h, LV2_Worker_Respond_Function r, LV2_Worker_Respond_Handle rh, uint32_t n, const void *d)
{
    (void)h, (void)r, (void)rh, (void)n, (void)d;
    return LV2_WORKER_SUCCESS;
}
static LV2_Worker_Status work_response(LV2_Handle h, uint32_t n, const void *d) { (void)h, (void)n, (void)d; return LV2_WORKER_SUCCESS; }
static const void *worker_data(const char *uri)
{
    static const LV2_Worker_Interface W = {work, work_response, NULL};
    return strcmp(uri, LV2_WORKER__interface) == 0 ? &W : NULL;
}

static const LV2_Descriptor DESC[] = {
    {DIRECT_FIXTURE_URI, instantiate, connect_port, NULL, run, deactivate, cleanup, NULL},
    {DIRECT_FIXTURE_URI "#plain", instantiate, connect_port, NULL, run, deactivate, cleanup, NULL},
    {DIRECT_FIXTURE_URI "#worker", instantiate, connect_port, NULL, run, deactivate, cleanup, worker_data},
    {DIRECT_FIXTURE_URI "#broken", instantiate, connect_port, NULL, run_broken, deactivate, cleanup, NULL},
    {DIRECT_FIXTURE_URI "#empty", instantiate, connect_port, NULL, run_empty, deactivate, cleanup, NULL},
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t i)
{
    return i < sizeof DESC / sizeof DESC[0] ? &DESC[i] : NULL;
}
