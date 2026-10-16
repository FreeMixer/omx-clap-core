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

/** @file
 * @brief The MIDI form of CLAP's note events: one conversion for the host's note output and the LV2 adapter. */

/*
 * omx_clap_midi.h: how long a MIDI message is from its status byte, and the MIDI message a CLAP note on, note off or
 * choke is. The host writes a plugin's note output as MIDI with them (clap_stage.h), and the LV2 adapter feeds an LV2
 * MIDI input with them, so a note means the same bytes on both paths.
 *
 * Header only and inline, safe on the audio thread: no allocation, no lock, no call out.
 */
#ifndef OMX_CLAP_MIDI_H
#define OMX_CLAP_MIDI_H

#include <math.h>
#include <stdint.h>

#include <clap/events.h>

/** The bytes a MIDI message with status byte `status` has: 3 for a channel message, 2 for a program change and a channel
 * pressure, 1 to 3 for a system common or real-time message by its kind; 0 for a byte no message starts with and for a
 * sysex (0xf0), which CLAP carries as a CLAP_EVENT_MIDI_SYSEX of its own. */
static inline uint32_t omx_clap_midi_size(uint8_t status)
{
    if (status < 0x80)
        return 0;
    if (status < 0xf0)
        return (status & 0xe0) == 0xc0 ? 2u : 3u;
    switch (status)
    {
    case 0xf1: case 0xf3: return 2;
    case 0xf2: return 3;
    case 0xf6: case 0xf8: case 0xf9: case 0xfa: case 0xfb: case 0xfc: case 0xfd: case 0xfe: case 0xff: return 1;
    default: return 0;
    }
}

/** A CLAP_EVENT_NOTE_ON, _NOTE_OFF or _NOTE_CHOKE as its MIDI message in `m`: 3, or 0 for an event of another type, one
 * that names no single key (0 to 127) on one channel (0 to 15), or one of a note port other than 0 (-1, every port, is
 * port 0's). The velocity is scaled to 0..127 and rounded; a note on is never sent at 0, which MIDI reads as a note off,
 * and a choke is a note off at 0. */
static inline uint32_t omx_clap_note_midi(const clap_event_header_t *h, uint8_t m[3])
{
    const clap_event_note_t *e = (const clap_event_note_t *)h;
    double v;
    long vel;

    if (h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->size < sizeof(clap_event_note_t)
        || (h->type != CLAP_EVENT_NOTE_ON && h->type != CLAP_EVENT_NOTE_OFF && h->type != CLAP_EVENT_NOTE_CHOKE))
        return 0;
    if (e->key < 0 || e->key > 127 || e->channel < 0 || e->channel > 15 || (e->port_index != 0 && e->port_index != -1))
        return 0;
    v = e->velocity < 0.0 ? 0.0 : e->velocity > 1.0 ? 1.0 : e->velocity;
    vel = lrint(v * 127.0);
    if (h->type == CLAP_EVENT_NOTE_ON && vel < 1)
        vel = 1;
    if (h->type == CLAP_EVENT_NOTE_CHOKE)
        vel = 0;
    m[0] = (uint8_t)((h->type == CLAP_EVENT_NOTE_ON ? 0x90 : 0x80) | e->channel);
    m[1] = (uint8_t)e->key;
    m[2] = (uint8_t)vel;
    return 3;
}

#endif /* OMX_CLAP_MIDI_H */
