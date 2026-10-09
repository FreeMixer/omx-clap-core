// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The time-probe test plugin: a mono pass-through whose atom input takes time:Position. Each run() reads the block's
// sequence and records what the time:Position objects in it say, so a test sees exactly what the adapter forged.

#include <stdlib.h>
#include <string.h>

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>

#include "lv2_time_probe.h"

struct lv2_time_probe lv2_time_probe;

enum { TP_IN = 0, TP_OUT = 1, TP_TIME = 2 };

struct tp
{
    const float *in;
    float *out;
    const LV2_Atom_Sequence *time;
    LV2_URID object, position, long_t, float_t, int_t;
    LV2_URID frame, speed, bar, bar_beat, beat_unit, beats_per_bar, bpm;
};

static LV2_Handle instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *features)
{
    const LV2_URID_Map *map = NULL;
    struct tp *t;

    (void)d, (void)rate, (void)path;
    for (int i = 0; features && features[i]; i++)
        if (!strcmp(features[i]->URI, LV2_URID__map))
            map = features[i]->data;
    if (!map || !(t = calloc(1, sizeof(*t))))
        return NULL;
    t->object = map->map(map->handle, LV2_ATOM__Object);
    t->position = map->map(map->handle, LV2_TIME__Position);
    t->long_t = map->map(map->handle, LV2_ATOM__Long);
    t->float_t = map->map(map->handle, LV2_ATOM__Float);
    t->int_t = map->map(map->handle, LV2_ATOM__Int);
    t->frame = map->map(map->handle, LV2_TIME__frame);
    t->speed = map->map(map->handle, LV2_TIME__speed);
    t->bar = map->map(map->handle, LV2_TIME__bar);
    t->bar_beat = map->map(map->handle, LV2_TIME__barBeat);
    t->beat_unit = map->map(map->handle, LV2_TIME__beatUnit);
    t->beats_per_bar = map->map(map->handle, LV2_TIME__beatsPerBar);
    t->bpm = map->map(map->handle, LV2_TIME__beatsPerMinute);
    return t;
}

static void connect_port(LV2_Handle h, uint32_t port, void *data)
{
    struct tp *t = h;

    if (port == TP_IN)
        t->in = data;
    else if (port == TP_OUT)
        t->out = data;
    else if (port == TP_TIME)
        t->time = data;
}

static void object_note(const struct tp *t, const LV2_Atom_Object *o)
{
    LV2_ATOM_OBJECT_FOREACH(o, p)
    {
        const LV2_Atom *v = &p->value;

        if (p->key == t->frame && v->type == t->long_t)
        {
            lv2_time_probe.frame = ((const LV2_Atom_Long *)v)->body;
            lv2_time_probe.keys |= LV2_TIME_PROBE_FRAME;
        }
        else if (p->key == t->speed && v->type == t->float_t)
        {
            lv2_time_probe.speed = ((const LV2_Atom_Float *)v)->body;
            lv2_time_probe.keys |= LV2_TIME_PROBE_SPEED;
        }
        else if (p->key == t->bar && v->type == t->long_t)
        {
            lv2_time_probe.bar = ((const LV2_Atom_Long *)v)->body;
            lv2_time_probe.keys |= LV2_TIME_PROBE_BAR;
        }
        else if (p->key == t->bar_beat && v->type == t->float_t)
        {
            lv2_time_probe.bar_beat = ((const LV2_Atom_Float *)v)->body;
            lv2_time_probe.keys |= LV2_TIME_PROBE_BAR_BEAT;
        }
        else if (p->key == t->beat_unit && v->type == t->int_t)
        {
            lv2_time_probe.beat_unit = ((const LV2_Atom_Int *)v)->body;
            lv2_time_probe.keys |= LV2_TIME_PROBE_BEAT_UNIT;
        }
        else if (p->key == t->beats_per_bar && v->type == t->float_t)
        {
            lv2_time_probe.beats_per_bar = ((const LV2_Atom_Float *)v)->body;
            lv2_time_probe.keys |= LV2_TIME_PROBE_BEATS_PER_BAR;
        }
        else if (p->key == t->bpm && v->type == t->float_t)
        {
            lv2_time_probe.bpm = ((const LV2_Atom_Float *)v)->body;
            lv2_time_probe.keys |= LV2_TIME_PROBE_BPM;
        }
    }
}

static void run(LV2_Handle h, uint32_t n)
{
    const struct tp *t = h;

    lv2_time_probe.blocks++;
    lv2_time_probe.events = 0;
    lv2_time_probe.keys = 0;
    if (t->time)
    {
        LV2_ATOM_SEQUENCE_FOREACH(t->time, ev)
        {
            lv2_time_probe.events++;
            if (ev->body.type == t->object && ((const LV2_Atom_Object *)&ev->body)->body.otype == t->position)
            {
                lv2_time_probe.objects++;
                lv2_time_probe.keys = 0;
                object_note(t, (const LV2_Atom_Object *)&ev->body);
            }
        }
    }
    memcpy(t->out, t->in, n * sizeof(float));
}

static void cleanup(LV2_Handle h)
{
    free(h);
}

static const LV2_Descriptor descriptor = {
    "urn:openmixer:test:time-probe", instantiate, connect_port, NULL, run, NULL, cleanup, NULL,
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : NULL;
}
