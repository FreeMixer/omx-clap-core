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
 *
 * The instrument, urn:openmixer:test:midi-echo#synth: a MIDI input and a stereo output, no audio input. A note on is a
 * sine at the key's frequency from phase 0 on its frame, the velocity / 127 its amplitude, on both channels; its note off
 * is silence from its frame on. One voice: the last note on sounds.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>

#define EXPORT __attribute__((visibility("default")))

#define ECHO_URI "urn:openmixer:test:midi-echo"
#define SYNTH_URI ECHO_URI "#synth"

#define SEEN_MAX 64u            // the input events kept for the test, the first SEEN_MAX of the last run
#define SEEN_BYTES 8u           // the bytes kept of each

enum { P_IN, P_OUT, P_MIDI_IN, P_MIDI_OUT };

enum { EXTRA_NONE, EXTRA_4_BYTES, EXTRA_EMPTY, EXTRA_DATA_FIRST, EXTRA_BEATS, EXTRA_OVERSIZE };

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
    if (g_extra == EXTRA_OVERSIZE)
    {
        // a sequence that declares more than its buffer holds: zeros inside the buffer, a size far past its end
        memset(e->midi_out, 0, cap + sizeof(LV2_Atom));
        e->midi_out->atom.size = 1u << 20;
        return;
    }
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

enum { S_MIDI_IN, S_OUT_L, S_OUT_R };

struct synth
{
    const LV2_Atom_Sequence *midi_in;
    float *out[2];
    LV2_URID midi;
    double rate;
    int key;                    // the sounding key, -1 for none
    double amplitude;
    uint64_t age;               // frames since its note on
};

static LV2_Handle synth_instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *features)
{
    struct synth *s = calloc(1, sizeof(*s));
    const LV2_URID_Map *map = NULL;
    int i;

    (void)d;
    (void)path;
    for (i = 0; s && features && features[i]; i++)
        if (!strcmp(features[i]->URI, LV2_URID__map))
            map = features[i]->data;
    if (!s || !map)
    {
        free(s);
        return NULL;
    }
    s->midi = map->map(map->handle, LV2_MIDI__MidiEvent);
    s->rate = rate;
    s->key = -1;
    return s;
}

static void synth_connect(LV2_Handle h, uint32_t port, void *data)
{
    struct synth *s = h;

    switch (port)
    {
    case S_MIDI_IN: s->midi_in = data; break;
    case S_OUT_L: s->out[0] = data; break;
    case S_OUT_R: s->out[1] = data; break;
    default: break;
    }
}

static void synth_render(struct synth *s, uint32_t from, uint32_t to)
{
    const double hz = s->key >= 0 ? 440.0 * pow(2.0, (s->key - 69) / 12.0) : 0.0;
    uint32_t i;

    for (i = from; i < to; i++)
    {
        const float v = s->key >= 0 ? (float)(s->amplitude * sin(2.0 * M_PI * hz * (double)s->age++ / s->rate)) : 0.0f;

        s->out[0][i] = v;
        s->out[1][i] = v;
    }
}

static void synth_run(LV2_Handle h, uint32_t n)
{
    struct synth *s = h;
    uint32_t at = 0;

    LV2_ATOM_SEQUENCE_FOREACH(s->midi_in, ev)
    {
        const uint8_t *m = LV2_ATOM_BODY_CONST(&ev->body);
        uint32_t frame = ev->time.frames < 0 ? 0u : (uint32_t)ev->time.frames;

        if (ev->body.type != s->midi || ev->body.size < 3)
            continue;
        if (frame > n)
            frame = n;
        synth_render(s, at, frame);
        at = frame;
        if ((m[0] & 0xf0) == 0x90 && m[2] > 0)
        {
            s->key = m[1];
            s->amplitude = m[2] / 127.0;
            s->age = 0;
        }
        else if (((m[0] & 0xf0) == 0x80 || (m[0] & 0xf0) == 0x90) && m[1] == s->key)
        {
            s->key = -1;
        }
    }
    synth_render(s, at, n);
}

static const LV2_Descriptor SYNTH =
{
    .URI = SYNTH_URI,
    .instantiate = synth_instantiate,
    .connect_port = synth_connect,
    .run = synth_run,
    .cleanup = echo_cleanup,
};

/* The twins with more than one MIDI port. #two-in: audio in/out, MIDI A (index 2) and MIDI B (3); its witness counts
 * the events each run finds on A and on B. #two-out: audio in/out, a MIDI input (2) and MIDI outputs A (3) and B (4);
 * it echoes every input event to BOTH outputs, so a host that reads only the first output reads each once. */
