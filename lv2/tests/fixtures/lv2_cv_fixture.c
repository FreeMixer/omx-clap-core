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
 * calls (reset by activate). The #many twin declares nine CV inputs and has no descriptor: the host refuses it from the
 * TTL. Nothing is allocated or locked in run().
 */

#include <math.h>
#include <stdlib.h>

#include <lv2/core/lv2.h>

#define CV_FIXTURE_URI "urn:openmixer:test:cv-fixture"

struct cv_fixture
{
    const float *in, *mod;
    float *out, *env;
    float e;
};

static LV2_Handle instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *features)
{
    (void)d;
    (void)rate;
    (void)path;
    (void)features;
    return calloc(1, sizeof(struct cv_fixture));
}

static void connect_port(LV2_Handle handle, uint32_t port, void *data)
{
    struct cv_fixture *s = handle;

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
    uint32_t i;

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

static const LV2_Descriptor g_descriptor = {
    CV_FIXTURE_URI, instantiate, connect_port, activate, run, deactivate, cleanup, NULL
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    return index == 0 ? &g_descriptor : NULL;
}
