// SPDX-License-Identifier: GPL-3.0-or-later
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

/* stage_bench.c: what one omx_clap_run block costs, a -20 dB pad behind the stage, 64-frame stereo blocks, in ns per
 * block (the median of 9 runs of 200 000 blocks). Built twice by 'make bench-stage': as the header is, and with the two
 * in_cycle stores of omx_clap_run relaxed (a copy of clap_stage.h patched under build/), so the two figures say what
 * their ordering costs. A measurement, not a test: it prints and exits 0. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "clap_stage.h"

#define BLOCK 64u
#define BLOCKS 200000u
#define RUNS 9

static bool p_true(const clap_plugin_t *p) { (void)p; return true; }
static void p_void(const clap_plugin_t *p) { (void)p; }
static bool p_activate(const clap_plugin_t *p, double sr, uint32_t a, uint32_t b) { (void)p, (void)sr, (void)a, (void)b; return true; }
static const void *p_ext(const clap_plugin_t *p, const char *id) { (void)p, (void)id; return NULL; }
static clap_process_status p_process(const clap_plugin_t *p, const clap_process_t *pr)
{
    (void)p;
    for (uint32_t c = 0; c < pr->audio_outputs[0].channel_count; c++)
        for (uint32_t i = 0; i < pr->frames_count; i++)
            pr->audio_outputs[0].data32[c][i] = 0.1f * pr->audio_inputs[0].data32[c][i];
    return CLAP_PROCESS_CONTINUE;
}
static const char *const FEATURES[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL};
static const clap_plugin_descriptor_t DESC = {CLAP_VERSION_INIT, "org.omx-clap-host.bench.pad", "pad", "omx-clap-host", "", "", "", "0", "", FEATURES};
static const clap_plugin_t PAD = {&DESC, NULL, p_true, p_void, p_activate, p_void, p_true, p_void, p_void, p_process, p_ext, p_void};

static int cmp(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(void)
{
    static struct omx_clap_stage st;
    static float in_l[BLOCK], in_r[BLOCK], out_l[BLOCK], out_r[BLOCK], l[BLOCK], r[BLOCK];
    static struct omx_clap_param_record recs[CLAP_HOST_PARAM_QUEUE_DEPTH];
    double ns[RUNS];

    if (omx_clap_stage_init(&st, &(struct omx_hosted_bounce){in_l, in_r, out_l, out_r, BLOCK}, recs, CLAP_HOST_PARAM_QUEUE_DEPTH) != 0
        || omx_clap_bind(&st, &PAD, 2) != 0)
        return 1;
    omx_clap_arm(&st);
    for (uint32_t i = 0; i < BLOCK; i++)
        l[i] = r[i] = 0.25f;
    for (int k = 0; k < RUNS; k++)
    {
        struct timespec t0, t1;

        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (uint32_t b = 0; b < BLOCKS; b++)
            omx_clap_run(&st, l, r, BLOCK);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ns[k] = ((double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec)) / BLOCKS;
    }
    qsort(ns, RUNS, sizeof(ns[0]), cmp);
    printf("stage_bench %s: %.1f ns/block (median of %d, min %.1f, max %.1f), %u-frame stereo blocks\n", BENCH_LABEL, ns[RUNS / 2], RUNS,
           ns[0], ns[RUNS - 1], BLOCK);
    return 0;
}
