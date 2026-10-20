/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/*
 * The feature-probe test plugin (lv2_feature_probe.c): a mono pass-through that requires nothing and records, at each
 * instantiate, every feature the host handed it and what options:options said, and writes one log:log line when it has
 * a log. Its twin requires worker:schedule and still instantiates without it, so a test sees a degraded load. What it
 * saw is written by the thread the host called it on, and read by the test through dlsym.
 */
#ifndef OMX_TEST_LV2_FEATURE_PROBE_H
#define OMX_TEST_LV2_FEATURE_PROBE_H

#include <stdint.h>

#define LV2_FEATURE_PROBE_URI "urn:openmixer:test:feature-probe"
#define LV2_FEATURE_PROBE_WORKER_URI "urn:openmixer:test:feature-probe#needs-worker"

/* the line the probe logs at instantiate, with the rate as an integer */
#define LV2_FEATURE_PROBE_LINE "feature-probe: instantiated at %d Hz\n"

/* the features the probe saw, one bit each */
enum
{
    LV2_FEATURE_PROBE_MAP = 1u << 0,
    LV2_FEATURE_PROBE_UNMAP = 1u << 1,
    LV2_FEATURE_PROBE_OPTIONS = 1u << 2,
    LV2_FEATURE_PROBE_BOUNDED = 1u << 3,
    LV2_FEATURE_PROBE_WORKER = 1u << 4,
    LV2_FEATURE_PROBE_DEFAULT_STATE = 1u << 5,
    LV2_FEATURE_PROBE_LOG = 1u << 6,
    LV2_FEATURE_PROBE_FIXED = 1u << 7,
    LV2_FEATURE_PROBE_POW2 = 1u << 8,
    LV2_FEATURE_PROBE_UNKNOWN = 1u << 9,     /* a URI the probe does not know */
    LV2_FEATURE_PROBE_MAP_PATH = 1u << 10,   /* state:mapPath */
    LV2_FEATURE_PROBE_FREE_PATH = 1u << 11,  /* state:freePath */
    LV2_FEATURE_PROBE_MAKE_PATH = 1u << 12,  /* state:makePath */
    LV2_FEATURE_PROBE_URI_MAP = 1u << 13,    /* uri-map */
    LV2_FEATURE_PROBE_EVENT = 1u << 14,      /* event */
};

struct lv2_feature_probe
{
    unsigned instantiate_calls;
    char uri[96];                   /* the descriptor instantiated last */
    uint32_t seen;                  /* LV2_FEATURE_PROBE_* */
    uint32_t n_features;            /* the array's length */
    char order[12][64];             /* the first URIs, in the array's order, cut at 63 bytes */
    int has_rate, has_min, has_max, has_nominal;
    float rate;                     /* param:sampleRate */
    int32_t min_block, max_block, nominal_block;
    int unmap_round_trip;           /* urid:unmap gave back what urid:map was handed */
    int log_answer;                 /* what log:log's printf answered, -1 without a log */
    uint32_t map_midi;              /* urid:map's id of midi:MidiEvent, 0 without urid:map */
    uint32_t uri_map_event;         /* uri-map's id of midi:MidiEvent under the event map, 0 without uri-map */
    uint32_t uri_map_other;         /* uri-map's id of midi:MidiEvent under another map, 0 without uri-map */
    int event_answers;              /* event's lv2_event_ref and lv2_event_unref each answered 0: 2, -1 without event */
};
extern struct lv2_feature_probe lv2_feature_probe;

#endif
