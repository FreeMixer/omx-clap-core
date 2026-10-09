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
 * lv2_sidechain_fixture.c: the side-chain plugins of omx-sidechain-fixture.lv2, a test fixture and never a shipped plugin.
 * Every URI of the bundle runs the same effect: out = main + side, per channel. Ports 0 and 1 are the main input, 2 and 3
 * the outputs, 4 and 5 the side chain's first and second channel; a mono side chain (port 4 alone) feeds both channels.
 * Nothing is allocated or locked in run().
 */

#include <stdlib.h>

#include <lv2/core/lv2.h>

#define N_PLUGINS 5u

struct sidechain
{
    const float *in[2];
    const float *side[2];
    float *out[2];
};

static LV2_Handle instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *features)
{
    (void)d;
    (void)rate;
    (void)path;
    (void)features;
    return calloc(1, sizeof(struct sidechain));
}

static void connect_port(LV2_Handle handle, uint32_t port, void *data)
{
    struct sidechain *s = handle;

    switch (port)
    {
    case 0: s->in[0] = data; break;
    case 1: s->in[1] = data; break;
    case 2: s->out[0] = data; break;
    case 3: s->out[1] = data; break;
    case 4: s->side[0] = data; break;
    case 5: s->side[1] = data; break;
    default: break;
    }
}

static void activate(LV2_Handle handle)
{
    (void)handle;
}

static void run(LV2_Handle handle, uint32_t n)
{
    struct sidechain *s = handle;
    uint32_t c, i;

    for (c = 0; c < 2; c++)
    {
        const float *side = s->side[c] ? s->side[c] : s->side[0];

        for (i = 0; i < n; i++)
            s->out[c][i] = s->in[c][i] + (side ? side[i] : 0.0f);
    }
}

static void deactivate(LV2_Handle handle)
{
    (void)handle;
}

static void cleanup(LV2_Handle handle)
{
    free(handle);
}

static const LV2_Descriptor g_descriptors[N_PLUGINS] = {
    { "urn:openmixer:test:sidechain-fixture", instantiate, connect_port, activate, run, deactivate, cleanup, NULL },
    { "urn:openmixer:test:sidechain-fixture#mono", instantiate, connect_port, activate, run, deactivate, cleanup, NULL },
    { "urn:openmixer:test:sidechain-fixture#wide", instantiate, connect_port, activate, run, deactivate, cleanup, NULL },
    { "urn:openmixer:test:sidechain-fixture#extra", instantiate, connect_port, activate, run, deactivate, cleanup, NULL },
    { "urn:openmixer:test:sidechain-fixture#port-of", instantiate, connect_port, activate, run, deactivate, cleanup, NULL },
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    return index < N_PLUGINS ? &g_descriptors[index] : NULL;
}
