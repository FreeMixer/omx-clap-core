/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/*
 * The time-probe test plugin (lv2_time_probe.c): a mono pass-through with a time:Position input. What it saw is written
 * by the thread the host called it on, and read by the test through dlsym.
 */
#ifndef OMX_TEST_LV2_TIME_PROBE_H
#define OMX_TEST_LV2_TIME_PROBE_H

#include <stdint.h>

#define LV2_TIME_PROBE_URI "urn:openmixer:test:time-probe"

/* the keys of the last time:Position object the block held, one bit each */
enum
{
    LV2_TIME_PROBE_FRAME = 1u << 0,
    LV2_TIME_PROBE_SPEED = 1u << 1,
    LV2_TIME_PROBE_BAR = 1u << 2,
    LV2_TIME_PROBE_BAR_BEAT = 1u << 3,
    LV2_TIME_PROBE_BEAT_UNIT = 1u << 4,
    LV2_TIME_PROBE_BEATS_PER_BAR = 1u << 5,
    LV2_TIME_PROBE_BPM = 1u << 6
};

struct lv2_time_probe
{
    uint32_t blocks;        /* run() calls, every block */
    uint32_t events;        /* events the last block's sequence held */
    uint32_t objects;       /* time:Position objects, every block's */
    uint32_t keys;          /* the last object's keys, LV2_TIME_PROBE_* */
    int64_t frame;          /* time:frame */
    float speed;            /* time:speed */
    int64_t bar;            /* time:bar */
    float bar_beat;         /* time:barBeat */
    int32_t beat_unit;      /* time:beatUnit */
    float beats_per_bar;    /* time:beatsPerBar */
    float bpm;              /* time:beatsPerMinute */
};

extern struct lv2_time_probe lv2_time_probe;

#endif
