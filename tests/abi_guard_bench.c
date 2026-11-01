/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/*
 * What omx_abi_call6 costs per call: an empty plugin call, and a gain over one 256-frame block (a run() with work in
 * it), each called directly and through the guard, the median of seven runs of N calls. Prints ns per call.
 *
 *   tests/abi_guard_bench [N]     (default 20000000)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../src/abi_guard.h"

#define FRAMES 256
static float buf[FRAMES];

__attribute__((noinline)) static uint64_t empty(uint64_t a) { __asm__ volatile(""); return a; }
__attribute__((noinline)) static uint64_t gain(uint64_t frames)
{
    for (uint32_t i = 0; i < (uint32_t)frames; i++) buf[i] *= 0.999f;
    return frames;
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
static int cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

static double median_ns(uint64_t (*fn)(uint64_t), uint64_t arg, long n, int guarded)
{
    double runs[7];
    struct omx_abi_report r = { 0, 0, 0 };
    for (int k = 0; k < 7; k++) {
        uint64_t sink = 0;
        const double t0 = now();
        if (guarded) for (long i = 0; i < n; i++) sink += omx_abi_call6(&r, (const void *)fn, arg, 0, 0, 0, 0, 0);
        else for (long i = 0; i < n; i++) sink += fn(arg);
        runs[k] = (now() - t0) / (double)n;
        __asm__ volatile("" : : "r"(sink));
    }
    qsort(runs, 7, sizeof runs[0], cmp);
    return runs[3];
}

int main(int argc, char **argv)
{
    const long n = argc > 1 ? atol(argv[1]) : 20000000L;
    for (int i = 0; i < FRAMES; i++) buf[i] = 1.0f;
    const double e0 = median_ns(empty, 1, n, 0), e1 = median_ns(empty, 1, n, 1);
    const double g0 = median_ns(gain, FRAMES, n / 20, 0), g1 = median_ns(gain, FRAMES, n / 20, 1);
    printf("empty call:       direct %.2f ns, guarded %.2f ns, cost %.2f ns per call\n", e0, e1, e1 - e0);
    printf("256-frame gain:   direct %.2f ns, guarded %.2f ns, cost %.2f ns per call (%.2f%%)\n", g0, g1, g1 - g0,
           100.0 * (g1 - g0) / g0);
    return 0;
}
