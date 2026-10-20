/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/*
 * The patch fixture's witnesses, read by the tests through the fixture's own symbols. A test fixture, never a shipped
 * plugin.
 */
#ifndef LV2_PATCH_FIXTURE_H
#define LV2_PATCH_FIXTURE_H

#include <stdint.h>

/* the #midi twin's: one atom input that takes midi:MidiEvent and patch:Message. Per run, what the sequence held. */
struct lv2_patch_fixture_midi
{
    unsigned runs;
    unsigned events;        /* every event of the last run */
    unsigned midi;          /* midi:MidiEvent events of the last run */
    unsigned patch;         /* patch:Set / patch:Put objects of the last run */
    unsigned out_of_order;  /* events whose frame was below the one before it, over every run */
    int64_t first_midi_frame, first_patch_frame;    /* -1: none in the last run */
};
extern struct lv2_patch_fixture_midi lv2_patch_fixture_midi;

#endif
