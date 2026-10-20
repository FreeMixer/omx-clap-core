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
 * The event-port fixture: MIDI through ev:EventPort, the event extension's port, read through uri-map (the MIDI type's
 * id) and the event extension's helpers. See lv2_event_fixture.h for what each descriptor does and what it records.
 */

#include <stdlib.h>
#include <string.h>

#include <lv2/core/lv2.h>
#include <lv2/event/event-helpers.h>
#include <lv2/event/event.h>
#include <lv2/midi/midi.h>
#include <lv2/uri-map/uri-map.h>

#include "lv2_event_fixture.h"

#define EXPORT __attribute__((visibility("default")))

// uri-map and event are deprecated upstream: the fixture is written to them as the plugins it stands for are
LV2_DISABLE_DEPRECATION_WARNINGS

EXPORT struct lv2_event_fixture lv2_event_fixture;

enum { E_IN_L, E_IN_R, E_OUT_L, E_OUT_R, E_MIDI_IN, E_MIDI_OUT };
enum { T_IN, T_OUT, T_MIDI_A, T_MIDI_B, T_OUT_A, T_OUT_B };

struct echo
{
    const float *in[2];
    float *out[2];
    LV2_Event_Buffer *midi_in, *b, *out_a, *out_b;
    uint16_t midi;                  // midi:MidiEvent's id through uri-map, 0 without it: no event is MIDI then
};

static LV2_Handle instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *features)
{
    struct echo *e = calloc(1, sizeof(*e));
    const LV2_URI_Map_Feature *map = NULL;
    int i;

    (void)d;
    (void)rate;
    (void)path;
    for (i = 0; e && features && features[i]; i++)
        if (!strcmp(features[i]->URI, LV2_URI_MAP_URI))
            map = features[i]->data;
    if (e && map)
        e->midi = (uint16_t)map->uri_to_id(map->callback_data, LV2_EVENT_URI, LV2_MIDI__MidiEvent);
    lv2_event_fixture.midi_type = e ? e->midi : 0;
    lv2_event_fixture.instantiated++;
    return e;
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
    case E_MIDI_OUT: e->out_a = data; break;
    default: break;
    }
}

static void record(uint32_t i, const LV2_Event *ev, const uint8_t *data)
{
    if (i >= LV2_EVENT_FIXTURE_SEEN)
        return;
    lv2_event_fixture.frame[i] = ev->frames;
    lv2_event_fixture.size[i] = ev->size;
    memset(lv2_event_fixture.data[i], 0, sizeof(lv2_event_fixture.data[i]));
    memcpy(lv2_event_fixture.data[i], data, ev->size < 4u ? ev->size : 4u);
}

/* the output spoilt as the test asks, after it was written */
static void misbehave(LV2_Event_Buffer *out)
{
    static uint8_t moved[256];

    switch (lv2_event_fixture.misbehave)
    {
    case LV2_EVENT_FIXTURE_MOVED:
        memcpy(moved, out->data, out->size < sizeof(moved) ? out->size : sizeof(moved));
        out->data = moved;
        break;
    case LV2_EVENT_FIXTURE_OVERSIZE:
        if (out->size >= sizeof(LV2_Event))
        {
            ((LV2_Event *)out->data)->size = 65520;
            out->size = out->capacity;
        }
        break;
    case LV2_EVENT_FIXTURE_STAMP:
        out->stamp_type = 1;        // any type but LV2_EVENT_AUDIO_STAMP: stamps that are not frames
        break;
    case LV2_EVENT_FIXTURE_COUNT:
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
    LV2_Event_Iterator in, out;
    uint32_t k;

    lv2_event_fixture.runs++;
    lv2_event_fixture.n = lv2_event_fixture.other = 0;
    for (k = 0; k < 2; k++)
        memcpy(e->out[k], e->in[k], n * sizeof(float));
    lv2_event_begin(&out, e->out_a);
    for (lv2_event_begin(&in, e->midi_in); lv2_event_is_valid(&in); lv2_event_increment(&in))
    {
        uint8_t *data, m[4] = { 0 };
        const LV2_Event *ev = lv2_event_get(&in, &data);

        if (!e->midi || ev->type != e->midi)
        {
            lv2_event_fixture.other++;
            continue;
        }
        record(lv2_event_fixture.n++, ev, data);
        // a note on or off goes out one key up, so what comes out proves the path rather than a copy
        memcpy(m, data, ev->size < 4u ? ev->size : 4u);
        if (ev->size == 3 && ((m[0] & 0xf0) == 0x90 || (m[0] & 0xf0) == 0x80) && m[1] < 127)
            m[1]++;
        lv2_event_write(&out, ev->frames, 0, e->midi, ev->size, ev->size <= 4u ? m : data);
    }
    misbehave(e->out_a);
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
    case T_OUT_A: e->out_a = data; break;
    case T_OUT_B: e->out_b = data; break;
    default: break;
    }
}

static void two_run(LV2_Handle h, uint32_t n)
{
    struct echo *e = h;
    LV2_Event_Iterator in, a, b;

    lv2_event_fixture.runs++;
    lv2_event_fixture.n = lv2_event_fixture.b = 0;
    memcpy(e->out[0], e->in[0], n * sizeof(float));
    for (lv2_event_begin(&in, e->b); lv2_event_is_valid(&in); lv2_event_increment(&in))
        lv2_event_fixture.b++;
    lv2_event_begin(&a, e->out_a);
    lv2_event_begin(&b, e->out_b);
    for (lv2_event_begin(&in, e->midi_in); lv2_event_is_valid(&in); lv2_event_increment(&in))
    {
        uint8_t *data;
        const LV2_Event *ev = lv2_event_get(&in, &data);

        record(lv2_event_fixture.n++, ev, data);
        lv2_event_write(&a, ev->frames, 0, ev->type, ev->size, data);
        lv2_event_write(&b, ev->frames, 0, ev->type, ev->size, data);
    }
}

static void cleanup(LV2_Handle h)
{
    free(h);
}

static const LV2_Descriptor ECHO =
{
    LV2_EVENT_FIXTURE_URI "#echo", instantiate, echo_connect, NULL, echo_run, NULL, cleanup, NULL
};

static const LV2_Descriptor TWO =
{
    LV2_EVENT_FIXTURE_URI "#two", instantiate, two_connect, NULL, two_run, NULL, cleanup, NULL
};

EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    return index == 0 ? &ECHO : index == 1 ? &TWO : NULL;
}

LV2_RESTORE_WARNINGS
