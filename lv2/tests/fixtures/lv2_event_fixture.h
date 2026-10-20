/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/*
 * The event-port test plugins (lv2_event_fixture.c): MIDI through ev:EventPort, the event extension's LV2_Event_Buffer.
 * #echo writes every MIDI event of its input to its output at the same frame, a note on or off one key up, and passes
 * its stereo audio through. #two counts the events on each of its two inputs and writes the first input's to both its
 * outputs. What they saw is written by the thread the host called them on, and read by the test through dlsym.
 */
#ifndef OMX_TEST_LV2_EVENT_FIXTURE_H
#define OMX_TEST_LV2_EVENT_FIXTURE_H

#include <stdint.h>

#define LV2_EVENT_FIXTURE_URI "urn:openmixer:test:event-fixture"
#define LV2_EVENT_FIXTURE_SEEN 80u      /* the input events kept, the first of the last run */

/* what #echo does to its output after writing it, when the test sets `misbehave` */
enum
{
    LV2_EVENT_FIXTURE_WELL = 0,         /* nothing: the output as written */
    LV2_EVENT_FIXTURE_MOVED = 1,        /* its data pointer moved to a buffer of the plugin's own, holding the same events */
    LV2_EVENT_FIXTURE_OVERSIZE = 2,     /* the first event's size 65520, the buffer's size its whole capacity */
    LV2_EVENT_FIXTURE_STAMP = 3,        /* its stamp type 1: stamps that are not frames */
    LV2_EVENT_FIXTURE_COUNT = 4,        /* its event count one short of the events it holds */
};

struct lv2_event_fixture
{
    uint32_t misbehave;             /* LV2_EVENT_FIXTURE_*, set by the test */
    uint32_t instantiated;          /* instantiate() calls */
    uint32_t runs;                  /* run() calls, every descriptor's */
    uint32_t midi_type;             /* midi:MidiEvent's id as uri-map answered it at instantiate, 0 without uri-map */
    uint32_t n;                     /* the MIDI events of the last run's input (#echo), or of input A (#two) */
    uint32_t b;                     /* #two: the events of input B in the last run, of any type */
    uint32_t other;                 /* #echo: the events of the last run's input whose type is not the MIDI id */
    uint32_t frame[LV2_EVENT_FIXTURE_SEEN];
    uint32_t size[LV2_EVENT_FIXTURE_SEEN];
    uint8_t data[LV2_EVENT_FIXTURE_SEEN][4];
};
extern struct lv2_event_fixture lv2_event_fixture;

#endif
