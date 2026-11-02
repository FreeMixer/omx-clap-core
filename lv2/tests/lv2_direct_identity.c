/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/*
 * Every plugin of one LV2 bundle through the CLAP face at 48000/256, on two legs of noise, with one line per plugin:
 *
 *   <uri> <hash of 400 blocks, the output bits> <hash again from a fresh instance> <scribble> <stale>
 *
 * scribble is the blocks in which the plugin wrote its input buffer, and stale the blocks in which an output sample still
 * held the marker the host wrote there before the run (a plugin that leaves output unwritten, or skips a block). The
 * marker is written over the output before each block, so a plugin that reads or leaves last block's output shows up:
 * under the instance's own memory it did so quietly. Two builds give one result for a plugin that is bit-identical; two
 * unequal hashes in one build mean the plugin is not deterministic and says nothing. tests/lv2_direct_identity.sh is the
 * driver. Prints; not a gate.
 *
 *   lv2_direct_identity <absolute bundle directory>
 */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <clap/clap.h>

#include "omx_clap_lv2.h"
#include "omx_clap_raw.h"

#define Q 256u
#define BLOCKS 400
#define MARK 0.0123456789f

static uint32_t in_size(const clap_input_events_t *l) { (void)l; return 0; }
static const clap_event_header_t *in_get(const clap_input_events_t *l, uint32_t i) { (void)l, (void)i; return NULL; }
static bool out_push(const clap_output_events_t *l, const clap_event_header_t *e) { (void)l, (void)e; return true; }

static float B[8][Q] __attribute__((aligned(64)));

static uint32_t rng(uint32_t *s)
{
    uint32_t x = *s;

    x ^= x << 13, x ^= x >> 17, x ^= x << 5;
    return *s = x;
}

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;

    while (n--)
        h = (h ^ *b++) * 1099511628211ull;
    return h;
}

/* one fresh instance rendered: its hash, and the two counts; 0: it did not run */
static int render(const clap_plugin_factory_t *f, clap_host_t *host, const char *uri, uint64_t *hash, unsigned *scribble, unsigned *stale)
{
    const clap_plugin_t *p = f->create_plugin(f, host, uri);
    const clap_plugin_audio_ports_t *ports;
    const clap_input_events_t in_events = {NULL, in_size, in_get};
    const clap_output_events_t out_events = {NULL, out_push};
    clap_audio_buffer_t ib[4], ob[4];
    float *ptr[8], *optr[8];
    static float ref[8][Q];
    uint32_t ni = 0, no = 0, k = 0, seed = 0x1234567u;
    uint32_t nib, nob;
    clap_process_t proc;

    *hash = 14695981039346656037ull, *scribble = 0, *stale = 0;
    if (!p || !p->init(p))
        return 0;
    ports = p->get_extension(p, CLAP_EXT_AUDIO_PORTS);
    nib = ports ? ports->count(p, true) : 0;
    nob = ports ? ports->count(p, false) : 0;
    if (!ports || nib > 4 || nob > 4 || !p->activate(p, 48000.0, 1, Q) || !p->start_processing(p))
    {
        p->destroy(p);
        return 0;
    }
    memset(ib, 0, sizeof ib), memset(ob, 0, sizeof ob);
    for (uint32_t i = 0; i < nib; i++)
    {
        clap_audio_port_info_t info;

        ports->get(p, i, true, &info);
        ib[i].data32 = &ptr[ni];
        ib[i].channel_count = info.channel_count;
        for (uint32_t c = 0; c < info.channel_count && ni < 4; c++)
            ptr[ni++] = B[k++];
        if (ni == 4 && i + 1 < nib)
            goto done;
    }
    for (uint32_t i = 0; i < nob; i++)
    {
        clap_audio_port_info_t info;

        ports->get(p, i, false, &info);
        ob[i].data32 = &optr[no];
        ob[i].channel_count = info.channel_count;
        for (uint32_t c = 0; c < info.channel_count && no < 4; c++)
            optr[no++] = B[k++];
        if (no == 4 && i + 1 < nob)
            goto done;
    }
    memset(&proc, 0, sizeof proc);
    proc.steady_time = -1;
    proc.frames_count = Q;
    proc.audio_inputs = ib;
    proc.audio_outputs = ob;
    proc.audio_inputs_count = nib;
    proc.audio_outputs_count = nob;
    proc.in_events = &in_events;
    proc.out_events = &out_events;
    for (int b = 0; b < BLOCKS; b++)
    {
        int wrote_in = 0, left = 0;

        for (uint32_t c = 0; c < ni; c++)
            for (uint32_t i = 0; i < Q; i++)
                ref[c][i] = ptr[c][i] = 0.25f * (float)(int32_t)rng(&seed) / 2147483648.0f;
        for (uint32_t c = 0; c < no; c++)
            for (uint32_t i = 0; i < Q; i++)
                optr[c][i] = MARK;
        if (p->process(p, &proc) == CLAP_PROCESS_ERROR)
            goto done;
        for (uint32_t c = 0; c < ni; c++)
            wrote_in |= memcmp(ptr[c], ref[c], sizeof ref[c]) != 0;
        for (uint32_t c = 0; c < no; c++)
        {
            for (uint32_t i = 0; i < Q; i++)
                left |= optr[c][i] == MARK;
            *hash = fnv(*hash, optr[c], Q * sizeof(float));
        }
        *scribble += (unsigned)wrote_in;
        *stale += (unsigned)left;
    }
    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
    return 1;
done:
    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
    return 0;
}

int main(int argc, char **argv)
{
    const omx_clap_lv2_config_t config = {omx_clap_lv2_provided, 1000u, 4096u, 0u, 250u, NULL, NULL};
    char why[OMX_CLAP_LV2_WHY_MAX] = "";
    clap_host_t host = OMX_CLAP_RAW_NULL_HOST("lv2_direct_identity");
    const clap_plugin_entry_t *entry;
    const clap_plugin_factory_t *f;

    if (argc < 2 || omx_clap_lv2_configure(&config, why) != 0)
    {
        printf("usage: lv2_direct_identity <absolute bundle directory> (%s)\n", why);
        return 2;
    }
    entry = omx_clap_lv2_entry(argv[1], why);
    f = entry ? entry->get_factory(CLAP_PLUGIN_FACTORY_ID) : NULL;
    if (!f)
        return 2;
    for (uint32_t i = 0; i < f->get_plugin_count(f); i++)
    {
        const char *uri = f->get_plugin_descriptor(f, i)->id;
        uint64_t a, b;
        unsigned sa, la, sb, lb;

        if (render(f, &host, uri, &a, &sa, &la) && render(f, &host, uri, &b, &sb, &lb))
            printf("%s %016llx %016llx %u %u\n", uri, (unsigned long long)a, (unsigned long long)b, sa, la);
        else
            printf("%s no-render - 0 0\n", uri);
        fflush(stdout);
    }
    omx_clap_lv2_entry_release(entry);
    return 0;
}
