/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/*
 * What a grouped chain costs per sample: 8 members, the fake passthrough and the fake compressor in turn, stereo, run
 * block after block through omx_clap_chain_process; the median of seven runs, in ns per frame for the chain and per
 * frame and member. A measurement, not a test. It also prints a hash of the output of a fixed input, so two builds
 * that should sound the same can be compared bit for bit.
 *
 *   tests/chain_bench <fake.clap> <fake_compressor.clap> [frames] [blocks]     (default 128, 20000)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../src/clap_chain.h"

#define CH       2u
#define MEMBERS  8u
#define RUNS     7

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
static int cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

static struct omx_clap_instance *open_one(const char *path, const char *id, uint32_t frames)
{
    struct omx_clap_instance *in = NULL;
    char why[OMX_CLAP_WHY_MAX] = "";

    if (omx_clap_host_open(path, id, &in, why) != 0 || !in || omx_clap_host_activate(in, 48000.0, frames, why) != 0)
    {
        fprintf(stderr, "%s cannot be opened and activated: %s\n", id, why);
        exit(1);
    }
    omx_clap_host_publish(in, pthread_self());
    return in;
}

/* block `b` of the input: deterministic noise, its level stepping so the compressor acts on some blocks only */
static void input_block(float *const *in, uint32_t n, uint32_t *seed, uint32_t b)
{
    static const float level[] = { 0.05f, 0.3f, 0.02f, 0.5f, 0.033f };

    for (uint32_t i = 0; i < n; i++)
        for (uint32_t c = 0; c < CH; c++)
        {
            *seed = *seed * 1664525u + 1013904223u;
            in[c][i] = level[b % 5u] * ((float)(*seed >> 8) / 8388608.0f - 1.0f);
        }
}

int main(int argc, char **argv)
{
    const uint32_t frames = argc > 3 ? (uint32_t)atoi(argv[3]) : 128u;
    const uint32_t blocks = argc > 4 ? (uint32_t)atoi(argv[4]) : 20000u;
    struct omx_clap_instance *member[MEMBERS];
    struct omx_clap_chain_config config;
    struct omx_clap_chain *chain = NULL;
    float *in[CH], *out[CH];
    double runs[RUNS];
    uint64_t hash = 1469598103934665603ull;
    uint32_t seed = 12345u;

    if (argc < 3 || frames == 0 || frames > OMX_CLAP_CHAIN_FRAMES_MAX)
    {
        fprintf(stderr, "usage: %s <fake.clap> <fake_compressor.clap> [frames <= %u] [blocks]\n", argv[0], OMX_CLAP_CHAIN_FRAMES_MAX);
        return 2;
    }
    omx_clap_chain_config_init(&config);
    config.channels = CH;
    if (omx_clap_chain_create(&config, &chain) != OMX_CLAP_CHAIN_OK)
        return 1;
    for (uint32_t i = 0; i < MEMBERS; i++)
    {
        struct omx_clap_chain_member d;
        uint32_t id;

        member[i] = i & 1u ? open_one(argv[2], "org.omx-clap-host.test.compressor", frames)
                           : open_one(argv[1], "org.omx-clap-host.test.passthrough", frames);
        omx_clap_chain_member_init(&d);
        d.instance = member[i];
        if (omx_clap_chain_add(chain, &d, OMX_CLAP_CHAIN_END, &id) != OMX_CLAP_CHAIN_OK)
            return 1;
    }
    for (uint32_t c = 0; c < CH; c++)
    {
        in[c] = aligned_alloc(OMX_CLAP_CHAIN_ALIGN, OMX_CLAP_CHAIN_FRAMES_MAX * sizeof(float));
        out[c] = aligned_alloc(OMX_CLAP_CHAIN_ALIGN, OMX_CLAP_CHAIN_FRAMES_MAX * sizeof(float));
    }
    // the hash: 2000 blocks of the fixed input, every output bit
    for (uint32_t b = 0; b < 2000u; b++)
    {
        input_block(in, frames, &seed, b);
        omx_clap_chain_process(chain, (const float *const *)in, out, frames);
        for (uint32_t c = 0; c < CH; c++)
        {
            const unsigned char *p = (const unsigned char *)out[c];

            for (size_t k = 0; k < frames * sizeof(float); k++)
                hash = (hash ^ p[k]) * 1099511628211ull;
        }
    }
    // the timing: the input held, the chain run `blocks` times a run
    for (int r = 0; r < RUNS; r++)
    {
        const double t0 = now();

        for (uint32_t b = 0; b < blocks; b++)
            omx_clap_chain_process(chain, (const float *const *)in, out, frames);
        runs[r] = (now() - t0) / ((double)blocks * frames);
    }
    qsort(runs, RUNS, sizeof runs[0], cmp);
    printf("chain of %u (passthrough, compressor in turn), %u ch, %u frames: %.3f ns/frame, %.3f ns/frame/member; output hash %016llx\n",
           MEMBERS, CH, frames, runs[RUNS / 2], runs[RUNS / 2] / MEMBERS, (unsigned long long)hash);
    omx_clap_chain_destroy(chain);
    for (uint32_t i = 0; i < MEMBERS; i++)
        omx_clap_host_close(member[i]);
    for (uint32_t c = 0; c < CH; c++)
    {
        free(in[c]);
        free(out[c]);
    }
    return 0;
}
