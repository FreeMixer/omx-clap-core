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
 * lv2_hints_fixture.c — the pads of lv2_hints_fixture.h, as the bundles omx-hints-fixture.lv2 and, built with
 * HINTS_REFUSED, omx-hints-refused.lv2. Each copies its input to its output. A test fixture, never a shipped plugin.
 */
#include <stdlib.h>
#include <string.h>

#include <lv2/core/lv2.h>

#include "lv2_hints_fixture.h"

struct pad
{
    const float *in;
    float *out;
};

static LV2_Handle instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *features)
{
    (void)d;
    (void)rate;
    (void)path;
    (void)features;
    return calloc(1, sizeof(struct pad));
}

static void connect(LV2_Handle h, uint32_t port, void *data)
{
    struct pad *p = h;

    if (port == 0)
        p->in = data;
    else if (port == 1)
        p->out = data;
}

static void run(LV2_Handle h, uint32_t n)
{
    struct pad *p = h;

    if (p->in && p->out && p->in != p->out)
        memcpy(p->out, p->in, n * sizeof(float));
}

static void cleanup(LV2_Handle h)
{
    free(h);
}

#define DESC(uri) { uri, instantiate, connect, NULL, run, NULL, cleanup, NULL }
static const LV2_Descriptor DESCRIPTORS[] =
{
#ifdef HINTS_REFUSED
    DESC(HINTS_WRONG_UNIT), DESC(HINTS_OUT_OF_RANGE),
#else
    DESC(HINTS_TWO), DESC(HINTS_NONE),
#endif
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    return index < sizeof(DESCRIPTORS) / sizeof(DESCRIPTORS[0]) ? &DESCRIPTORS[index] : NULL;
}
