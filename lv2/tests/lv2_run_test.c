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
 * lv2_run_test.c — the LV2 half of a block (lv2_instance_run) against closed forms, at every declared rate, with the
 * fakes of the console's mix_lv2.test.c racked from their bundle through lilv. Moved from that test with every number
 * it asserts unchanged; the arms whose body moved to the CLAP body (the mono mirror, the bypass, the crossfade, the
 * non-finite strikes, the clamp, the warm-up) run through libomx-clap-core in lv2_clap_test.c.
 *
 *  1. PAD: a -20 dB 1x1 fake with a latency port reads exactly 0.1 x the input once steady, and the instance reads the
 *     latency the port reports (rate / PAD_LATENCY_DIV frames: 7 at 44.1 k, 8 at 48 k, 16 at 96 k, 32 at 192 k).
 *  2. inPlaceBroken TWIN: a plugin that zeroes its output before reading its input gives the SAME samples as the pad,
 *     because every port is the instance's own memory; the positive control, the twin run in place by hand, reads
 *     silence.
 *  3. MXCSR: a plugin that clears FTZ/DAZ inside run() leaves them SET after lv2_instance_run; positive control: the
 *     same plugin run bare leaves them clear.
 *  4. WORKER DRAIN ORDER: run(schedule) -> end_run -> work(respond) -> work_response -> run, with the real worker
 *     thread pinned by the quiesce; a respond outside work() is refused and counted; a full request ring answers
 *     NO_SPACE (in a child: the ring size is configured once per process).
 *  5. RT: no allocation inside lv2_instance_run across every arm above (the malloc family wrapped).
 *
 *   lv2_run_test <omx-lv2-fakes.lv2 directory>
 */
#include <pthread.h>
#include <stdint.h>
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

#include "hosted_stage.h"
#include "lv2_core.h"
#include "fixtures/lv2_fakes.h"
#include "lv2_test_util.h"

#define MAXB 256u

/* ---- RT allocation witness ---- */
static volatile int in_rt = 0, rt_allocs = 0;  // volatile: GCC assumes malloc reads no global
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);
void *__wrap_malloc(size_t n) { rt_allocs += in_rt; return __real_malloc(n); }
void *__wrap_calloc(size_t a, size_t b) { rt_allocs += in_rt; return __real_calloc(a, b); }
void *__wrap_realloc(void *p, size_t n) { rt_allocs += in_rt; return __real_realloc(p, n); }
void __wrap_free(void *p) { rt_allocs += in_rt; __real_free(p); }

static char g_dir[PATH_MAX], g_so[PATH_MAX + 32];
static double g_rate;

struct rig
{
    struct lv2_bundle *bundle;
    struct lv2_plugin *plugin;
    struct lv2_instance *in;
    struct fake *f;
};

static int rig_up(struct rig *g, const char *uri, uint32_t max)
{
    char why[LV2_CORE_WHY_MAX] = "";
    lv2_fakes_last_fn last;

    memset(g, 0, sizeof(*g));
    g->bundle = lv2_bundle_ref(g_dir, why);
    g->plugin = g->bundle ? lv2_plugin_open(g->bundle, uri, why) : NULL;
    if (!g->plugin || lv2_plugin_load(g->plugin, why) != 0 || !(g->in = lv2_instance_new(g->plugin))
        || lv2_instance_activate(g->in, g_rate, 1, max, why) != 0)
    {
        CHECK(0, "rack %s (%s)", uri, why);
        return -1;
    }
    *(void **)&last = loaded_symbol(g_so, "lv2_fakes_last");
    g->f = last ? last() : NULL;
    CHECK(g->f && g->f->rate == g_rate, "%s was instantiated at %.0f", uri, g_rate);
    return g->f ? 0 : -1;
}

static void rig_down(struct rig *g)
{
    lv2_instance_free(g->in);
    lv2_plugin_close(g->plugin);
    lv2_bundle_unref(g->bundle);
}

