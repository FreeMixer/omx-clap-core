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
 * The ll-plugins MIDI port fixture: MIDI through llext:MidiPort over an LV2_MIDI buffer, read as zynadd reads it (a
 * position walked through the buffer's bytes: a double timestamp, a size_t size, the bytes). See lv2_ll_midi_fixture.h
 * for what each descriptor does and what it records.
 */
#include <stdlib.h>
#include <string.h>
#include <lv2/core/lv2.h>
#include "../../core/lv2_ll_midi.h"
#include "lv2_ll_midi_fixture.h"

#define EXPORT __attribute__((visibility("default")))
#define EVENT_HEAD (sizeof(double) + sizeof(size_t))

EXPORT struct lv2_ll_midi_fixture lv2_ll_midi_fixture;

enum { E_IN_L, E_IN_R, E_OUT_L, E_OUT_R, E_MIDI_IN, E_MIDI_OUT };
enum { T_IN, T_OUT, T_MIDI_A, T_MIDI_B };

struct echo
{
    const float *in[2];
    float *out[2];
    LV2_MIDI *midi_in, *midi_out, *b;
};

static LV2_Handle instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *features)
{
    (void)d;
    (void)rate;
    (void)path;
    (void)features;
    lv2_ll_midi_fixture.instantiated++;
    return calloc(1, sizeof(struct echo));
}

static void echo_connect(LV2_Handle h, uint32_t port, void *data)
{
    struct echo *e = h;

    switch (port)
    {
    case E_IN_L: e->in[0] = data; break;
    case E_IN_R: e->in[1] = data; break;
    case E_OUT_L: e->out[0] = data; break;
    case E_OUT_R: e->out[1] = data; break;
    case E_MIDI_IN: e->midi_in = data; break;
    case E_MIDI_OUT: e->midi_out = data; break;
    default: break;
    }
}

static void record(uint32_t i, double stamp, size_t size, const uint8_t *data)
{
    if (i >= LV2_LL_MIDI_FIXTURE_SEEN)
        return;
    lv2_ll_midi_fixture.frame[i] = (uint32_t)stamp;
    lv2_ll_midi_fixture.size[i] = (uint32_t)size;
    memset(lv2_ll_midi_fixture.data[i], 0, sizeof(lv2_ll_midi_fixture.data[i]));
    memcpy(lv2_ll_midi_fixture.data[i], data, size < 4u ? size : 4u);
}

/* one event appended to an output buffer the ll way, when it fits */
static void write_out(LV2_MIDI *out, double stamp, size_t size, const uint8_t *data)
{
    uint8_t *at = out->data + out->size;

    if (EVENT_HEAD + size > out->capacity - out->size)
        return;
    memcpy(at, &stamp, sizeof(stamp));
    memcpy(at + sizeof(stamp), &size, sizeof(size));
    memcpy(at + EVENT_HEAD, data, size);
    out->size += (uint32_t)(EVENT_HEAD + size);
    out->event_count++;
}

/* the output spoilt as the test asks, after it was written */
static void misbehave(LV2_MIDI *out)
{
    static uint8_t moved[256];
    const double bad = -1.0;
    const size_t huge = 65520;

    switch (lv2_ll_midi_fixture.misbehave)
    {
    case LV2_LL_MIDI_FIXTURE_MOVED:
        memcpy(moved, out->data, out->size < sizeof(moved) ? out->size : sizeof(moved));
        out->data = moved;
        break;
    case LV2_LL_MIDI_FIXTURE_OVERSIZE:
        if (out->size >= EVENT_HEAD)
        {
            memcpy(out->data + sizeof(double), &huge, sizeof(huge));
            out->size = out->capacity;
        }
        break;
    case LV2_LL_MIDI_FIXTURE_STAMP:
        if (out->size >= EVENT_HEAD)
            memcpy(out->data, &bad, sizeof(bad));
        break;
    case LV2_LL_MIDI_FIXTURE_COUNT:
        if (out->event_count)
            out->event_count--;
        break;
    default:
        break;
    }
}

static void echo_run(LV2_Handle h, uint32_t n)
{
    struct echo *e = h;
    uint32_t k, pos = 0;

    lv2_ll_midi_fixture.runs++;
    lv2_ll_midi_fixture.n = 0;
    for (k = 0; k < 2; k++)
        memcpy(e->out[k], e->in[k], n * sizeof(float));
    e->midi_out->event_count = 0;
    e->midi_out->size = 0;
    // read as zynadd does: event_count events walked from the start of the data, each a double, a size_t and the bytes
    for (k = 0; k < e->midi_in->event_count && pos + EVENT_HEAD <= e->midi_in->size; k++)
    {
        const uint8_t *at = e->midi_in->data + pos;
        uint8_t m[4] = { 0 };
        double stamp;
        size_t size;

        memcpy(&stamp, at, sizeof(stamp));
        memcpy(&size, at + sizeof(stamp), sizeof(size));
        if (size > e->midi_in->size - pos - EVENT_HEAD)
            break;
        record(lv2_ll_midi_fixture.n++, stamp, size, at + EVENT_HEAD);
        // a note on or off goes out one key up, so what comes out proves the path rather than a copy
        memcpy(m, at + EVENT_HEAD, size < 4u ? size : 4u);
        if (size == 3 && ((m[0] & 0xf0) == 0x90 || (m[0] & 0xf0) == 0x80) && m[1] < 127)
            m[1]++;
        write_out(e->midi_out, stamp, size, size <= 4u ? m : at + EVENT_HEAD);
        pos += (uint32_t)(EVENT_HEAD + size);
    }
    misbehave(e->midi_out);
}

static void two_connect(LV2_Handle h, uint32_t port, void *data)
{
    struct echo *e = h;

    switch (port)
    {
    case T_IN: e->in[0] = data; break;
    case T_OUT: e->out[0] = data; break;
    case T_MIDI_A: e->midi_in = data; break;
    case T_MIDI_B: e->b = data; break;
    default: break;
    }
}

static void two_run(LV2_Handle h, uint32_t n)
{
    struct echo *e = h;

    lv2_ll_midi_fixture.runs++;
    memcpy(e->out[0], e->in[0], n * sizeof(float));
}

static void cleanup(LV2_Handle h)
{
    free(h);
}

static const LV2_Descriptor ECHO =
{
    LV2_LL_MIDI_FIXTURE_URI "#echo", instantiate, echo_connect, NULL, echo_run, NULL, cleanup, NULL
};

static const LV2_Descriptor TWO_IN =
{
    LV2_LL_MIDI_FIXTURE_URI "#two-in", instantiate, two_connect, NULL, two_run, NULL, cleanup, NULL
};

EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    static const LV2_Descriptor *const all[] = { &ECHO, &TWO_IN };

    return index < sizeof(all) / sizeof(all[0]) ? all[index] : NULL;
}
