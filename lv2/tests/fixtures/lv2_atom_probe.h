/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */

/*
 * What lv2_atom_fixture.c records for a test to read: the process's one probe, exported by the fixture's binary as
 * `lv2_atom_probe`. A test fixture, never a shipped plugin.
 */

#ifndef OMX_TEST_LV2_ATOM_PROBE_H
#define OMX_TEST_LV2_ATOM_PROBE_H

#include <stdint.h>

#define LV2_ATOM_FIXTURE_URI "urn:openmixer:test:atom-fixture"
#define LV2_ATOM_PROBE_MIDI 16u

struct lv2_atom_probe
{
    /* the text plugin */
    uint32_t text_blocks;           /* run() calls */
    uint32_t notify_capacity;       /* the capacity the notify output offered at the last run */
    uint32_t notify_written;        /* objects written to the notify output, every block's */
    char name[64];                  /* the last name the plugin received */
    char file[256];                 /* the last file path the plugin received */
    float gain;                     /* the last gain the plugin received */
    /* the MIDI-and-time plugin, the last block's input */
    uint32_t events;                /* events in the sequence */
    int time_first;                 /* 1: the first event was a time:Position object */
    int64_t time_event_frame;       /* that event's frame */
    uint32_t objects;               /* time:Position objects, every block's */
    float speed, bpm;               /* the last object's time:speed and time:beatsPerMinute */
    uint32_t n_midi;                /* MIDI messages in the sequence, in order */
    int64_t midi_frame[LV2_ATOM_PROBE_MIDI];
    uint32_t midi_size[LV2_ATOM_PROBE_MIDI];
    uint8_t midi[LV2_ATOM_PROBE_MIDI][4];
};

#endif
