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
 * lv2_atom_fixture.c: the atom plugins of omx-atom-fixture.lv2, test fixtures and never shipped plugins.
 *
 *   urn:openmixer:test:atom-fixture    a gain driven by patch:Set, with two text properties. Each message it gets is
 *                                      echoed in another form: the gain as a patch:Set forged as an atom:Blank, the
 *                                      name as a patch:Put whose patch:body carries "got:<name>" (and, for the name
 *                                      "big", as a patch:Set of a 5000-byte string), the file as a patch:Set of the
 *                                      same atom:Path. Every block it also writes an object that is not a patch:Set or
 *                                      patch:Put, naming the gain, to its patch output, and one object to its notify
 *                                      output. out = in x gain.
 *   ...#midi-time                      one input that takes both midi:MidiEvent and time:Position: each block's events
 *                                      recorded in order. out = in.
 *
 * What either saw is in lv2_atom_probe (lv2_atom_probe.h). Nothing is allocated or locked in run().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/patch/patch.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>

#include "lv2_atom_probe.h"

#define FX_PREFIX LV2_ATOM_FIXTURE_URI "#"
#define BIG_BYTES 5000u

LV2_SYMBOL_EXPORT struct lv2_atom_probe lv2_atom_probe;

struct fx
{
    const float *in;
    float *out;
    const LV2_Atom_Sequence *patch_in, *events;
    LV2_Atom_Sequence *patch_out, *notify;
    LV2_Atom_Forge forge;
    LV2_URID object, blank, set, put, property, value, body, urid, float_t, string_t, path_t, midi_t, position;
    LV2_URID gain, name, file, other, peak, speed, bpm;
    float gain_value;
    char big[BIG_BYTES];
};

static LV2_Handle instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *features)
{
    LV2_URID_Map *map = NULL;
    struct fx *f;
    uint32_t i;

    (void)d;
    (void)rate;
    (void)path;
    for (i = 0; features && features[i]; i++)
        if (strcmp(features[i]->URI, LV2_URID__map) == 0)
            map = features[i]->data;
    if (!map || !(f = calloc(1, sizeof(*f))))
        return NULL;
    lv2_atom_forge_init(&f->forge, map);
    f->object = map->map(map->handle, LV2_ATOM__Object);
    f->blank = map->map(map->handle, LV2_ATOM__Blank);
    f->set = map->map(map->handle, LV2_PATCH__Set);
    f->put = map->map(map->handle, LV2_PATCH__Put);
    f->property = map->map(map->handle, LV2_PATCH__property);
    f->value = map->map(map->handle, LV2_PATCH__value);
    f->body = map->map(map->handle, LV2_PATCH__body);
    f->urid = map->map(map->handle, LV2_ATOM__URID);
    f->float_t = map->map(map->handle, LV2_ATOM__Float);
    f->string_t = map->map(map->handle, LV2_ATOM__String);
    f->path_t = map->map(map->handle, LV2_ATOM__Path);
    f->midi_t = map->map(map->handle, LV2_MIDI__MidiEvent);
    f->position = map->map(map->handle, LV2_TIME__Position);
    f->speed = map->map(map->handle, LV2_TIME__speed);
    f->bpm = map->map(map->handle, LV2_TIME__beatsPerMinute);
    f->gain = map->map(map->handle, FX_PREFIX "gain");
    f->name = map->map(map->handle, FX_PREFIX "name");
    f->file = map->map(map->handle, FX_PREFIX "file");
    f->other = map->map(map->handle, FX_PREFIX "Other");
    f->peak = map->map(map->handle, FX_PREFIX "Peak");
    f->gain_value = 1.0f;
    memset(f->big, 'x', BIG_BYTES - 1u);
    return f;
}

static void connect_port(LV2_Handle h, uint32_t port, void *data)
{
    struct fx *f = h;

    switch (port)
    {
    case 0: f->in = data; break;
    case 1: f->out = data; break;
    case 2: f->patch_in = data; f->events = data; break;
    case 3: f->patch_out = data; break;
    case 4: f->notify = data; break;
    default: break;
    }
}

