/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/*
 * The hosted clamp keeps the libm definition, bit for bit, on every value but a NaN: each value below and a sweep of
 * bit patterns, compared with fminf(fmaxf(x, -c), c), and its count of moved samples. tests/hosted_clamp_no_libm.sh holds the other
 * half: the compiled body makes no libm call per sample, the 3.9 ns/sample tests/hosted_cost_bench measured.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/hosted_stage.h"

#define N 4096u
static float b[N], in[N];

int main(void)
{
    const float c = omx_db_to_lin(OMX_HOSTED_STAGE_CLAMP_DBFS);
    const float edge[] = {0.0f, -0.0f, c, -c, nextafterf(c, 0.0f), nextafterf(c, 10.0f), nextafterf(-c, 0.0f), nextafterf(-c, -10.0f),
                          1e-42f, -1e-42f, 1.0f, -1.0f, 1e30f, -1e30f, INFINITY, -INFINITY, NAN, -NAN};
    const uint32_t ne = sizeof edge / sizeof edge[0];
    uint32_t want = 0, fail = 0, seed = 12345;

    for (uint32_t i = 0; i < N; i++)
    {
        if (i < ne)
            in[i] = edge[i];
        else
        {
            seed = seed * 1664525u + 1013904223u;
            memcpy(&in[i], &seed, sizeof seed);     // every bit pattern a plugin could write
        }
        b[i] = in[i];
        want += (uint32_t)(in[i] > c) + (uint32_t)(in[i] < -c);
    }
    const uint32_t moved = omx_hosted_clamp(b, N);
    for (uint32_t i = 0; i < N; i++)
    {
        const float ref = fminf(fmaxf(in[i], -c), c);
        // libm's answer to a NaN depends on quiet or signalling; the non-finite guard strikes those blocks before the
        // clamp, so all a NaN owes here is a finite sample at a ceiling
        if (isnan(in[i]) ? fabsf(b[i]) != c : memcmp(&ref, &b[i], sizeof ref) != 0)
        {
            printf("FAIL sample %u: in %a clamp %a libm %a\n", i, (double)in[i], (double)b[i], (double)ref);
            fail++;
        }
    }
    if (moved != want)
    {
        printf("FAIL moved %u, want %u\n", moved, want);
        fail++;
    }
    printf("hosted_clamp: %u samples, %u moved, %u failures\n", N, moved, fail);
    return fail ? 1 : 0;
}
