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
 * lv2_layout_fixture.c: the audio-layout plugins of omx-layout-fixture.lv2, a test fixture and never a shipped plugin.
 * Each twin is a fixed sum over its ports, so a value read at an output proves which input reached it: the port numbers
 * below are the TTL's indices. Nothing is allocated or locked in run().
 */

#include <stdlib.h>
#include <string.h>

#include <lv2/core/lv2.h>

#define LAYOUT_URI "urn:openmixer:test:layout-fixture"
#define MAX_PORTS 80u

enum twin
{
    MAIN_AUX,       // in 0-1 main group, 2-3 and 4-5 two aux groups, out 6-7: out_c = main_c + aux1_c + aux2_c
    TWO_MONO,       // in 0 and 1 two mono groups, out 2: out = a + b
    FOUR_FOUR,      // in 0-3, out 4-7: out_c = in_c
    FOUR_TWO,       // in 0-3, out 4-5: out_c = in_c + in_(c+2)
    MONO_STEREO,    // in 0, out 1-2 the main output group: out_l = in, out_r = -in
    TWO_ONE,        // in 0-1, out 2: out = l + r
    BANDS,          // in 0-1, out 2-3 the main output group, 4-9 three band groups: main = in, band_k = in * (k + 1) / 4
    SIXTY_FOUR,     // in 0-65, out 66: out = the sum of the inputs
    SIXTY_FIVE,     // in 0-66, out 67: refused before it runs
    CV_SIXTY_FOUR,  // audio in 0-63, CV in 64-65, out 66: out = the sum of every input
    CV_SIXTY_FIVE,  // audio in 0-63, CV in 64-66, out 67: refused before it runs
    EMPTY_MAIN,     // in 0-1, out 2-3 (pg:mainInput names a group without members): out_c = in_c
    FIVE_ONE,       // a 5.1 group each side, its members in scrambled port order: each output is the input of its designation
    AMBI_FIRST,     // a first-order ambisonic group each side, in 0-3 scrambled, out 4-7 in ACN order: out ACN k = in ACN k
    AMBI_MIXED,     // a mixed-order (BH2P1) ambisonic group each side, in 0-5, out 6-11: out_c = in_c
    WIDE_SIXTY_FIVE,// in 0-64 one group, out 65: refused before it runs
    N_TWINS
};

struct layout
{
    enum twin twin;
    float *port[MAX_PORTS];
};

static LV2_Handle instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *features);
static void connect_port(LV2_Handle handle, uint32_t port, void *data);
static void run(LV2_Handle handle, uint32_t n);
static void cleanup(LV2_Handle handle);

#define TWIN(name) { LAYOUT_URI name, instantiate, connect_port, NULL, run, NULL, cleanup, NULL }
static const LV2_Descriptor descriptors[N_TWINS] =
{
    [MAIN_AUX] = TWIN("#main-aux"), [TWO_MONO] = TWIN("#two-mono"), [FOUR_FOUR] = TWIN("#four-four"),
    [FOUR_TWO] = TWIN("#four-two"), [MONO_STEREO] = TWIN("#mono-stereo"), [TWO_ONE] = TWIN("#two-one"),
    [BANDS] = TWIN("#bands"), [SIXTY_FOUR] = TWIN("#sixty-four"), [SIXTY_FIVE] = TWIN("#sixty-five"),
    [CV_SIXTY_FOUR] = TWIN("#cv-sixty-four"), [CV_SIXTY_FIVE] = TWIN("#cv-sixty-five"), [EMPTY_MAIN] = TWIN("#empty-main"),
    [FIVE_ONE] = TWIN("#five-one"), [AMBI_FIRST] = TWIN("#ambi-first"), [AMBI_MIXED] = TWIN("#ambi-mixed"),
    [WIDE_SIXTY_FIVE] = TWIN("#wide-sixty-five"),
};

/* five-one: the output port of each designation, then the input port of the same one (L C R BL BR LFE) */
static const uint32_t FIVE_ONE_OUT[6] = { 8, 6, 7, 11, 10, 9 }, FIVE_ONE_IN[6] = { 1, 3, 5, 4, 0, 2 };
/* ambi-first: the input port of ACN 0 to 3 */
static const uint32_t AMBI_FIRST_IN[4] = { 1, 0, 3, 2 };

static LV2_Handle instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *features)
{
    struct layout *l = calloc(1, sizeof(*l));

    (void)rate;
    (void)path;
    (void)features;
    if (l)
        l->twin = (enum twin)(d - descriptors);
    return l;
}

static void connect_port(LV2_Handle handle, uint32_t port, void *data)
{
    struct layout *l = handle;

    if (port < MAX_PORTS)
        l->port[port] = data;
}

/* out[i] = the sum of ports first..last at frame i */
static void sum(struct layout *l, float *out, uint32_t first, uint32_t last, uint32_t n)
{
    uint32_t i, k;

    for (i = 0; i < n; i++)
    {
        float v = 0.0f;

        for (k = first; k <= last; k++)
            v += l->port[k][i];
        out[i] = v;
    }
}

static void run(LV2_Handle handle, uint32_t n)
{
    struct layout *l = handle;
    float **p = l->port;
    uint32_t c, i, k;

    switch (l->twin)
    {
    case MAIN_AUX:
        for (c = 0; c < 2; c++)
            for (i = 0; i < n; i++)
                p[6 + c][i] = p[c][i] + p[2 + c][i] + p[4 + c][i];
        break;
    case TWO_MONO:
    case TWO_ONE:
        sum(l, p[2], 0, 1, n);
        break;
    case FOUR_FOUR:
        for (c = 0; c < 4; c++)
            memmove(p[4 + c], p[c], n * sizeof(float));
        break;
    case FOUR_TWO:
        for (c = 0; c < 2; c++)
            for (i = 0; i < n; i++)
                p[4 + c][i] = p[c][i] + p[c + 2][i];
        break;
    case MONO_STEREO:
        for (i = 0; i < n; i++)
        {
            const float x = p[0][i];

            p[1][i] = x;
            p[2][i] = -x;
        }
        break;
    case BANDS:
        for (c = 0; c < 2; c++)
            for (i = 0; i < n; i++)
            {
                const float x = p[c][i];

                p[2 + c][i] = x;
                for (k = 0; k < 3; k++)
                    p[4 + 2 * k + c][i] = x * (float)(k + 1) / 4.0f;
            }
        break;
    case SIXTY_FOUR:
    case CV_SIXTY_FOUR:
        sum(l, p[66], 0, 65, n);
        break;
    case EMPTY_MAIN:
        for (c = 0; c < 2; c++)
            memmove(p[2 + c], p[c], n * sizeof(float));
        break;
    case FIVE_ONE:
        for (c = 0; c < 6; c++)
            memmove(p[FIVE_ONE_OUT[c]], p[FIVE_ONE_IN[c]], n * sizeof(float));
        break;
    case AMBI_FIRST:
        for (c = 0; c < 4; c++)
            memmove(p[4 + c], p[AMBI_FIRST_IN[c]], n * sizeof(float));
        break;
    case AMBI_MIXED:
        for (c = 0; c < 6; c++)
            memmove(p[6 + c], p[c], n * sizeof(float));
        break;
    default:
        break;
    }
}

static void cleanup(LV2_Handle handle)
{
    free(handle);
}

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    return index < N_TWINS ? &descriptors[index] : NULL;
}