static void copy_text(char *dst, size_t cap, const LV2_Atom *v)
{
    size_t n = v->size < cap ? v->size : cap;

    memcpy(dst, LV2_ATOM_BODY_CONST(v), n);
    dst[cap - 1] = '\0';
}

/* one patch:Set of `key` opened on the output, the value to follow; forged as an atom:Blank when `blank` */
static void set_begin(struct fx *f, LV2_Atom_Forge_Frame *obj, LV2_URID key, int blank)
{
    const LV2_URID object = f->forge.Object;

    lv2_atom_forge_frame_time(&f->forge, 0);
    if (blank)
        f->forge.Object = f->blank;
    lv2_atom_forge_object(&f->forge, obj, 0, f->set);
    f->forge.Object = object;
    lv2_atom_forge_key(&f->forge, f->property);
    lv2_atom_forge_urid(&f->forge, key);
    lv2_atom_forge_key(&f->forge, f->value);
}

static void text_run(LV2_Handle h, uint32_t n)
{
    struct fx *f = h;
    LV2_Atom_Forge_Frame seq, obj, body;
    char got[80];
    uint32_t i;
    float peak = 0.0f;

    lv2_atom_probe.text_blocks++;
    lv2_atom_forge_set_buffer(&f->forge, (uint8_t *)f->patch_out, f->patch_out->atom.size + sizeof(LV2_Atom));
    lv2_atom_forge_sequence_head(&f->forge, &seq, 0);
    LV2_ATOM_SEQUENCE_FOREACH(f->patch_in, ev)
    {
        const LV2_Atom_Object *o = (const LV2_Atom_Object *)&ev->body;
        const LV2_Atom *prop = NULL, *val = NULL;
        LV2_URID key;

        if ((ev->body.type != f->object && ev->body.type != f->blank) || o->body.otype != f->set)
            continue;
        lv2_atom_object_get(o, f->property, &prop, f->value, &val, 0);
        if (!prop || !val || prop->type != f->urid)
            continue;
        key = ((const LV2_Atom_URID *)prop)->body;
        if (key == f->gain && val->type == f->float_t)
        {
            f->gain_value = ((const LV2_Atom_Float *)val)->body;
            lv2_atom_probe.gain = f->gain_value;
            set_begin(f, &obj, key, 1);
            lv2_atom_forge_float(&f->forge, f->gain_value);
            lv2_atom_forge_pop(&f->forge, &obj);
        }
        else if (key == f->name && val->type == f->string_t)
        {
            copy_text(lv2_atom_probe.name, sizeof(lv2_atom_probe.name), val);
            if (strcmp(lv2_atom_probe.name, "big") == 0)
            {
                set_begin(f, &obj, key, 0);
                lv2_atom_forge_string(&f->forge, f->big, BIG_BYTES - 1u);
                lv2_atom_forge_pop(&f->forge, &obj);
                continue;
            }
            snprintf(got, sizeof(got), "got:%s", lv2_atom_probe.name);
            lv2_atom_forge_frame_time(&f->forge, 0);
            lv2_atom_forge_object(&f->forge, &obj, 0, f->put);
            lv2_atom_forge_key(&f->forge, f->body);
            lv2_atom_forge_object(&f->forge, &body, 0, 0);
            lv2_atom_forge_key(&f->forge, f->name);
            lv2_atom_forge_string(&f->forge, got, (uint32_t)strlen(got));
            lv2_atom_forge_pop(&f->forge, &body);
            lv2_atom_forge_pop(&f->forge, &obj);
        }
        else if (key == f->file && val->type == f->path_t)
        {
            copy_text(lv2_atom_probe.file, sizeof(lv2_atom_probe.file), val);
            set_begin(f, &obj, key, 0);
            lv2_atom_forge_path(&f->forge, lv2_atom_probe.file, (uint32_t)strlen(lv2_atom_probe.file));
            lv2_atom_forge_pop(&f->forge, &obj);
        }
    }
    // an object that is neither a patch:Set nor a patch:Put, naming the gain: the host must not read it as one
    lv2_atom_forge_frame_time(&f->forge, 0);
    lv2_atom_forge_object(&f->forge, &obj, 0, f->other);
    lv2_atom_forge_key(&f->forge, f->property);
    lv2_atom_forge_urid(&f->forge, f->gain);
    lv2_atom_forge_key(&f->forge, f->value);
    lv2_atom_forge_float(&f->forge, 99.0f);
    lv2_atom_forge_pop(&f->forge, &obj);
    lv2_atom_forge_pop(&f->forge, &seq);

    for (i = 0; i < n; i++)
    {
        f->out[i] = f->in[i] * f->gain_value;
        peak = f->out[i] > peak ? f->out[i] : -f->out[i] > peak ? -f->out[i] : peak;
    }
    // the notify output: one object of the plugin's own, as a GUI would read it
    lv2_atom_probe.notify_capacity = f->notify->atom.size;
    lv2_atom_forge_set_buffer(&f->forge, (uint8_t *)f->notify, f->notify->atom.size + sizeof(LV2_Atom));
    lv2_atom_forge_sequence_head(&f->forge, &seq, 0);
    lv2_atom_forge_frame_time(&f->forge, 0);
    lv2_atom_forge_object(&f->forge, &obj, 0, f->peak);
    lv2_atom_forge_key(&f->forge, f->value);
    lv2_atom_forge_float(&f->forge, peak);
    lv2_atom_forge_pop(&f->forge, &obj);
    lv2_atom_forge_pop(&f->forge, &seq);
    lv2_atom_probe.notify_written++;
}

