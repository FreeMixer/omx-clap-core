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
 * lv2_fakes.c — the fakes of lv2_fakes.h as the bundle omx-lv2-fakes.lv2. A test fixture, never a shipped plugin.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

#include "lv2_fakes.h"

#define EXPORT __attribute__((visibility("default")))

static struct fake *g_last;
static char g_trace[512];

static void tr(const char *ev)
{
    if (g_trace[0])
        strncat(g_trace, ",", sizeof(g_trace) - strlen(g_trace) - 1);
    strncat(g_trace, ev, sizeof(g_trace) - strlen(g_trace) - 1);
}

EXPORT struct fake *lv2_fakes_last(void)
{
    return g_last;
}

EXPORT const char *lv2_fakes_trace(void)
{
    return g_trace;
}

EXPORT void lv2_fakes_trace_clear(void)
{
    g_trace[0] = '\0';
}

static LV2_Handle f_instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *features)
{
    struct fake *f = calloc(1, sizeof(*f));

    (void)path;
    if (!f)
        return NULL;
    f->rate = rate;
    f->kind = (enum fake_kind)(d->URI[strlen(d->URI) - 1] - '0');
    f->legs = strstr(d->URI, "stereo") ? 2 : 1;
    if (f->kind == K_PROPS)
        f->kind = K_PAD;
    if (f->kind == K_NAN_ALWAYS)
    {
        f->kind = K_NAN;
        f->nan_on = 1;
    }
    for (int i = 0; features && features[i]; i++)
        if (!strcmp(features[i]->URI, LV2_WORKER__schedule))
            f->sched = features[i]->data;
    g_last = f;
    return f;
}

static void f_connect(LV2_Handle h, uint32_t port, void *data)
{
    struct fake *f = h;

    if (f->legs == 1)
    {
        switch (port)
        {
        case 0: f->in[0] = data; break;
        case 1: f->out[0] = data; break;
        case 2: f->latency = data; break;
        default: break;     // the props fake's controls: connected, never read
        }
        return;
    }
    switch (port)
    {
    case 0: f->in[0] = data; break;
    case 1: f->in[1] = data; break;
    case 2: f->out[0] = data; break;
    case 3: f->out[1] = data; break;
    case 4: f->latency = data; break;
    }
}

static void f_run(LV2_Handle h, uint32_t n)
{
    struct fake *f = h;

    f->runs++;
    if (f->legs == 2)
        f->saw_in_r0 = f->in[1][0];
    for (uint32_t k = 0; k < f->legs; k++)
    {
        switch (f->kind)
        {
        case K_BROKEN:      // inPlaceBroken: the output is cleared before the input is read
            for (uint32_t i = 0; i < n; i++)
                f->out[k][i] = 0.0f;
            for (uint32_t i = 0; i < n; i++)
                f->out[k][i] += f->in[k][i] * PAD_GAIN;
            break;
        case K_NAN:
            for (uint32_t i = 0; i < n; i++)
                f->out[k][i] = f->nan_on ? NAN : f->in[k][i] * PAD_GAIN;
            break;
        case K_HOT:
            for (uint32_t i = 0; i < n; i++)
                f->out[k][i] = f->in[k][i] * 100.0f;        // +40 dB
            break;
        default:
            for (uint32_t i = 0; i < n; i++)
                f->out[k][i] = f->in[k][i] * PAD_GAIN;
        }
    }
    if (f->latency)
        *f->latency = (float)((int)pad_latency_frames(f->rate) + f->latency_bias);
#if defined(__x86_64__) || defined(__i386__)
    if (f->kind == K_CLOBBER)
        _mm_setcsr(_mm_getcsr() & ~0x8040u);
#endif
    if (f->kind == K_WORKER && f->sched)
    {
        char ev[16];

        snprintf(ev, sizeof(ev), "run%u", f->seq);
        tr(ev);
        f->sched->schedule_work(f->sched->handle, sizeof(f->seq), &f->seq);
        f->seq++;
    }
}

static void f_cleanup(LV2_Handle h)
{
    if (g_last == h)
        g_last = NULL;
    free(h);
}

static LV2_Worker_Status w_work(LV2_Handle h, LV2_Worker_Respond_Function respond, LV2_Worker_Respond_Handle rh,
                                uint32_t size, const void *data)
{
    struct fake *f = h;
    uint32_t seq;
    char ev[16];

    memcpy(&seq, data, sizeof(seq));
    snprintf(ev, sizeof(ev), "work%u", seq);
    tr(ev);
    f->respond = respond;
    f->respond_handle = rh;
    return respond(rh, size, data);
}

static LV2_Worker_Status w_response(LV2_Handle h, uint32_t size, const void *data)
{
    uint32_t seq;
    char ev[16];

    (void)h;
    (void)size;
    memcpy(&seq, data, sizeof(seq));
    snprintf(ev, sizeof(ev), "resp%u", seq);
    tr(ev);
    return LV2_WORKER_SUCCESS;
}

static LV2_Worker_Status w_end_run(LV2_Handle h)
{
    struct fake *f = h;
    char ev[16];

    snprintf(ev, sizeof(ev), "end%u", f->seq - 1u);
    tr(ev);
    return LV2_WORKER_SUCCESS;
}

static const LV2_Worker_Interface WORKER = { w_work, w_response, w_end_run };

static const void *f_extension_data(const char *uri)
{
    return strcmp(uri, LV2_WORKER__interface) == 0 ? &WORKER : NULL;
}

#define DESC(uri, ext) { uri, f_instantiate, f_connect, NULL, f_run, NULL, f_cleanup, ext }
static const LV2_Descriptor DESCRIPTORS[] =
{
    DESC(FAKE_PAD, NULL), DESC(FAKE_PAD2, NULL), DESC(FAKE_BROKEN, NULL), DESC(FAKE_CLOBBER, NULL),
    DESC(FAKE_NAN, NULL), DESC(FAKE_HOT, NULL), DESC(FAKE_WORKER, f_extension_data), DESC(FAKE_NAN_ALWAYS, NULL), DESC(FAKE_PROPS, NULL),
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    return index < sizeof(DESCRIPTORS) / sizeof(DESCRIPTORS[0]) ? &DESCRIPTORS[index] : NULL;
}
