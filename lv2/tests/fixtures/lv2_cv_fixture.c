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
 * lv2_cv_fixture.c: the CV plugin of omx-cv-fixture.lv2, a test fixture and never a shipped plugin. Port 0 is the audio
 * input, 1 the audio output, 2 the CV input `mod` and 3 the CV output `env`:
 *
 *   out[i] = in[i] * mod[i]
 *   e      = e + 0.125 * (|out[i]| - e),   env[i] = e
 *
 * so the audio output follows the CV input frame by frame, and the CV output carries a state that runs on across run()
 * calls (reset by activate). The #many twin has sixteen CV inputs at ports 2 to 17 and the #many-out twin sixteen CV
 * outputs there (ams-lv2's multiphase_lfo has sixteen, #268); each port carries a weight of its own, so a value read
 * proves which of the sixteen reached run():
 *
 *   many:      out[i] = in[i] + sum over k of (k + 1) * cv_k[i]
 *   many-out:  out[i] = in[i],  cv_k[i] = (k + 1) * in[i]
 *
 * Nothing is allocated or locked in run().
 */

#include <math.h>
#include <stdlib.h>

#include <lv2/core/lv2.h>

#define CV_FIXTURE_URI "urn:openmixer:test:cv-fixture"
#define MANY 16u

enum twin { BASE, MANY_IN, MANY_OUT, N_TWINS };

struct cv_fixture
{
    enum twin twin;
    const float *in, *mod;
    float *out, *env;
    const float *cv[MANY];  // many: the sixteen CV inputs
    float *cvo[MANY];       // many-out: the sixteen CV outputs
    float e;
};

static const LV2_Descriptor descriptors[N_TWINS];

static LV2_Handle instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *features)
{
    struct cv_fixture *s = calloc(1, sizeof(*s));

    (void)rate;
    (void)path;
    (void)features;
    if (s)
        s->twin = (enum twin)(d - descriptors);
    return s;
}

static void connect_port(LV2_Handle handle, uint32_t port, void *data)
{
    struct cv_fixture *s = handle;

    if (s->twin != BASE && port >= 2 && port < 2 + MANY)
    {
        if (s->twin == MANY_IN)
            s->cv[port - 2] = data;
        else
            s->cvo[port - 2] = data;
        return;
    }
    switch (port)
    {
    case 0: s->in = data; break;
    case 1: s->out = data; break;
    case 2: s->mod = data; break;
    case 3: s->env = data; break;
    default: break;
    }
}

static void activate(LV2_Handle handle)
{
    ((struct cv_fixture *)handle)->e = 0.0f;
}

static void run(LV2_Handle handle, uint32_t n)
{
    struct cv_fixture *s = handle;
    float e = s->e;
    uint32_t i, k;

    if (s->twin == MANY_IN)
    {
        for (i = 0; i < n; i++)
        {
            float v = s->in[i];

            for (k = 0; k < MANY; k++)
                v += (float)(k + 1) * s->cv[k][i];
            s->out[i] = v;
        }
        return;
    }
    if (s->twin == MANY_OUT)
    {
        for (i = 0; i < n; i++)
        {
            s->out[i] = s->in[i];
            for (k = 0; k < MANY; k++)
                s->cvo[k][i] = (float)(k + 1) * s->in[i];
        }
        return;
    }
    for (i = 0; i < n; i++)
    {
        const float y = s->in[i] * s->mod[i];

        s->out[i] = y;
        e = e + 0.125f * (fabsf(y) - e);
        s->env[i] = e;
    }
    s->e = e;
}

static void deactivate(LV2_Handle handle)
{
    (void)handle;
}

static void cleanup(LV2_Handle handle)
{
    free(handle);
}

#define TWIN(name) { CV_FIXTURE_URI name, instantiate, connect_port, activate, run, deactivate, cleanup, NULL }
static const LV2_Descriptor descriptors[N_TWINS] = {
    [BASE] = TWIN(""), [MANY_IN] = TWIN("#many"), [MANY_OUT] = TWIN("#many-out")
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    return index < N_TWINS ? &descriptors[index] : NULL;
}
