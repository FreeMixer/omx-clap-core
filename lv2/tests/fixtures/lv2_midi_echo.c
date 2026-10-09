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
 * The MIDI fixture: one plugin with an audio pass-through and one MIDI input and one MIDI output. Every event the input
 * sequence carries is written to the output sequence at the same frame, byte for byte, in the same order; the audio is
 * copied from input to output. So what a test puts into the MIDI input is what comes back out of the MIDI output, and a
 * second block with no input writes nothing.
 */

#include <stdlib.h>
#include <string.h>

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>

#define EXPORT __attribute__((visibility("default")))

#define ECHO_URI "urn:openmixer:test:midi-echo"

enum { P_IN, P_OUT, P_MIDI_IN, P_MIDI_OUT };

struct echo
{
    const float *in;
    float *out;
    const LV2_Atom_Sequence *midi_in;
    LV2_Atom_Sequence *midi_out;
};

static LV2_Handle echo_instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *features)
{
    (void)d;
    (void)rate;
    (void)path;
    (void)features;
    return calloc(1, sizeof(struct echo));
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

static void echo_run(LV2_Handle h, uint32_t n)
{
    struct echo *e = h;
    const uint32_t cap = e->midi_out->atom.size;

    memcpy(e->out, e->in, n * sizeof(float));
    lv2_atom_sequence_clear(e->midi_out);
    LV2_ATOM_SEQUENCE_FOREACH(e->midi_in, ev)
        lv2_atom_sequence_append_event(e->midi_out, cap, ev);
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