/* one block of the lane through the instance's own buffers, the way the shim drives it */
static void run_rt(struct rig *g, float *l, float *r, uint32_t n)
{
    const uint32_t legs = g->plugin->legs;

    memcpy(lv2_instance_audio_in(g->in, 0), l, n * sizeof(float));
    if (legs == 2)
        memcpy(lv2_instance_audio_in(g->in, 1), r ? r : l, n * sizeof(float));
    in_rt = 1;
    lv2_instance_run(g->in, n, NULL, NULL);
    in_rt = 0;
    memcpy(l, lv2_instance_audio_out(g->in, 0), n * sizeof(float));
    if (r)
        memcpy(r, lv2_instance_audio_out(g->in, legs == 2 ? 1 : 0), n * sizeof(float));
}

static void test_pad_and_latency(void)
{
    struct rig g;
    float l[128], x[128], worst = 0.0f;
    uint32_t want_lat;
    double db;
    int blk, i;

    if (rig_up(&g, FAKE_PAD, MAXB) != 0)
        return;
    CHECK(lv2_instance_latency(g.in) == 0, "no latency before the first run");
    for (blk = 0; blk < 3; blk++)
    {
        tone(x, 128, 0.5f, (uint32_t)blk * 128u);
        memcpy(l, x, sizeof(l));
        run_rt(&g, l, NULL, 128);
    }
    for (i = 0; i < 128; i++)
        worst = fmaxf(worst, fabsf(l[i] - x[i] * PAD_GAIN));
    CHECK(worst == 0.0f, "steady pad reads exactly 0.1 x in, worst |err| %g", worst);
    db = 20.0 * log10((double)fabsf(l[20]) / (double)fabsf(x[20]));
    CHECK(fabs(db + 20.0) < 1e-4, "pad measures %.5f dB", db);
    want_lat = pad_latency_frames(g_rate);
    CHECK(lv2_instance_latency(g.in) == want_lat, "latency read %u, want %u at %.0f", lv2_instance_latency(g.in), want_lat, g_rate);
    rig_down(&g);
}

static void test_in_place_broken(void)
{
    struct rig a, b;
    float la[128], lb[128], buf[128], peak = 0.0f;
    int blk, i;

    if (rig_up(&a, FAKE_PAD, MAXB) != 0 || rig_up(&b, FAKE_BROKEN, MAXB) != 0)
        return;
    for (blk = 0; blk < 3; blk++)
    {
        tone(la, 128, 0.5f, (uint32_t)blk * 128u);
        memcpy(lb, la, sizeof(la));
        run_rt(&a, la, NULL, 128);
        run_rt(&b, lb, NULL, 128);
    }
    CHECK(bits_equal(la, lb, 128), "the inPlaceBroken twin reads the pad's samples through the instance's own buffers");
    // positive control: the twin run IN PLACE by hand destroys its input
    tone(buf, 128, 0.5f, 0);
    b.plugin->desc->connect_port(b.f, 0, buf);
    b.plugin->desc->connect_port(b.f, 1, buf);
    b.plugin->desc->run(b.f, 128);
    for (i = 0; i < 128; i++)
        peak = fmaxf(peak, fabsf(buf[i]));
    CHECK(peak == 0.0f, "positive control: the twin in place reads silence (peak %g)", peak);
    rig_down(&a);
    rig_down(&b);
}

static void test_mxcsr(void)
{
#if defined(__x86_64__) || defined(__i386__)
    struct rig g;
    float l[64];

    if (rig_up(&g, FAKE_CLOBBER, MAXB) != 0)
        return;
    omx_hosted_denormals_off();
    tone(l, 64, 0.5f, 0);
    g.plugin->desc->run(g.f, 64);       // positive control: bare, the plugin clears FTZ/DAZ
    CHECK((_mm_getcsr() & 0x8040u) == 0u, "positive control: the clobberer clears FTZ/DAZ");
    omx_hosted_denormals_off();
    run_rt(&g, l, NULL, 64);
    CHECK((_mm_getcsr() & 0x8040u) == 0x8040u, "FTZ/DAZ re-asserted after lv2_instance_run (mxcsr %#x)", _mm_getcsr());
    rig_down(&g);
#endif
}

static const char *trace(void)
{
    lv2_fakes_trace_fn fn;

    *(void **)&fn = loaded_symbol(g_so, "lv2_fakes_trace");
    return fn ? fn() : "";
}

static void trace_clear(void)
{
    lv2_fakes_trace_clear_fn fn;

    *(void **)&fn = loaded_symbol(g_so, "lv2_fakes_trace_clear");
    if (fn)
        fn();
}