struct lv2_midi_echo_two { unsigned runs, a, b, in; };
EXPORT struct lv2_midi_echo_two lv2_midi_echo_two;
struct two { const float *in; float *out; const LV2_Atom_Sequence *a, *b, *midi_in; LV2_Atom_Sequence *out_a, *out_b; };
static LV2_Handle two_instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *f)
{
    (void)d; (void)rate; (void)path; (void)f;
    return calloc(1, sizeof(struct two));
}
static void two_in_connect(LV2_Handle h, uint32_t port, void *data)
{
    struct two *t = h;
    switch (port) { case 0: t->in = data; break; case 1: t->out = data; break; case 2: t->a = data; break; case 3: t->b = data; break; default: break; }
}
static void two_in_run(LV2_Handle h, uint32_t n)
{
    struct two *t = h;
    lv2_midi_echo_two.runs++;
    lv2_midi_echo_two.a = lv2_midi_echo_two.b = 0;
    if (t->a) LV2_ATOM_SEQUENCE_FOREACH(t->a, ev) { (void)ev; lv2_midi_echo_two.a++; }
    if (t->b) LV2_ATOM_SEQUENCE_FOREACH(t->b, ev) { (void)ev; lv2_midi_echo_two.b++; }
    if (t->in && t->out) memcpy(t->out, t->in, n * sizeof(float));
}
static void two_out_connect(LV2_Handle h, uint32_t port, void *data)
{
    struct two *t = h;
    switch (port) { case 0: t->in = data; break; case 1: t->out = data; break; case 2: t->midi_in = data; break; case 3: t->out_a = data; break; case 4: t->out_b = data; break; default: break; }
}
static void two_out_run(LV2_Handle h, uint32_t n)
{
    struct two *t = h;
    const uint32_t cap_a = t->out_a ? t->out_a->atom.size : 0, cap_b = t->out_b ? t->out_b->atom.size : 0;
    lv2_midi_echo_two.runs++;
    lv2_midi_echo_two.in = 0;
    if (t->out_a) lv2_atom_sequence_clear(t->out_a);
    if (t->out_b) lv2_atom_sequence_clear(t->out_b);
    if (t->midi_in) LV2_ATOM_SEQUENCE_FOREACH(t->midi_in, ev)
    {
        lv2_midi_echo_two.in++;
        if (t->out_a) lv2_atom_sequence_append_event(t->out_a, cap_a, ev);
        if (t->out_b) lv2_atom_sequence_append_event(t->out_b, cap_b, ev);
    }
    if (t->in && t->out) memcpy(t->out, t->in, n * sizeof(float));
}
/* the MIDI tool twin (#tool): a MIDI input (0) and output (1), no audio port; every note message goes out an octave
 * up, everything else as it came: a transform the host's e2e can read on its real ports */
struct tool { const LV2_Atom_Sequence *in; LV2_Atom_Sequence *out; LV2_URID midi; };
static LV2_Handle tool_instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *features)
{
    struct tool *t = calloc(1, sizeof(*t));
    const LV2_URID_Map *map = NULL;
    int i;
    (void)d; (void)rate; (void)path;
    for (i = 0; t && features && features[i]; i++)
        if (!strcmp(features[i]->URI, LV2_URID__map))
            map = features[i]->data;
    if (t && map)
        t->midi = map->map(map->handle, LV2_MIDI__MidiEvent);
    return t;
}
static void tool_connect(LV2_Handle h, uint32_t port, void *data)
{
    struct tool *t = h;
    if (port == 0) t->in = data; else if (port == 1) t->out = data;
}
static void tool_run(LV2_Handle h, uint32_t n)
{
    struct tool *t = h;
    const uint32_t cap = t->out ? t->out->atom.size : 0;
    (void)n;
    if (!t->out) return;
    lv2_atom_sequence_clear(t->out);
    if (t->in) LV2_ATOM_SEQUENCE_FOREACH(t->in, ev)
    {
        struct { LV2_Atom_Event ev; uint8_t data[8]; } raw;
        const uint8_t *m = LV2_ATOM_BODY_CONST(&ev->body);
        if (ev->body.type != t->midi || ev->body.size > sizeof(raw.data))
        {
            lv2_atom_sequence_append_event(t->out, cap, ev);
            continue;
        }
        memset(&raw, 0, sizeof(raw));
        raw.ev = *ev;
        memcpy(raw.data, m, ev->body.size);
        if (ev->body.size == 3 && ((m[0] & 0xf0) == 0x90 || (m[0] & 0xf0) == 0x80) && m[1] + 12 < 128)
            raw.data[1] = (uint8_t)(m[1] + 12);
        lv2_atom_sequence_append_event(t->out, cap, &raw.ev);
    }
}
static const LV2_Descriptor TOOL = { ECHO_URI "#tool", tool_instantiate, tool_connect, NULL, tool_run, NULL, echo_cleanup, NULL };

/* the MIDI source twin (#source): a MIDI output (0) and no input of any kind; a note on (key 60, velocity 100) at frame
 * 0 of every run, so a host reads a note it never sent */
static void source_connect(LV2_Handle h, uint32_t port, void *data)
{
    struct tool *t = h;
    if (port == 0) t->out = data;
}
static void source_run(LV2_Handle h, uint32_t n)
{
    struct tool *t = h;
    struct { LV2_Atom_Event ev; uint8_t data[8]; } raw;
    const uint32_t cap = t->out ? t->out->atom.size : 0;
    (void)n;
    if (!t->out) return;
    lv2_atom_sequence_clear(t->out);
    memset(&raw, 0, sizeof(raw));
    raw.ev.time.frames = 0;
    raw.ev.body.type = t->midi;
    raw.ev.body.size = 3;
    raw.data[0] = 0x90; raw.data[1] = 60; raw.data[2] = 100;
    lv2_atom_sequence_append_event(t->out, cap, &raw.ev);
}
static const LV2_Descriptor SOURCE = { ECHO_URI "#source", tool_instantiate, source_connect, NULL, source_run, NULL, echo_cleanup, NULL };

static const LV2_Descriptor TWO_IN = { ECHO_URI "#two-in", two_instantiate, two_in_connect, NULL, two_in_run, NULL, echo_cleanup, NULL };
static const LV2_Descriptor TWO_OUT = { ECHO_URI "#two-out", two_instantiate, two_out_connect, NULL, two_out_run, NULL, echo_cleanup, NULL };

EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    return index == 0 ? &DESCRIPTOR : index == 1 ? &SYNTH : index == 2 ? &TWO_IN : index == 3 ? &TWO_OUT : index == 4 ? &TOOL : index == 5 ? &SOURCE : NULL;
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
