/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/*
 * What the host adds to a plugin per call and per sample: an empty CLAP (process() returns at once) through
 * omx_clap_host_run on a sustained -6 dBFS tone, 64 warm-up blocks discarded, the p50 and p95 of 2000 blocks at quanta
 * 16 to 1024, a least-squares line through them. Then the same at one quantum with each guard taken off, so the
 * per-sample figure can be split. Then a chain of CHAIN_N members at 48000/256, through omx_clap_chain_process. Prints;
 * not a gate. Built at -O2, as an engine compiles the inline body.
 *
 *   tests/hosted_cost_bench [cpu]                                  the empty CLAP alone, and a chain of them
 *   tests/hosted_cost_bench_lv2 <omx-direct-fixture.lv2> [cpu]     the chains alone: CLAP, LV2 and every other one of each
 *
 * The LV2 member is the fixture's #empty, stating lv2:hardRTCapable, so a chain's figure is the host's cost alone.
 */
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/clap_chain.h"
#include "../src/clap_host.h"
#ifdef HOSTED_COST_LV2
#include "omx_clap_lv2.h"
#endif

static bool nl_init(const clap_plugin_t *p) { (void)p; return true; }
static void nl_destroy(const clap_plugin_t *p) { free((void *)p); }
static bool nl_activate(const clap_plugin_t *p, double sr, uint32_t a, uint32_t b) { (void)p, (void)sr, (void)a, (void)b; return true; }
static void nl_nop(const clap_plugin_t *p) { (void)p; }
static bool nl_start(const clap_plugin_t *p) { (void)p; return true; }
static clap_process_status nl_process(const clap_plugin_t *p, const clap_process_t *pr) { (void)p, (void)pr; return CLAP_PROCESS_CONTINUE; }
static uint32_t nl_ports_count(const clap_plugin_t *p, bool is_input) { (void)p, (void)is_input; return 1; }
static bool nl_ports_get(const clap_plugin_t *p, uint32_t i, bool is_input, clap_audio_port_info_t *info)
{
    (void)p, (void)is_input;
    if (i != 0) return false;
    memset(info, 0, sizeof *info);
    snprintf(info->name, sizeof info->name, "main");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
static const clap_plugin_audio_ports_t NL_PORTS = {nl_ports_count, nl_ports_get};
static const void *nl_ext(const clap_plugin_t *p, const char *id) { (void)p; return strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0 ? &NL_PORTS : NULL; }
static const char *const NL_FEATURES[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL};
static const clap_plugin_descriptor_t NL_DESC = {CLAP_VERSION_INIT, "bench.empty", "empty", "bench", "", "", "", "0", "", NL_FEATURES};
static uint32_t nl_count(const clap_plugin_factory_t *f) { (void)f; return 1; }
static const clap_plugin_descriptor_t *nl_desc(const clap_plugin_factory_t *f, uint32_t i) { (void)f; return i == 0 ? &NL_DESC : NULL; }
static const clap_plugin_t *nl_create(const clap_plugin_factory_t *f, const clap_host_t *host, const char *id)
{
    (void)f, (void)host;
    if (strcmp(id, NL_DESC.id) != 0) return NULL;
    clap_plugin_t *p = calloc(1, sizeof *p);
    *p = (clap_plugin_t){&NL_DESC, NULL, nl_init, nl_destroy, nl_activate, nl_nop, nl_start, nl_nop, nl_nop, nl_process, nl_ext, nl_nop};
    return p;
}
static const clap_plugin_factory_t NL_FACTORY = {nl_count, nl_desc, nl_create};
static bool nl_entry_init(const char *path) { (void)path; return true; }
static void nl_entry_deinit(void) {}
static const void *nl_factory(const char *id) { return strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &NL_FACTORY : NULL; }
static const clap_plugin_entry_t NL_ENTRY = {CLAP_VERSION_INIT, nl_entry_init, nl_entry_deinit, nl_factory};

#define QMAX 1024u
#define WARM 64
#define BLOCKS 2000
static float L[QMAX], R[QMAX];
static long T[BLOCKS];

static long now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000L + t.tv_nsec; }
static int cmp(const void *a, const void *b) { const long x = *(const long *)a, y = *(const long *)b; return (x > y) - (x < y); }