static void midi_time_run(LV2_Handle h, uint32_t n)
{
    struct fx *f = h;
    uint32_t i = 0;

    lv2_atom_probe.events = 0;
    lv2_atom_probe.time_first = 0;
    lv2_atom_probe.n_midi = 0;
    LV2_ATOM_SEQUENCE_FOREACH(f->events, ev)
    {
        if (ev->body.type == f->object && ((const LV2_Atom_Object *)&ev->body)->body.otype == f->position)
        {
            const LV2_Atom *speed = NULL, *bpm = NULL;

            lv2_atom_object_get((const LV2_Atom_Object *)&ev->body, f->speed, &speed, f->bpm, &bpm, 0);
            lv2_atom_probe.objects++;
            if (i == 0)
            {
                lv2_atom_probe.time_first = 1;
                lv2_atom_probe.time_event_frame = ev->time.frames;
            }
            lv2_atom_probe.speed = speed && speed->type == f->float_t ? ((const LV2_Atom_Float *)speed)->body : -1.0f;
            lv2_atom_probe.bpm = bpm && bpm->type == f->float_t ? ((const LV2_Atom_Float *)bpm)->body : -1.0f;
        }
        else if (ev->body.type == f->midi_t && lv2_atom_probe.n_midi < LV2_ATOM_PROBE_MIDI)
        {
            const uint32_t k = lv2_atom_probe.n_midi++;

            lv2_atom_probe.midi_frame[k] = ev->time.frames;
            lv2_atom_probe.midi_size[k] = ev->body.size;
            memcpy(lv2_atom_probe.midi[k], LV2_ATOM_BODY_CONST(&ev->body), ev->body.size < 4u ? ev->body.size : 4u);
        }
        i++;
    }
    lv2_atom_probe.events = i;
    memcpy(f->out, f->in, n * sizeof(float));
}

static void cleanup(LV2_Handle h)
{
    free(h);
}

static const LV2_Descriptor g_descriptors[2] = {
    { LV2_ATOM_FIXTURE_URI, instantiate, connect_port, NULL, text_run, NULL, cleanup, NULL },
    { FX_PREFIX "midi-time", instantiate, connect_port, NULL, midi_time_run, NULL, cleanup, NULL },
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    return index < 2u ? &g_descriptors[index] : NULL;
}
