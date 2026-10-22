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
 * The chain of libomx-clap-core (clap_chain.h, docs/chain.md, #311), by value: the fake plugins of this tree
 * (tests/fake.clap's passthrough and sidechain, tests/fake_compressor.clap's compressor) and stages of the test's own
 * (a gain, a one-pole low-pass and a delay that reports its length as its latency), each set of members built twice,
 * fresh, so one runs as a chain and the other in series, each member into a buffer of its own as one client each would.
 *
 *   clap_chain_test <fake.clap> <fake_compressor.clap>
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#include "../src/clap_chain.h"
#include "test_util.h"

#define COMPRESSOR      "org.omx-clap-host.test.compressor"
#define CH              2u
#define FRAMES          OMX_CLAP_CHAIN_FRAMES_MAX
#define DELAY_MAX       64u
#define SETTLE_US       50000
#define SET_MAX         8u
#define LATENCY_PASS    64u     // the passthrough's default
#define LATENCY_PASS_B  100u    // the passthrough whose parameter 0 is written before it is published
#define LATENCY_NEXT    128u    // the passthrough's after a write of its parameter 0

/* what a member of a set is */
enum kind { K_PASS, K_PASS_B, K_SIDECHAIN, K_COMP, K_GAIN, K_LOWPASS, K_DELAY };

struct spec
{
    enum kind kind;
    float value;                // the gain, the low-pass coefficient, the delay in frames
};

/* a stage of the test's own, and what it saw */
struct stage
{
    enum kind kind;
    float value;
    float z[CH];                // the low-pass's state
    float ring[CH][DELAY_MAX];  // the delay's
    uint32_t pos;
    uint32_t calls;
    uint32_t aliased;           // calls handed an input that is its output
    const float *const *last_in;
    float *const *last_out;
};

struct member
{
    struct spec spec;
    struct omx_clap_instance *instance;
    struct stage stage;
    float *out[CH];             // the member's own output in series
};

struct set
{
    uint32_t n;
    struct member m[SET_MAX];
};

static const char *g_fake, *g_comp;

/* the latencies latency_changed was told */
struct told
{
    uint32_t calls;
    uint32_t last;
};

static void on_latency(void *ctx, uint32_t frames)
{
    struct told *told = ctx;

    told->calls++;
    told->last = frames;
}

static void stage_process(void *ctx, const float *const *in, float *const *out, uint32_t n)
{
    struct stage *s = ctx;
    uint32_t c, i;

    s->calls++;
    s->last_in = in;
    s->last_out = out;
    for (c = 0; c < CH; c++)
    {
        if ((const float *)out[c] == in[c])
            s->aliased++;
        for (i = 0; i < n; i++)
        {
            const float x = in[c][i];

            switch (s->kind)
            {
            case K_GAIN:
                out[c][i] = x * s->value;
                break;
            case K_LOWPASS:
                s->z[c] += s->value * (x - s->z[c]);
                out[c][i] = s->z[c];
                break;
            default:    // K_DELAY: each channel's ring advances together, so the position moves once per frame
                out[c][i] = s->ring[c][(s->pos + i) % (uint32_t)s->value];
                s->ring[c][(s->pos + i) % (uint32_t)s->value] = x;
                break;
            }
        }
    }
    if (s->kind == K_DELAY)
        s->pos = (s->pos + n) % (uint32_t)s->value;
}

static float *block_new(void)
{
    void *p = NULL;

    if (posix_memalign(&p, OMX_CLAP_CHAIN_ALIGN, (FRAMES + 64) * sizeof(float)) != 0)
        abort();
    memset(p, 0, (FRAMES + 64) * sizeof(float));
    return p;
}

/* the passthrough's latency parameter written and the restart it asks for run, the instance not published */
static void latency_to(struct omx_clap_instance *in, uint32_t frames)
{
    omx_clap_host_param_write(in, 0, frames);
    omx_clap_host_param_deliver(in);
    if (omx_clap_host_tick(in) && in->active)
        omx_clap_host_restart(in);
    omx_clap_host_settle(in, SETTLE_US);
}

static struct omx_clap_instance *instance_new(enum kind kind, int publish)
{
    struct omx_clap_instance *in = NULL;
    char why[OMX_CLAP_WHY_MAX] = "";
    const char *path = kind == K_COMP ? g_comp : g_fake;
    const char *id = kind == K_COMP ? COMPRESSOR : kind == K_SIDECHAIN ? FAKE_SIDECHAIN : FAKE_PASSTHROUGH;

    if (omx_clap_host_open(path, id, &in, why) != 0 || !in || omx_clap_host_activate(in, SAMPLE_RATE, FRAMES, why) != 0)
    {
        printf("FAIL %s cannot be opened and activated: %s\n", id, why);
        exit(1);
    }
    if (kind == K_PASS_B)
        latency_to(in, LATENCY_PASS_B);
    if (publish)
        omx_clap_host_publish(in, pthread_self());
    return in;
}

/* the members of `specs` but the ones `skip` has a bit for, each fresh */
static void set_new(struct set *set, const struct spec *specs, uint32_t n, uint32_t skip)
{
    uint32_t i, c;

    memset(set, 0, sizeof(*set));
    for (i = 0; i < n; i++)
    {
        struct member *m;

        if (skip & (1u << i))
            continue;
        m = &set->m[set->n++];
        m->spec = specs[i];
        if (specs[i].kind <= K_COMP)
            m->instance = instance_new(specs[i].kind, 1);
        else
        {
            m->stage.kind = specs[i].kind;
            m->stage.value = specs[i].value;
        }
        for (c = 0; c < CH; c++)
            m->out[c] = block_new();
    }
}

static void set_free(struct set *set)
{
    uint32_t i, c;

    for (i = 0; i < set->n; i++)
    {
        if (set->m[i].instance)
            omx_clap_host_close(set->m[i].instance);
        for (c = 0; c < CH; c++)
            free(set->m[i].out[c]);
    }
    set->n = 0;
}

static void member_desc(struct member *m, struct omx_clap_chain_member *d)
{
    omx_clap_chain_member_init(d);
    if (m->instance)
        d->instance = m->instance;
    else
    {
        d->process = stage_process;
        d->ctx = &m->stage;
        d->in_channels = d->out_channels = CH;
        d->latency = m->spec.kind == K_DELAY ? (uint32_t)m->spec.value : 0u;
    }
}

/* a chain of the set's members in order, the ones `bypassed` has a bit for added bypassed; their ids in `ids` */
static struct omx_clap_chain *chain_of(struct set *set, uint32_t bypassed, uint32_t *ids, struct told *told)
{
    struct omx_clap_chain_config config;
    struct omx_clap_chain *chain = NULL;
    uint32_t i, id;

    omx_clap_chain_config_init(&config);
    config.channels = CH;
    config.latency_changed = told ? on_latency : NULL;
    config.ctx = told;
    if (omx_clap_chain_create(&config, &chain) != OMX_CLAP_CHAIN_OK)
    {
        printf("FAIL a chain of %u channels cannot be created\n", CH);
        exit(1);
    }
    for (i = 0; i < set->n; i++)
    {
        struct omx_clap_chain_member d;

        member_desc(&set->m[i], &d);
        d.bypass = (bypassed >> i) & 1u;
        if (omx_clap_chain_add(chain, &d, OMX_CLAP_CHAIN_END, &id) != OMX_CLAP_CHAIN_OK)
        {
            printf("FAIL member %u cannot be added\n", i);
            exit(1);
        }
        if (ids)
            ids[i] = id;
    }
    return chain;
}

/* the set in series: each member from the one before it into its own buffer, the last one's copied to `out` */
static void series(struct set *set, const float *const *in, float *const *out, uint32_t n)
{
    const float *const *from = in;
    uint32_t i, c;

    for (i = 0; i < set->n; i++)
    {
        struct member *m = &set->m[i];

        if (m->instance)
            omx_clap_host_run_ports(m->instance, from, m->out, n);
        else
            stage_process(&m->stage, from, m->out, n);
        from = (const float *const *)m->out;
    }
    for (c = 0; c < CH; c++)
        memcpy(out[c], from[c], n * sizeof(float));
}

/* block `b` of the input: deterministic noise, its level stepping so the compressor acts on some blocks only */
static void input_block(float *const *in, uint32_t n, uint32_t b)
{
    static const float level[] = { 0.05f, 0.3f, 0.02f, 0.5f, 0.033f };
    static uint32_t seed = 12345u;
    uint32_t c, i;

    for (i = 0; i < n; i++)
        for (c = 0; c < CH; c++)
        {
            seed = seed * 1664525u + 1013904223u;
            in[c][i] = level[b % 5u] * ((float)(seed >> 8) / 8388608.0f - 1.0f);
        }
}

static const uint32_t g_sizes[] = { 256, 1, 64, 333, 4096, FRAMES, 128, 256, 7, 512 };
#define SIZES (sizeof(g_sizes) / sizeof(g_sizes[0]))

struct io
{
    float *in[CH], *a[CH], *b[CH];
};

static void io_new(struct io *io)
{
    uint32_t c;

    for (c = 0; c < CH; c++)
    {
        io->in[c] = block_new();
        io->a[c] = block_new();
        io->b[c] = block_new();
    }
}

static void io_free(struct io *io)
{
    uint32_t c;

    for (c = 0; c < CH; c++)
    {
        free(io->in[c]);
        free(io->a[c]);
        free(io->b[c]);
    }
}

static int same(float *const *a, float *const *b, uint32_t n)
{
    uint32_t c;

    for (c = 0; c < CH; c++)
        if (memcmp(a[c], b[c], n * sizeof(float)) != 0)
            return 0;
    return 1;
}

static struct omx_clap_chain_report report_of(struct omx_clap_chain *chain)
{
    struct omx_clap_chain_report r;

    omx_clap_chain_report_init(&r);
    if (omx_clap_chain_report(chain, &r) != OMX_CLAP_CHAIN_OK)
        printf("FAIL the report is refused\n");
    return r;
}

/* the fixture the identity, the bypass and the latency are read on: every kind of member, stateful ones among them */
static const struct spec g_ident[] = {
    { K_GAIN, 3.0f }, { K_COMP, 0 }, { K_LOWPASS, 0.25f }, { K_PASS, 0 }, { K_DELAY, 10 }, { K_COMP, 0 }, { K_SIDECHAIN, 0 },
};
#define IDENT_N 7u
// the passthrough's and the sidechain's 64 frames each (every effect of fake.clap reports its parameter 0) and the delay's 10
#define IDENT_LATENCY (2u * LATENCY_PASS + 10u)

/* a chain's output equals its members in series, bit for bit, over blocks of every size up to the blocks' */
static void t_identity(void)
{
    struct set a, b;
    struct io io;
    struct omx_clap_chain *chain;
    struct omx_clap_chain_report r;
    uint32_t k, ok = 0, equal = 0, moved = 0, runs[2], total = 0;

    set_new(&a, g_ident, IDENT_N, 0);
    set_new(&b, g_ident, IDENT_N, 0);
    io_new(&io);
    chain = chain_of(&a, 0, NULL, NULL);
    for (k = 0; k < 3u * SIZES; k++)
    {
        const uint32_t n = g_sizes[k % SIZES];

        input_block(io.in, n, k);
        ok += omx_clap_chain_process(chain, (const float *const *)io.in, io.a, n) == OMX_CLAP_CHAIN_OK;
        series(&b, (const float *const *)io.in, io.b, n);
        equal += same(io.a, io.b, n);
        moved += memcmp(io.a[0], io.in[0], n * sizeof(float)) != 0;
        total++;
    }
    CHECK(equal == total, "a chain of %u members (gain, compressor, low-pass, passthrough, delay, compressor, sidechain) equals them in series bit for bit: %u of %u blocks of 1 to %u frames",
          IDENT_N, equal, total, FRAMES);
    runs[0] = atomic_load(&a.m[1].instance->stage.h.runs);
    runs[1] = atomic_load(&a.m[6].instance->stage.h.runs);
    CHECK(ok == total && moved == total && runs[0] == total && runs[1] == total,
          "and the members acted: every cycle OMX_CLAP_CHAIN_OK (%u), every block differs from the input (%u of %u), the first compressor and the sidechain ran every cycle (%u, %u)",
          ok, moved, total, runs[0], runs[1]);
    r = report_of(chain);
    CHECK(r.members == IDENT_N && r.running == IDENT_N && r.channels == CH && r.latency == IDENT_LATENCY,
          "the report: %u members, %u running, %u channels, %u frames of latency (passthrough %u, sidechain %u, delay 10)", r.members, r.running, r.channels,
          r.latency, omx_clap_host_latency(a.m[3].instance), omx_clap_host_latency(a.m[6].instance));
    CHECK(a.m[2].stage.aliased == 0 && a.m[4].stage.aliased == 0, "no member is handed its input as its output (%u, %u)", a.m[2].stage.aliased, a.m[4].stage.aliased);
    omx_clap_chain_destroy(chain);
    set_free(&a);
    set_free(&b);

    // the oracle sees order: the same two members the other way round are not the chain
    {
        static const struct spec gc[] = { { K_GAIN, 3.0f }, { K_COMP, 0 } }, cg[] = { { K_COMP, 0 }, { K_GAIN, 3.0f } };
        uint32_t differ = 0;

        set_new(&a, gc, 2, 0);
        set_new(&b, cg, 2, 0);
        chain = chain_of(&a, 0, NULL, NULL);
        for (k = 0; k < SIZES; k++)
        {
            input_block(io.in, g_sizes[k], k);
            omx_clap_chain_process(chain, (const float *const *)io.in, io.a, g_sizes[k]);
            series(&b, (const float *const *)io.in, io.b, g_sizes[k]);
            differ += !same(io.a, io.b, g_sizes[k]);
        }
        CHECK(differ > 0, "the comparison sees order: gain then compressor differs from compressor then gain on %u of %u blocks", differ, (unsigned)SIZES);
        omx_clap_chain_destroy(chain);
        set_free(&a);
        set_free(&b);
    }

    // a chain of one member hands it the caller's own buffers and equals it alone
    {
        static const struct spec one[] = { { K_COMP, 0 } }, lp[] = { { K_LOWPASS, 0.5f } };
        uint32_t eq = 0;

        set_new(&a, one, 1, 0);
        set_new(&b, one, 1, 0);
        chain = chain_of(&a, 0, NULL, NULL);
        for (k = 0; k < SIZES; k++)
        {
            input_block(io.in, g_sizes[k], k);
            omx_clap_chain_process(chain, (const float *const *)io.in, io.a, g_sizes[k]);
            omx_clap_host_run_ports(b.m[0].instance, (const float *const *)io.in, io.b, g_sizes[k]);
            eq += same(io.a, io.b, g_sizes[k]);
        }
        CHECK(eq == SIZES, "a chain of one compressor equals the compressor alone, bit for bit: %u of %u blocks", eq, (unsigned)SIZES);
        omx_clap_chain_destroy(chain);
        set_free(&a);
        set_free(&b);
        set_new(&a, lp, 1, 0);
        chain = chain_of(&a, 0, NULL, NULL);
        omx_clap_chain_process(chain, (const float *const *)io.in, io.a, 64);
        CHECK(a.m[0].stage.last_in == (const float *const *)io.in && a.m[0].stage.last_out == io.a,
              "and is handed the caller's own input and output, no block between");
        omx_clap_chain_destroy(chain);
        set_free(&a);
    }
    io_free(&io);
}

/* the latency is the non-bypassed members' sum, told on every change and only on a change */
static void t_latency(void)
{
    static const struct spec lat[] = { { K_PASS, 0 }, { K_PASS_B, 0 }, { K_DELAY, 10 } };
    struct set a;
    struct told told = { 0, 0 };
    struct omx_clap_chain *chain;
    struct omx_clap_chain_member d;
    struct omx_clap_chain_report r;
    struct omx_clap_instance *pass;
    uint32_t ids[3], id;
    double value;

    set_new(&a, lat, 3, 0);
    CHECK(omx_clap_host_latency(a.m[0].instance) == LATENCY_PASS && omx_clap_host_latency(a.m[1].instance) == LATENCY_PASS_B,
          "the two passthroughs report %u and %u frames", omx_clap_host_latency(a.m[0].instance), omx_clap_host_latency(a.m[1].instance));
    chain = chain_of(&a, 0, ids, &told);
    r = report_of(chain);
    CHECK(r.latency == LATENCY_PASS + LATENCY_PASS_B + 10u && told.last == r.latency && told.calls == 3,
          "the chain's latency is the sum, %u + %u + 10 = %u frames, told at each of the 3 adds (%u, last %u)", LATENCY_PASS, LATENCY_PASS_B, r.latency,
          told.calls, told.last);

    CHECK(omx_clap_chain_member_latency_set(chain, ids[2], 20) == OMX_CLAP_CHAIN_OK && report_of(chain).latency == LATENCY_PASS + LATENCY_PASS_B + 20u
          && told.calls == 4 && told.last == LATENCY_PASS + LATENCY_PASS_B + 20u,
          "a stage's latency stated again, 20 frames: the sum %u, told (%u calls)", told.last, told.calls);
    CHECK(omx_clap_chain_member_latency_set(chain, ids[2], 20) == OMX_CLAP_CHAIN_OK && told.calls == 4,
          "the same latency stated again tells nothing (%u calls)", told.calls);
    CHECK(omx_clap_chain_member_latency_set(chain, ids[0], 5) == OMX_CLAP_CHAIN_ERR_ARG, "an instance's latency is not stated: it is read");
    CHECK(omx_clap_chain_member_latency_set(chain, 999, 5) == OMX_CLAP_CHAIN_ERR_NO_MEMBER, "a member that is not there is no member");

    // a bypassed member counts nothing, and counts again when it runs again
    CHECK(omx_clap_chain_bypass(chain, ids[1], 1) == OMX_CLAP_CHAIN_OK && report_of(chain).latency == LATENCY_PASS + 20u && told.last == LATENCY_PASS + 20u
          && report_of(chain).running == 2,
          "the passthrough of %u frames bypassed: the sum %u, %u running", LATENCY_PASS_B, told.last, report_of(chain).running);
    CHECK(omx_clap_chain_bypass(chain, ids[1], 0) == OMX_CLAP_CHAIN_OK && told.last == LATENCY_PASS + LATENCY_PASS_B + 20u,
          "and running again: %u", told.last);
    CHECK(omx_clap_chain_remove(chain, ids[0]) == OMX_CLAP_CHAIN_OK && told.last == LATENCY_PASS_B + 20u && report_of(chain).members == 2,
          "a member removed takes its latency with it: %u", told.last);

    // an instance whose latency changes (a write of the passthrough's parameter 0 and the restart it asks for), read
    // again by refresh: not published, so no cycle runs it while it restarts
    pass = instance_new(K_PASS, 0);
    omx_clap_chain_member_init(&d);
    d.instance = pass;
    CHECK(omx_clap_chain_add(chain, &d, 0, &id) == OMX_CLAP_CHAIN_OK && told.last == LATENCY_PASS_B + 20u + LATENCY_PASS,
          "an instance added at the head: %u", told.last);
    latency_to(pass, LATENCY_NEXT);
    CHECK(omx_clap_host_param_read(pass, 0, &value) == 0 && value == LATENCY_NEXT && omx_clap_host_latency(pass) == LATENCY_NEXT,
          "the instance restarted at %u frames", omx_clap_host_latency(pass));
    CHECK(report_of(chain).latency == LATENCY_PASS_B + 20u + LATENCY_PASS, "the chain has not read it yet: %u", report_of(chain).latency);
    CHECK(omx_clap_chain_refresh(chain) == OMX_CLAP_CHAIN_OK && told.last == LATENCY_PASS_B + 20u + LATENCY_NEXT
          && report_of(chain).latency == told.last,
          "refresh reads it and tells the new sum, %u", told.last);
    omx_clap_chain_destroy(chain);
    omx_clap_host_close(pass);
    set_free(&a);
}

/* a bypassed member adds nothing: no call, no latency, and the output of a chain built without it */
static void t_bypass(void)
{
    struct set a, b;
    struct io io;
    struct omx_clap_chain *with, *without;
    uint32_t k, eq = 0;
    // the low-pass and the delay, stateful both, bypassed
    const uint32_t skip = (1u << 2) | (1u << 4);

    set_new(&a, g_ident, IDENT_N, 0);
    set_new(&b, g_ident, IDENT_N, skip);
    io_new(&io);
    with = chain_of(&a, skip, NULL, NULL);
    without = chain_of(&b, 0, NULL, NULL);
    for (k = 0; k < 2u * SIZES; k++)
    {
        const uint32_t n = g_sizes[k % SIZES];

        input_block(io.in, n, k);
        omx_clap_chain_process(with, (const float *const *)io.in, io.a, n);
        omx_clap_chain_process(without, (const float *const *)io.in, io.b, n);
        eq += same(io.a, io.b, n);
    }
    CHECK(eq == 2u * SIZES, "the chain with the low-pass and the delay bypassed equals the chain built without them, bit for bit: %u of %u blocks", eq,
          2u * (unsigned)SIZES);
    CHECK(a.m[2].stage.calls == 0 && a.m[4].stage.calls == 0, "the bypassed members are never called (%u, %u)", a.m[2].stage.calls, a.m[4].stage.calls);
    CHECK(report_of(with).latency == report_of(without).latency && report_of(with).latency == IDENT_LATENCY - 10u && report_of(with).running == IDENT_N - 2u,
          "and add no latency: %u frames, as the chain without them (%u), %u of %u running", report_of(with).latency, report_of(without).latency,
          report_of(with).running, report_of(with).members);
    omx_clap_chain_destroy(with);
    omx_clap_chain_destroy(without);
    set_free(&a);
    set_free(&b);
    io_free(&io);
}

/* the removal's fixture: the member taken out (the low-pass, 2) has only memoryless members after it, so a chain built
 * without it, fed the same input from the start, holds the same state in every member the removal keeps */
static const struct spec g_remove[] = {
    { K_DELAY, 10 }, { K_GAIN, 3.0f }, { K_LOWPASS, 0.25f }, { K_COMP, 0 }, { K_PASS, 0 }, { K_COMP, 0 }, { K_SIDECHAIN, 0 },
};
#define REMOVE_N 7u
#define REMOVED 2u

/* a member removed, or bypassed while the cycles run: the chain closes around it, and the cycle after the call equals a
 * chain built without it, bit for bit */
static void t_remove(int by_bypass)
{
    struct set a, c;
    struct io io;
    struct omx_clap_chain *chain, *without;
    uint32_t ids[REMOVE_N], k, before = 0, after = 0, calls;
    const char *what = by_bypass ? "bypassed" : "removed";

    set_new(&a, g_remove, REMOVE_N, 0);
    set_new(&c, g_remove, REMOVE_N, 1u << REMOVED);
    io_new(&io);
    chain = chain_of(&a, 0, ids, NULL);
    without = chain_of(&c, 0, NULL, NULL);
    for (k = 0; k < SIZES; k++)
    {
        input_block(io.in, g_sizes[k], k);
        omx_clap_chain_process(chain, (const float *const *)io.in, io.a, g_sizes[k]);
        omx_clap_chain_process(without, (const float *const *)io.in, io.b, g_sizes[k]);
        before += !same(io.a, io.b, g_sizes[k]);
    }
    CHECK(before == SIZES, "before it is %s the low-pass moves every block (%u of %u differ from the chain without it)", what, before, (unsigned)SIZES);
    if (by_bypass)
        CHECK(omx_clap_chain_bypass(chain, ids[REMOVED], 1) == OMX_CLAP_CHAIN_OK, "the low-pass bypassed while the chain runs");
    else
        CHECK(omx_clap_chain_remove(chain, ids[REMOVED]) == OMX_CLAP_CHAIN_OK, "the low-pass removed while the chain runs");
    calls = a.m[REMOVED].stage.calls;
    for (k = 0; k < 2u * SIZES; k++)
    {
        const uint32_t n = g_sizes[k % SIZES];

        input_block(io.in, n, k);
        omx_clap_chain_process(chain, (const float *const *)io.in, io.a, n);
        omx_clap_chain_process(without, (const float *const *)io.in, io.b, n);
        after += same(io.a, io.b, n);
    }
    CHECK(after == 2u * SIZES, "from the cycle after it is %s the chain equals the chain built without it, bit for bit: %u of %u blocks", what, after,
          2u * (unsigned)SIZES);
    CHECK(a.m[REMOVED].stage.calls == calls, "and the %s member is not called again (%u calls)", what, a.m[REMOVED].stage.calls);
    CHECK(report_of(chain).running == REMOVE_N - 1u && report_of(chain).members == (by_bypass ? REMOVE_N : REMOVE_N - 1u),
          "the report: %u members, %u running", report_of(chain).members, report_of(chain).running);
    omx_clap_chain_destroy(chain);
    omx_clap_chain_destroy(without);
    set_free(&a);
    set_free(&c);
    io_free(&io);
}

/* the head and the tail taken out, then the last one: the chain closes at either end, and an empty chain passes dry */
static void t_ends(void)
{
    static const struct spec g3[] = { { K_GAIN, 2.0f }, { K_GAIN, 3.0f }, { K_GAIN, 5.0f } };
    struct set a, b;
    struct io io;
    struct omx_clap_chain *chain;
    uint32_t ids[3];

    io_new(&io);
    set_new(&a, g3, 3, 0);
    chain = chain_of(&a, 0, ids, NULL);
    input_block(io.in, 256, 1);

    set_new(&b, g3, 3, 1u << 0);
    CHECK(omx_clap_chain_remove(chain, ids[0]) == OMX_CLAP_CHAIN_OK, "the head removed");
    omx_clap_chain_process(chain, (const float *const *)io.in, io.a, 256);
    series(&b, (const float *const *)io.in, io.b, 256);
    CHECK(same(io.a, io.b, 256) && io.a[0][5] == (io.in[0][5] * 3.0f) * 5.0f, "the chain is x 3 then x 5: %g from %g", (double)io.a[0][5], (double)io.in[0][5]);
    set_free(&b);

    set_new(&b, g3, 3, (1u << 0) | (1u << 2));
    CHECK(omx_clap_chain_remove(chain, ids[2]) == OMX_CLAP_CHAIN_OK, "the tail removed");
    omx_clap_chain_process(chain, (const float *const *)io.in, io.a, 256);
    series(&b, (const float *const *)io.in, io.b, 256);
    CHECK(same(io.a, io.b, 256) && io.a[1][7] == io.in[1][7] * 3.0f, "the chain is x 3: %g from %g", (double)io.a[1][7], (double)io.in[1][7]);
    set_free(&b);

    CHECK(omx_clap_chain_remove(chain, ids[1]) == OMX_CLAP_CHAIN_OK && report_of(chain).members == 0, "the last member removed");
    omx_clap_chain_process(chain, (const float *const *)io.in, io.a, 256);
    CHECK(same(io.a, io.in, 256), "an empty chain copies its input to its output");
    CHECK(omx_clap_chain_process(chain, (const float *const *)io.in, io.in, 256) == OMX_CLAP_CHAIN_OK && same(io.a, io.in, 256),
          "and leaves it as it is in place");
    CHECK(omx_clap_chain_remove(chain, ids[1]) == OMX_CLAP_CHAIN_ERR_NO_MEMBER, "a member removed twice is no member the second time");
    omx_clap_chain_destroy(chain);
    set_free(&a);
    io_free(&io);
}

/* a cycle longer than the blocks passes: the input copied dry, no member run, said and counted */
static void t_long_cycle(void)
{
    static const struct spec g1[] = { { K_GAIN, 2.0f }, { K_GAIN, 0.5f } };
    struct set a;
    struct io io;
    struct omx_clap_chain *chain;
    uint32_t calls;
    int ret;

    io_new(&io);
    set_new(&a, g1, 2, 0);
    chain = chain_of(&a, 0, NULL, NULL);
    input_block(io.in, FRAMES + 1, 3);
    ret = omx_clap_chain_process(chain, (const float *const *)io.in, io.a, FRAMES);
    CHECK(ret == OMX_CLAP_CHAIN_OK && a.m[0].stage.calls == 1 && io.a[0][FRAMES - 1] == (io.in[0][FRAMES - 1] * 2.0f) * 0.5f,
          "a cycle of %u frames, the blocks' length, runs the members (%d)", FRAMES, ret);
    calls = a.m[0].stage.calls;
    memset(io.a[0], 0, (FRAMES + 1) * sizeof(float));
    memset(io.a[1], 0, (FRAMES + 1) * sizeof(float));
    io.in[0][FRAMES] = 0.123f;
    ret = omx_clap_chain_process(chain, (const float *const *)io.in, io.a, FRAMES + 1);
    CHECK(ret == OMX_CLAP_CHAIN_PASSED, "a cycle of %u frames passes: OMX_CLAP_CHAIN_PASSED (%d)", FRAMES + 1, ret);
    CHECK(same(io.a, io.in, FRAMES + 1) && a.m[0].stage.calls == calls && a.m[1].stage.calls == calls,
          "its input is its output, dry, every one of the %u frames, and no member ran (%u calls)", FRAMES + 1, a.m[0].stage.calls);
    CHECK(report_of(chain).passed == 1, "the report counts it: %llu", (unsigned long long)report_of(chain).passed);
    omx_clap_chain_destroy(chain);
    set_free(&a);
    io_free(&io);
}

/* every structure of another ABI or size is refused, nothing read and nothing written */
static void t_abi(void)
{
    struct omx_clap_chain_config config;
    struct omx_clap_chain_member d;
    struct omx_clap_chain_report r, canary_r;
    struct omx_clap_chain_member_info info, canary_i;
    struct omx_clap_chain *chain = (struct omx_clap_chain *)&config, *ok = NULL;
    struct stage st = { K_GAIN, 1.0f, { 0 }, { { 0 } }, 0, 0, 0, NULL, NULL };
    static const int32_t abi_delta[] = { 1, -1 };
    static const int32_t size_delta[] = { -4, 4, -1 };
    uint32_t i, refused = 0, tries = 0, untouched = 0;

    omx_clap_chain_config_init(&config);
    config.channels = CH;
    CHECK(config.abi == OMX_CLAP_CHAIN_ABI && config.size == sizeof(config), "the configuration says its ABI and its size (%u, %u)", config.abi, config.size);
    for (i = 0; i < 5; i++)
    {
        struct omx_clap_chain_config bad = config;

        if (i < 2)
            bad.abi = (uint32_t)((int32_t)config.abi + abi_delta[i]);
        else
            bad.size = (uint32_t)((int32_t)config.size + size_delta[i - 2]);
        chain = (struct omx_clap_chain *)&config;
        refused += omx_clap_chain_create(&bad, &chain) == OMX_CLAP_CHAIN_ERR_ABI && chain == NULL;
        tries++;
    }
    CHECK(refused == tries, "a configuration of another ABI (newer, 0) or size (smaller, larger, odd) is refused: %u of %u", refused, tries);
    CHECK(omx_clap_chain_create(&config, &ok) == OMX_CLAP_CHAIN_OK && ok, "this header's is created");

    refused = tries = 0;
    for (i = 0; i < 5; i++)
    {
        omx_clap_chain_member_init(&d);
        d.process = stage_process;
        d.ctx = &st;
        d.in_channels = d.out_channels = CH;
        if (i < 2)
            d.abi = (uint32_t)((int32_t)d.abi + abi_delta[i]);
        else
            d.size = (uint32_t)((int32_t)d.size + size_delta[i - 2]);
        refused += omx_clap_chain_add(ok, &d, OMX_CLAP_CHAIN_END, NULL) == OMX_CLAP_CHAIN_ERR_ABI;
        tries++;
    }
    CHECK(refused == tries && report_of(ok).members == 0, "a member of another ABI or size is refused, nothing added: %u of %u, %u members", refused, tries,
          report_of(ok).members);
    omx_clap_chain_member_init(&d);
    d.process = stage_process;
    d.ctx = &st;
    d.in_channels = d.out_channels = CH;
    CHECK(omx_clap_chain_add(ok, &d, OMX_CLAP_CHAIN_END, NULL) == OMX_CLAP_CHAIN_OK, "this header's is added");

    refused = tries = 0;
    for (i = 0; i < 5; i++)
    {
        memset(&r, 0xa5, sizeof(r));
        omx_clap_chain_report_init(&r);
        if (i < 2)
            r.abi = (uint32_t)((int32_t)r.abi + abi_delta[i]);
        else
            r.size = (uint32_t)((int32_t)r.size + size_delta[i - 2]);
        memset((char *)&r + 2 * sizeof(uint32_t), 0xa5, sizeof(r) - 2 * sizeof(uint32_t));
        canary_r = r;
        refused += omx_clap_chain_report(ok, &r) == OMX_CLAP_CHAIN_ERR_ABI;
        untouched += memcmp(&r, &canary_r, sizeof(r)) == 0;
        memset(&info, 0xa5, sizeof(info));
        omx_clap_chain_member_info_init(&info);
        if (i < 2)
            info.abi = (uint32_t)((int32_t)info.abi + abi_delta[i]);
        else
            info.size = (uint32_t)((int32_t)info.size + size_delta[i - 2]);
        memset((char *)&info + 2 * sizeof(uint32_t), 0xa5, sizeof(info) - 2 * sizeof(uint32_t));
        canary_i = info;
        refused += omx_clap_chain_member_info(ok, 0, &info) == OMX_CLAP_CHAIN_ERR_ABI;
        untouched += memcmp(&info, &canary_i, sizeof(info)) == 0;
        tries += 2;
    }
    CHECK(refused == tries && untouched == tries, "a report or a member's info of another ABI or size is refused and not written to: %u of %u refused, %u untouched",
          refused, tries, untouched);
    omx_clap_chain_member_info_init(&info);
    CHECK(omx_clap_chain_member_info(ok, 0, &info) == OMX_CLAP_CHAIN_OK && info.id != 0 && info.ctx == &st && info.in_channels == CH && info.out_channels == CH
          && info.position == 0 && info.instance == NULL && info.bypass == 0,
          "this header's info is written: id %u, position %u, %u in, %u out", info.id, info.position, info.in_channels, info.out_channels);
    omx_clap_chain_destroy(ok);
}

/* what the chain refuses besides the ABI */
static void t_refusals(void)
{
    struct omx_clap_chain_config config;
    struct omx_clap_chain_member d;
    struct omx_clap_chain *chain = NULL;
    struct stage st = { K_GAIN, 1.0f, { 0 }, { { 0 } }, 0, 0, 0, NULL, NULL };
    struct omx_clap_instance *wide = NULL;
    char why[OMX_CLAP_WHY_MAX];
    uint32_t i, added = 0;

    omx_clap_chain_config_init(&config);
    config.channels = 0;
    CHECK(omx_clap_chain_create(&config, &chain) == OMX_CLAP_CHAIN_ERR_WIDTH && !chain, "a chain of no channel is refused");
    config.channels = CLAP_HOST_PORT_CHANNELS + 1u;
    CHECK(omx_clap_chain_create(&config, &chain) == OMX_CLAP_CHAIN_ERR_WIDTH && !chain, "and one of %u", CLAP_HOST_PORT_CHANNELS + 1u);
    config.channels = CH;
    CHECK(omx_clap_chain_create(&config, &chain) == OMX_CLAP_CHAIN_OK, "a chain of %u channels", CH);

    omx_clap_chain_member_init(&d);
    CHECK(omx_clap_chain_add(chain, &d, OMX_CLAP_CHAIN_END, NULL) == OMX_CLAP_CHAIN_ERR_ARG, "a member that is neither an instance nor a stage is refused");
    d.process = stage_process;
    d.ctx = &st;
    d.instance = (struct omx_clap_instance *)&st;
    CHECK(omx_clap_chain_add(chain, &d, OMX_CLAP_CHAIN_END, NULL) == OMX_CLAP_CHAIN_ERR_ARG, "and one that is both");
    d.instance = NULL;
    d.in_channels = 1;
    d.out_channels = CH;
    CHECK(omx_clap_chain_add(chain, &d, OMX_CLAP_CHAIN_END, NULL) == OMX_CLAP_CHAIN_ERR_WIDTH, "a stage of one channel in, two out is refused");
    d.in_channels = CH;
    d.out_channels = 1;
    CHECK(omx_clap_chain_add(chain, &d, OMX_CLAP_CHAIN_END, NULL) == OMX_CLAP_CHAIN_ERR_WIDTH, "and one of two in, one out");
    if (omx_clap_host_open(g_fake, FAKE_MONO_STEREO, &wide, why) == 0 && wide)
    {
        omx_clap_chain_member_init(&d);
        d.instance = wide;
        CHECK(omx_clap_chain_add(chain, &d, OMX_CLAP_CHAIN_END, NULL) == OMX_CLAP_CHAIN_ERR_WIDTH,
              "an instance of %u channel in, %u out is refused by its own widths", wide->main_in.channels, wide->main_out.channels);
        omx_clap_host_close(wide);
    }
    else
        CHECK(0, "%s opens: %s", FAKE_MONO_STEREO, why);
    omx_clap_chain_member_init(&d);
    d.process = stage_process;
    d.ctx = &st;
    d.in_channels = d.out_channels = CH;
    CHECK(omx_clap_chain_add(chain, &d, 1, NULL) == OMX_CLAP_CHAIN_ERR_ARG, "a position past the end is refused");
    for (i = 0; i < OMX_CLAP_CHAIN_MEMBERS_MAX + 1u; i++)
        added += omx_clap_chain_add(chain, &d, 0, NULL) == OMX_CLAP_CHAIN_OK;
    CHECK(added == OMX_CLAP_CHAIN_MEMBERS_MAX && omx_clap_chain_add(chain, &d, 0, NULL) == OMX_CLAP_CHAIN_ERR_FULL,
          "%u members are added and the next one is refused, full", added);
    CHECK(omx_clap_chain_bypass(chain, 999, 1) == OMX_CLAP_CHAIN_ERR_NO_MEMBER && omx_clap_chain_remove(chain, 0) == OMX_CLAP_CHAIN_ERR_NO_MEMBER,
          "a bypass or a removal of an id the chain never gave is no member");
    CHECK(omx_clap_chain_process(NULL, NULL, NULL, 64) == OMX_CLAP_CHAIN_ERR_ARG && omx_clap_chain_create(&config, NULL) == OMX_CLAP_CHAIN_ERR_ARG,
          "NULLs are refused");
    omx_clap_chain_destroy(chain);
}

/* the cycles run on a thread of their own while the control thread changes the chain: every block is one whole walk,
 * never half of the old one and half of the new, and no cycle waits */
struct rt
{
    struct omx_clap_chain *chain;
    _Atomic int stop;
    uint32_t blocks, torn, seen[4];
};

static void *rt_thread(void *arg)
{
    struct rt *rt = arg;
    static float in_l[64], in_r[64], out_l[64], out_r[64];
    const float *in[CH] = { in_l, in_r };
    float *out[CH] = { out_l, out_r };
    static const float values[4] = { 3.0f, 6.0f, 15.0f, 30.0f };
    uint32_t i, v;

    for (i = 0; i < 64; i++)
        in_l[i] = in_r[i] = 1.0f;
    while (!atomic_load(&rt->stop))
    {
        omx_clap_chain_process(rt->chain, in, out, 64);
        for (v = 0; v < 4; v++)
            if (out_l[0] == values[v])
                break;
        for (i = 0; i < 64; i++)
            if (out_l[i] != out_l[0] || out_r[i] != out_l[0])
                break;
        if (v == 4 || i < 64)
            rt->torn++;
        else
            rt->seen[v]++;
        rt->blocks++;
    }
    return NULL;
}

static void t_concurrent(void)
{
    static const struct spec g2[] = { { K_GAIN, 3.0f }, { K_GAIN, 2.0f } }, five = { K_GAIN, 5.0f };
    struct set a;
    struct member m5;
    struct omx_clap_chain_member d;
    struct rt rt;
    pthread_t thread;
    uint32_t ids[2], id5, i, changes = 0;

    memset(&rt, 0, sizeof(rt));
    set_new(&a, g2, 2, 0);
    rt.chain = chain_of(&a, 0, ids, NULL);
    memset(&m5, 0, sizeof(m5));
    m5.spec = five;
    m5.stage.kind = K_GAIN;
    m5.stage.value = 5.0f;
    member_desc(&m5, &d);
    pthread_create(&thread, NULL, rt_thread, &rt);
    for (i = 0; i < 400; i++)
    {
        changes += omx_clap_chain_bypass(rt.chain, ids[1], (int)(i & 1u)) == OMX_CLAP_CHAIN_OK;
        if (i % 4 == 0)
            changes += omx_clap_chain_add(rt.chain, &d, i % 8 ? 0 : OMX_CLAP_CHAIN_END, &id5) == OMX_CLAP_CHAIN_OK;
        else if (i % 4 == 2)
            changes += omx_clap_chain_remove(rt.chain, id5) == OMX_CLAP_CHAIN_OK;
        usleep(200);
    }
    atomic_store(&rt.stop, 1);
    pthread_join(thread, NULL);
    CHECK(changes == 600 && rt.torn == 0 && rt.blocks > 0, "%u changes while %u cycles ran: none torn (%u), each a whole walk", changes, rt.blocks, rt.torn);
    CHECK(rt.seen[0] && rt.seen[1] && rt.seen[2] && rt.seen[3], "and every walk was seen: x3 %u, x6 %u, x15 %u, x30 %u", rt.seen[0], rt.seen[1], rt.seen[2],
          rt.seen[3]);
    omx_clap_chain_destroy(rt.chain);
    set_free(&a);
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <fake.clap> <fake_compressor.clap>\n", argv[0]);
        return 2;
    }
    g_fake = argv[1];
    g_comp = argv[2];

    t_identity();
    t_latency();
    t_bypass();
    t_remove(0);
    t_remove(1);
    t_ends();
    t_long_cycle();
    t_abi();
    t_refusals();
    t_concurrent();
    CHECK(omx_clap_host_binaries_open() == 0, "every binary closed (%u open)", omx_clap_host_binaries_open());
    if (g_failures)
    {
        printf("FAIL chain test: %d failures\n", g_failures);
        return 1;
    }
    printf("chain test ok\n");
    return 0;
}