/* p50 and p95 of one quantum, `guards` as the stage's guard word (UINT32_MAX: as opened) */
static int cell(double sr, uint32_t q, uint32_t guards, long *p50, long *p95)
{
    struct omx_clap_instance *in = NULL;
    char why[OMX_CLAP_WHY_MAX] = "";
    uint32_t ph = 0;

    if (omx_clap_host_open_entry(&NL_ENTRY, NL_DESC.id, &in, why) != 0 || omx_clap_host_activate(in, sr, q, why) != 0)
    {
        printf("FAIL %s\n", why);
        return -1;
    }
    omx_clap_host_publish(in, pthread_self());
    if (guards != UINT32_MAX) in->stage.h.guards = guards;
    for (int b = 0; b < WARM + BLOCKS; b++)
    {
        for (uint32_t i = 0; i < q; i++, ph++) L[i] = R[i] = 0.5f * sinf(2.0f * (float)M_PI * 1000.0f * (float)ph / (float)sr);
        const long t0 = now_ns();
        omx_clap_host_run(in, L, R, q);
        const long t1 = now_ns();
        if (b >= WARM) T[b - WARM] = t1 - t0;
    }
    (void)omx_clap_host_unpublish(in, 100, 2000000);
    omx_clap_host_deactivate(in);
    omx_clap_host_close(in);
    qsort(T, BLOCKS, sizeof T[0], cmp);
    *p50 = T[BLOCKS / 2];
    *p95 = T[BLOCKS * 95 / 100];
    return 0;
}

#define CHAIN_N 24u
#define CHAIN_RATE 48000.0
#define CHAIN_Q 256u
#define CHAIN_BLOCKS 4000
static long CT[CHAIN_BLOCKS];

/* the median block of a chain of CHAIN_N members, `entries[i % n]` naming each; ns/sample of the chain, per channel pair */
static int chain_cell(const char *name, const clap_plugin_entry_t *const *entries, const char *const *ids, uint32_t n_kinds)
{
    struct omx_clap_instance *in[CHAIN_N];
    struct omx_clap_chain *chain = NULL;
    struct omx_clap_chain_config config;
    char why[OMX_CLAP_WHY_MAX] = "";
    static float il[CHAIN_Q], ir[CHAIN_Q], ol[CHAIN_Q], or_[CHAIN_Q];
    const float *const ins[2] = {il, ir};
    float *const outs[2] = {ol, or_};
    uint32_t ph = 0;

    omx_clap_chain_config_init(&config);
    config.channels = 2;
    if (omx_clap_chain_create(&config, &chain) != OMX_CLAP_CHAIN_OK)
        return -1;
    for (uint32_t i = 0; i < CHAIN_N; i++)
    {
        struct omx_clap_chain_member m;

        if (omx_clap_host_open_entry(entries[i % n_kinds], ids[i % n_kinds], &in[i], why) != 0
            || omx_clap_host_activate(in[i], CHAIN_RATE, CHAIN_Q, why) != 0)
        {
            printf("FAIL %s\n", why);
            return -1;
        }
        omx_clap_host_publish(in[i], pthread_self());
        omx_clap_chain_member_init(&m);
        m.instance = in[i];
        if (omx_clap_chain_add(chain, &m, OMX_CLAP_CHAIN_END, NULL) != OMX_CLAP_CHAIN_OK)
        {
            printf("FAIL chain add\n");
            return -1;
        }
    }
    for (int b = 0; b < WARM + CHAIN_BLOCKS; b++)
    {
        for (uint32_t i = 0; i < CHAIN_Q; i++, ph++)
            il[i] = ir[i] = 0.5f * sinf(2.0f * (float)M_PI * 1000.0f * (float)ph / (float)CHAIN_RATE);
        const long t0 = now_ns();
        omx_clap_chain_process(chain, ins, outs, CHAIN_Q);
        const long t1 = now_ns();
        if (b >= WARM) CT[b - WARM] = t1 - t0;
    }
    qsort(CT, CHAIN_BLOCKS, sizeof CT[0], cmp);
    printf("chain %-5s %u members 48000/%u: p50 %ld ns/block  %.3f ns/sample  %.4f ns/sample per member  p95 %ld ns/block\n", name, CHAIN_N, CHAIN_Q,
           CT[CHAIN_BLOCKS / 2], (double)CT[CHAIN_BLOCKS / 2] / CHAIN_Q, (double)CT[CHAIN_BLOCKS / 2] / CHAIN_Q / CHAIN_N, CT[CHAIN_BLOCKS * 95 / 100]);
    omx_clap_chain_destroy(chain);
    for (uint32_t i = 0; i < CHAIN_N; i++)
    {
        (void)omx_clap_host_unpublish(in[i], 100, 2000000);
        omx_clap_host_deactivate(in[i]);
        omx_clap_host_close(in[i]);
    }
    return 0;
}

