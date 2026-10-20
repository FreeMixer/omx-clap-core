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
 * The ll-plugins MIDI port fixture: MIDI through llext:MidiPort, the port class zynadd declares its MIDI input as, over
 * an LV2_MIDI buffer (lv2_ll_midi.h), read the way zynadd reads it. #echo: a stereo pair in and out, one llext:MidiPort
 * input and one output; what comes in goes out at its frame, a note on or off one key up. The input is read the way
 * zynadd's run reads it (lv2-midifunctions.h: a position walked by each event's size until it reaches `size`, the
 * event count unread, and the stamp after the last event read before any check). #two-in and #two-out: a mono pair
 * and two llext:MidiPorts one way, one more than the adapter feeds, refused; #atom-then-ll, #ll-then-atom,
 * #ev-then-ll, #ll-then-ev: an llext:MidiPort input beside an atom or ev:EventPort MIDI input in either index order,
 * refused the same. What the last run read is recorded here for the test.
 */
#ifndef OMX_TEST_LV2_LL_MIDI_FIXTURE_H
#define OMX_TEST_LV2_LL_MIDI_FIXTURE_H

#include <stdint.h>

#define LV2_LL_MIDI_FIXTURE_URI "urn:openmixer:test:ll-midi-fixture"
#define LV2_LL_MIDI_FIXTURE_SEEN 80u    /* the input events kept, the first of the last run */

enum
{
    LV2_LL_MIDI_FIXTURE_WELL = 0,       /* nothing: the output as written */
    LV2_LL_MIDI_FIXTURE_MOVED = 1,      /* its data pointer moved to a buffer of the plugin's own, holding the same events */
    LV2_LL_MIDI_FIXTURE_OVERSIZE = 2,   /* the first event's size 65520, the buffer's size its whole capacity */
    LV2_LL_MIDI_FIXTURE_STAMP = 3,      /* the first event's timestamp -1: not a frame */
    LV2_LL_MIDI_FIXTURE_COUNT = 4,      /* its event count one short of the events it holds */
    LV2_LL_MIDI_FIXTURE_STAMP_NAN = 5,  /* the first event's timestamp NaN */
    LV2_LL_MIDI_FIXTURE_STAMP_FAR = 6,  /* the first event's timestamp 2^24: past any frame */
    LV2_LL_MIDI_FIXTURE_SIZE_PAST = 7,  /* filled to exactly its capacity (54 events), then its size 256 bytes past it and its count 1000 */
};

struct lv2_ll_midi_fixture
{
    uint32_t misbehave;             /* LV2_LL_MIDI_FIXTURE_*, set by the test */
    uint32_t instantiated;          /* instantiate() calls */
    uint32_t runs;                  /* run() calls */
    uint32_t n;                     /* the MIDI events of the last run's input */
    uint32_t count_seen;            /* the input header's event_count as handed, unused by the walk, as zynadd leaves it */
    uint32_t size_seen;             /* the input header's size as handed */
    double trailing_stamp;          /* the stamp read after the last event, as zynadd's lv2midi_step reads it */
    uint32_t frame[LV2_LL_MIDI_FIXTURE_SEEN];
    uint32_t size[LV2_LL_MIDI_FIXTURE_SEEN];
    uint8_t data[LV2_LL_MIDI_FIXTURE_SEEN][4];
};

extern struct lv2_ll_midi_fixture lv2_ll_midi_fixture;

#endif
