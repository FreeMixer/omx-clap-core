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
 * The MIDI fixture: one plugin with an audio pass-through, one MIDI input and one MIDI output. Every event the input
 * sequence carries is written to the output sequence at the same frame, byte for byte, in the same order, and the audio
 * is copied from input to output. A second block with no input writes nothing.
 *
 * Two things a test reads back. The last run's input, event by event, with the size of each message as the adapter
 * wrote it: what the adapter made of a CLAP event shows here, where the output cannot tell a 2-byte message from a 3-byte
 * one. And one malformed output the next run writes after the echo, chosen by lv2_midi_echo_extra: a message the adapter
 * must not pass on.
 */

#include <stdlib.h>
#include <string.h>

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>

#define EXPORT __attribute__((visibility("default")))

#define ECHO_URI "urn:openmixer:test:midi-echo"

#define SEEN_MAX 64u            // the input events kept for the test, the first SEEN_MAX of the last run
#define SEEN_BYTES 8u           // the bytes kept of each

enum { P_IN, P_OUT, P_MIDI_IN, P_MIDI_OUT };

enum { EXTRA_NONE, EXTRA_4_BYTES, EXTRA_EMPTY, EXTRA_DATA_FIRST, EXTRA_BEATS };

struct echo
{
    const float *in;
    float *out;
    const LV2_Atom_Sequence *midi_in;
    LV2_Atom_Sequence *midi_out;
    LV2_URID midi;
};

struct seen
{
    uint32_t n;
    int64_t frame[SEEN_MAX];
    uint32_t size[SEEN_MAX];
    uint8_t data[SEEN_MAX][SEEN_BYTES];
};

static struct seen g_seen;
static int g_extra;

static LV2_Handle echo_instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *features)
{
    struct echo *e = calloc(1, sizeof(*e));
    const LV2_URID_Map *map = NULL;
    int i;

    (void)d;
    (void)rate;
    (void)path;
    for (i = 0; e && features && features[i]; i++)
        if (!strcmp(features[i]->URI, LV2_URID__map))
            map = features[i]->data;
    if (e && map)
        e->midi = map->map(map->handle, LV2_MIDI__MidiEvent);
    return e;
}

static void echo_connect(LV2_Handle h, uint32_t port, void *data)
{
    struct echo *e = h;

    switch (port)
    {
    case P_IN: e->in = data; break;
    case P_OUT: e->out = data; break;
    case P_MIDI_IN: e->midi_in = data; break;
    case P_MIDI_OUT: e->midi_out = data; break;
    default: break;
    }
}

static void record(const LV2_Atom_Event *ev)
{
    const uint32_t i = g_seen.n++;

    if (i < SEEN_MAX)
    {
        g_seen.frame[i] = ev->time.frames;
        g_seen.size[i] = ev->body.size;
        memcpy(g_seen.data[i], LV2_ATOM_BODY_CONST(&ev->body), ev->body.size < SEEN_BYTES ? ev->body.size : SEEN_BYTES);
    }
}

/* one malformed event after the echo, of the type the test chose: its bytes as they are, the body's size as it is */
static void append_extra(struct echo *e, uint32_t cap)
{
    struct { LV2_Atom_Event ev; uint8_t data[8]; } raw;
    static const uint8_t four[4] = { 0x90, 0x3c, 0x64, 0x00 }, data_first[2] = { 0x3c, 0x40 };

    memset(&raw, 0, sizeof(raw));
    raw.ev.time.frames = 0;
    raw.ev.body.type = e->midi;
    switch (g_extra)
    {
    case EXTRA_4_BYTES: raw.ev.body.size = sizeof(four); memcpy(raw.data, four, sizeof(four)); break;
    case EXTRA_EMPTY: raw.ev.body.size = 0; break;
    case EXTRA_DATA_FIRST: raw.ev.body.size = sizeof(data_first); memcpy(raw.data, data_first, sizeof(data_first)); break;
    case EXTRA_BEATS:
        // a sequence in beats: the adapter passes none of its events
        e->midi_out->body.unit = 1;
        raw.ev.body.size = 3;
        memcpy(raw.data, four, 3);
        break;
    default: return;
    }
    lv2_atom_sequence_append_event(e->midi_out, cap, &raw.ev);
}

static void echo_run(LV2_Handle h, uint32_t n)
{
    struct echo *e = h;
    const uint32_t cap = e->midi_out->atom.size;

    memcpy(e->out, e->in, n * sizeof(float));
    g_seen.n = 0;
    lv2_atom_sequence_clear(e->midi_out);
    LV2_ATOM_SEQUENCE_FOREACH(e->midi_in, ev)
    {
        record(ev);
        lv2_atom_sequence_append_event(e->midi_out, cap, ev);
    }
    if (g_extra)
        append_extra(e, cap);
}

static void echo_cleanup(LV2_Handle h)
{
    free(h);
}

static const LV2_Descriptor DESCRIPTOR =
{
    .URI = ECHO_URI,
    .instantiate = echo_instantiate,
    .connect_port = echo_connect,
    .run = echo_run,
    .cleanup = echo_cleanup,
};

EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    return index == 0 ? &DESCRIPTOR : NULL;
}

/* the last run's input: how many events it had, and the one at `i`, with its frame, size and first bytes */
EXPORT uint32_t lv2_midi_echo_seen_count(void)
{
    return g_seen.n;
}

EXPORT int lv2_midi_echo_seen(uint32_t i, int64_t *frame, uint32_t *size, uint8_t *bytes)
{
    if (i >= g_seen.n || i >= SEEN_MAX)
        return -1;
    *frame = g_seen.frame[i];
    *size = g_seen.size[i];
    memcpy(bytes, g_seen.data[i], SEEN_BYTES);
    return 0;
}

/* the malformed output of the next runs: one of the EXTRA_ kinds, EXTRA_NONE for none */
EXPORT void lv2_midi_echo_extra(int kind)
{
    g_extra = kind;
}