static void test_worker(void)
{
    struct lv2_counters c;
    struct rig g;
    float l[32] = { 0 };

    if (rig_up(&g, FAKE_WORKER, MAXB) != 0)
        return;
    trace_clear();
    CHECK(lv2_instance_has_worker(g.in), "the worker fake runs a worker thread");
    run_rt(&g, l, NULL, 32);
    CHECK(strncmp(trace(), "run0,end0", 9) == 0, "block 0: run then end_run, no work on the RT (%s)", trace());
    lv2_instance_worker_quiesce(g.in);
    run_rt(&g, l, NULL, 32);
    CHECK(!strcmp(trace(), "run0,end0,work0,resp0,run1,end1"), "response drained BEFORE the next run: %s", trace());

    lv2_instance_worker_quiesce(g.in);
    CHECK(g.f->respond && g.f->respond(g.f->respond_handle, 4, "abcd") == LV2_WORKER_ERR_UNKNOWN, "respond outside work() is refused");
    lv2_instance_counters(g.in, &c);
    CHECK(c.respond_strikes == 1, "and counted a strike (%u)", c.respond_strikes);
    trace_clear();
    run_rt(&g, l, NULL, 32);
    CHECK(!strcmp(trace(), "resp1,run2,end2"), "and never reaches the response ring: %s", trace());
    rig_down(&g);
}

/* a 16-byte request ring holds two 4-byte records; the third schedule is NO_SPACE */
static void worker_full_ring(void)
{
    static const struct lv2_core_config config = { lv2_core_provided, 1000000u, 16u, 0u, NULL };
    struct lv2_counters c;
    char why[LV2_CORE_WHY_MAX];
    struct rig g;
    float l[32] = { 0 };
    int blk;

    g_rate = 48000.0;
    CHECK(lv2_core_configure(&config, why) == 0, "configure a 16-byte ring, a worker polling every second (%s)", why);
    if (rig_up(&g, FAKE_WORKER, MAXB) != 0)
        return;
    trace_clear();
    for (blk = 0; blk < 3; blk++)
        run_rt(&g, l, NULL, 32);
    lv2_instance_counters(g.in, &c);
    CHECK(c.schedule_refused == 1, "full request ring answered NO_SPACE once (%u)", c.schedule_refused);
    lv2_instance_worker_quiesce(g.in);
    CHECK(strstr(trace(), "work0") && strstr(trace(), "work1") && !strstr(trace(), "work2"),
          "the two that fit are still serviced (%s)", trace());
    rig_down(&g);
}

int main(int argc, char **argv)
{
    static const struct lv2_core_config config = { lv2_core_provided, 1000u, 64u, 0u, NULL };
    char why[LV2_CORE_WHY_MAX];
    void *volatile probe;
    size_t r;

    if (argc < 2 || abs_dir(argv[1], g_dir) != 0)
    {
        printf("FAIL usage: lv2_run_test <omx-lv2-fakes.lv2>\n");
        return 1;
    }
    snprintf(g_so, sizeof(g_so), "%somx-lv2-fakes.so", g_dir);
    // the witness's own positive control: an allocation inside the RT window IS seen
    in_rt = 1;
    probe = malloc(16);
    free(probe);
    in_rt = 0;
    CHECK(rt_allocs == 2, "allocation witness sees malloc+free (%d)", rt_allocs);
    rt_allocs = 0;

    in_child("worker full ring", worker_full_ring);
    CHECK(lv2_core_configure(&config, why) == 0, "configure every provider, a 64-byte ring (%s)", why);
    for (r = 0; r < N_RATES; r++)
    {
        const int before = g_failures;

        g_rate = RATES[r];
        test_pad_and_latency();
        test_in_place_broken();
        test_mxcsr();
        test_worker();
        printf("lv2 run @ %.0f: %d failure(s)\n", g_rate, g_failures - before);
    }
    CHECK(rt_allocs == 0, "no allocation inside lv2_instance_run (%d)", rt_allocs);
    printf("%s\n", g_failures == 0 ? "lv2 run test ok" : "lv2 run test FAILED");
    return g_failures == 0 ? 0 : 1;
}