int main(int argc, char **argv)
{
    static const uint32_t Q[] = {16, 32, 64, 128, 256, 512, 1024};
    const size_t nq = sizeof Q / sizeof Q[0];
    const double sr = 96000;
    double sx = 0, sxx = 0, s50 = 0, sx50 = 0, s95 = 0, sx95 = 0;
    cpu_set_t set;
    const clap_plugin_entry_t *const clap_entry[1] = {&NL_ENTRY};
    const char *const clap_id[1] = {NL_DESC.id};
#ifdef HOSTED_COST_LV2
    const int cpu_arg = 2;
#else
    const int cpu_arg = 1;
#endif

    if (argc > cpu_arg)
    {
        CPU_ZERO(&set);
        CPU_SET(atoi(argv[cpu_arg]), &set);
        (void)sched_setaffinity(0, sizeof set, &set);
    }
#ifdef HOSTED_COST_LV2
    {
        static const char URI[] = "urn:openmixer:test:direct-fixture#empty";
        const omx_clap_lv2_config_t config = {omx_clap_lv2_provided, 1000u, 4096u, 0u, 250u, NULL, NULL};
        char why[OMX_CLAP_LV2_WHY_MAX] = "";
        const clap_plugin_entry_t *lv2 = argc > 1 && omx_clap_lv2_configure(&config, why) == 0 ? omx_clap_lv2_entry(argv[1], why) : NULL;
        const clap_plugin_entry_t *const both[2] = {&NL_ENTRY, lv2};
        const char *const both_id[2] = {NL_DESC.id, URI};

        if (!lv2)
        {
            printf("FAIL usage: hosted_cost_bench_lv2 <omx-direct-fixture.lv2, absolute> [cpu] (%s)\n", why);
            return 2;
        }
        return chain_cell("clap", clap_entry, clap_id, 1) || chain_cell("lv2", &both[1], &both_id[1], 1) || chain_cell("mixed", both, both_id, 2) ? 2 : 0;
    }
#endif
    for (size_t k = 0; k < nq; k++)
    {
        long a, b;
        if (cell(sr, Q[k], UINT32_MAX, &a, &b) != 0) return 2;
        printf("q=%-5u p50 %6ld ns  p95 %6ld ns\n", Q[k], a, b);
        sx += Q[k], sxx += (double)Q[k] * Q[k], s50 += (double)a, sx50 += (double)Q[k] * a, s95 += (double)b, sx95 += (double)Q[k] * b;
    }
    const double det = nq * sxx - sx * sx;
    printf("stereo line: p50 %.1f ns/call + %.3f ns/sample; p95 %.1f ns/call + %.3f ns/sample\n", (sxx * s50 - sx * sx50) / det,
           (nq * sx50 - sx * s50) / det, (sxx * s95 - sx * sx95) / det, (nq * sx95 - sx * s95) / det);
    static const struct { const char *name; uint32_t off; } V[] = {
        {"all guards", 0}, {"no clamp", OMX_HOSTED_GUARD_CLAMP}, {"no finite scan", OMX_HOSTED_GUARD_NONFINITE},
        {"neither", OMX_HOSTED_GUARD_CLAMP | OMX_HOSTED_GUARD_NONFINITE}};
    for (size_t v = 0; v < sizeof V / sizeof V[0]; v++)
    {
        long a, b;
        if (cell(sr, 1024, OMX_HOSTED_GUARDS_ALL & ~V[v].off, &a, &b) != 0) return 2;
        printf("q=1024 %-16s p50 %.3f ns/sample  p95 %.3f ns/sample\n", V[v].name, (double)a / 1024, (double)b / 1024);
    }
    return chain_cell("clap", clap_entry, clap_id, 1) ? 2 : 0;
}
