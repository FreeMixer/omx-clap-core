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
 * omx-patch-fixture.ttl's plugin: a gain that scales the audio, driven by patch:Set on its input atom port. Every
 * numeric property it receives is echoed on its output atom port, as patch:Set, except mode, which is echoed as
 * patch:Put, so a test reads back what the plugin got and in which form. A gain above 2 scales the audio as received,
 * but is echoed as 2: a value the plugin keeps on its own, which no host write carries. A test fixture, never a shipped
 * plugin.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "lv2_patch_fixture.h"

#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/midi/midi.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/patch/patch.h>
#include <lv2/state/state.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>

#define FX_URI "urn:openmixer:test:patch-fixture"
#define FX_PREFIX FX_URI "#"

enum { IN = 0, OUT = 1, PATCH_IN = 2, PATCH_OUT = 3 };

struct fx
{
    const float *in;
    float *out;
    const LV2_Atom_Sequence *patch_in;
    LV2_Atom_Sequence *patch_out;
    LV2_URID_Map *map;
    LV2_Atom_Forge forge;
    LV2_URID object, set, put, property, value, urid, float_t, int_t, bool_t;
    LV2_URID gain, mode, flag, level;
    float gain_value;
};

static LV2_Handle instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *features)
{
    struct fx *f;
    uint32_t i;

    (void)d;
    (void)rate;
    (void)path;
    f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    for (i = 0; features && features[i]; i++)
        if (strcmp(features[i]->URI, LV2_URID__map) == 0)
            f->map = features[i]->data;
    if (!f->map)
    {
        free(f);
        return NULL;
    }
    lv2_atom_forge_init(&f->forge, f->map);
    f->object = f->map->map(f->map->handle, LV2_ATOM__Object);
    f->set = f->map->map(f->map->handle, LV2_PATCH__Set);
    f->put = f->map->map(f->map->handle, LV2_PATCH__Put);
    f->property = f->map->map(f->map->handle, LV2_PATCH__property);
    f->value = f->map->map(f->map->handle, LV2_PATCH__value);
    f->urid = f->map->map(f->map->handle, LV2_ATOM__URID);
    f->float_t = f->map->map(f->map->handle, LV2_ATOM__Float);
    f->int_t = f->map->map(f->map->handle, LV2_ATOM__Int);
    f->bool_t = f->map->map(f->map->handle, LV2_ATOM__Bool);
    f->gain = f->map->map(f->map->handle, FX_PREFIX "gain");
    f->mode = f->map->map(f->map->handle, FX_PREFIX "mode");
    f->flag = f->map->map(f->map->handle, FX_PREFIX "flag");
    f->level = f->map->map(f->map->handle, FX_PREFIX "level");
    f->gain_value = 1.0f;
    return f;
}

static void connect_port(LV2_Handle h, uint32_t port, void *data)
{
    struct fx *f = h;

    switch (port)
    {
    case IN: f->in = data; break;
    case OUT: f->out = data; break;
    case PATCH_IN: f->patch_in = data; break;
    case PATCH_OUT: f->patch_out = data; break;
    default: break;
    }
}

/* one patch:Set (or Put) of `key` on the output: the frame, the object, its property and its value */
static void echo_begin(struct fx *f, LV2_Atom_Forge_Frame *obj, LV2_URID otype, LV2_URID key)
{
    lv2_atom_forge_frame_time(&f->forge, 0);
    lv2_atom_forge_object(&f->forge, obj, 0, otype);
    lv2_atom_forge_key(&f->forge, f->property);
    lv2_atom_forge_urid(&f->forge, key);
    lv2_atom_forge_key(&f->forge, f->value);
}

static void run(LV2_Handle h, uint32_t n)
{
    struct fx *f = h;
    LV2_Atom_Forge_Frame seq;
    uint32_t i;

    lv2_atom_forge_set_buffer(&f->forge, (uint8_t *)f->patch_out, f->patch_out->atom.size + sizeof(LV2_Atom));
    lv2_atom_forge_sequence_head(&f->forge, &seq, 0);
    LV2_ATOM_SEQUENCE_FOREACH(f->patch_in, ev)
    {
        const LV2_Atom_Object *obj = (const LV2_Atom_Object *)&ev->body;
        const LV2_Atom *prop = NULL, *val = NULL;
        LV2_Atom_Forge_Frame frame;
        LV2_URID key;

        if (ev->body.type != f->object || (obj->body.otype != f->set && obj->body.otype != f->put))
            continue;
        lv2_atom_object_get(obj, f->property, &prop, f->value, &val, 0);
        if (!prop || !val || prop->type != f->urid)
            continue;
        key = ((const LV2_Atom_URID *)prop)->body;
        if (key == f->gain && val->type == f->float_t)
        {
            f->gain_value = ((const LV2_Atom_Float *)val)->body;
            echo_begin(f, &frame, f->set, key);
            lv2_atom_forge_float(&f->forge, f->gain_value > 2.0f ? 2.0f : f->gain_value);
            lv2_atom_forge_pop(&f->forge, &frame);
        }
        else if (key == f->mode && val->type == f->int_t)
        {
            echo_begin(f, &frame, f->put, key);
            lv2_atom_forge_int(&f->forge, ((const LV2_Atom_Int *)val)->body);
            lv2_atom_forge_pop(&f->forge, &frame);
        }
        else if (key == f->flag && val->type == f->bool_t)
        {
            echo_begin(f, &frame, f->set, key);
            lv2_atom_forge_bool(&f->forge, ((const LV2_Atom_Bool *)val)->body);
            lv2_atom_forge_pop(&f->forge, &frame);
        }
        else if (key == f->level && val->type == f->float_t)
        {
            echo_begin(f, &frame, f->set, key);
            lv2_atom_forge_float(&f->forge, ((const LV2_Atom_Float *)val)->body);
            lv2_atom_forge_pop(&f->forge, &frame);
        }
    }
    lv2_atom_forge_pop(&f->forge, &seq);
    for (i = 0; i < n; i++)
        f->out[i] = f->in[i] * f->gain_value;
}

static void cleanup(LV2_Handle h)
{
    free(h);
}

static const LV2_Descriptor descriptor = {
    FX_URI, instantiate, connect_port, NULL, run, NULL, cleanup, NULL,
};

/* the #midi twin: audio in and out, and ONE atom input that takes midi:MidiEvent and patch:Message (the lsp
 * samplers' shape). Its witness counts what each run's sequence held and whether it was in frame order. */
struct lv2_patch_fixture_midi lv2_patch_fixture_midi;
struct mfx
{
    const float *in;
    float *out;
    const LV2_Atom_Sequence *events;
    LV2_URID midi, object, blank, set, put;
};
static LV2_Handle midi_instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *features)
{
    struct mfx *f = calloc(1, sizeof(*f));
    LV2_URID_Map *map = NULL;
    uint32_t i;

    (void)d; (void)rate; (void)path;
    if (!f)
        return NULL;
    for (i = 0; features && features[i]; i++)
        if (strcmp(features[i]->URI, LV2_URID__map) == 0)
            map = features[i]->data;
    if (!map)
    {
        free(f);
        return NULL;
    }
    f->midi = map->map(map->handle, LV2_MIDI__MidiEvent);
    f->object = map->map(map->handle, LV2_ATOM__Object);
    f->blank = map->map(map->handle, LV2_ATOM__Blank);
    f->set = map->map(map->handle, LV2_PATCH__Set);
    f->put = map->map(map->handle, LV2_PATCH__Put);
    return f;
}
static void midi_connect_port(LV2_Handle h, uint32_t port, void *data)
{
    struct mfx *f = h;

    if (port == 0) f->in = data; else if (port == 1) f->out = data; else if (port == 2) f->events = data;
}
static void midi_run(LV2_Handle h, uint32_t n)
{
    struct mfx *f = h;
    struct lv2_patch_fixture_midi *w = &lv2_patch_fixture_midi;
    int64_t last = -1;

    w->runs++;
    w->events = w->midi = w->patch = 0;
    w->first_midi_frame = w->first_patch_frame = -1;
    if (f->events)
        LV2_ATOM_SEQUENCE_FOREACH(f->events, ev)
        {
            w->events++;
            if (ev->time.frames < last)
                w->out_of_order++;
            last = ev->time.frames;
            if (ev->body.type == f->midi)
            {
                w->midi++;
                if (w->first_midi_frame < 0) w->first_midi_frame = ev->time.frames;
            }
            else if (ev->body.type == f->object || ev->body.type == f->blank)
            {
                const LV2_Atom_Object *obj = (const LV2_Atom_Object *)&ev->body;

                if (obj->body.otype == f->set || obj->body.otype == f->put)
                {
                    w->patch++;
                    if (w->first_patch_frame < 0) w->first_patch_frame = ev->time.frames;
                }
            }
        }
    if (f->in && f->out)
        memmove(f->out, f->in, n * sizeof(float));
}
static void midi_cleanup(LV2_Handle h) { free(h); }
static const LV2_Descriptor midi_descriptor = {
    FX_URI "#midi", midi_instantiate, midi_connect_port, NULL, midi_run, NULL, midi_cleanup, NULL,
};

/* the #synth twin: an instrument whose real values live in the plugin. It starts at volume 0.5 and drive 1, whatever its
 * TTL says, and the TTL says lv2:default 0 for both, as a plugin that generates its TTL from normalised parameters can:
 * the state of a new instance is the plugin's, and a host that sends the TTL's default mutes it. A held note is a
 * full-scale 440 Hz sine times volume times drive; audio in is ignored, audio out is the note alone. Its state holds the
 * volume. */
struct sfx
{
    float *out;
    const LV2_Atom_Sequence *events;
    LV2_Atom_Sequence *notify;
    LV2_URID midi, object, blank, set, put, property, value, urid, float_t, volume, drive;
    float volume_value, drive_value;
    double rate, phase;
    int held;
};
static LV2_Handle synth_instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *features)
{
    struct sfx *f = calloc(1, sizeof(*f));
    LV2_URID_Map *map = NULL;
    uint32_t i;

    (void)d; (void)path;
    if (!f)
        return NULL;
    for (i = 0; features && features[i]; i++)
        if (strcmp(features[i]->URI, LV2_URID__map) == 0)
            map = features[i]->data;
    if (!map)
    {
        free(f);
        return NULL;
    }
    f->midi = map->map(map->handle, LV2_MIDI__MidiEvent);
    f->object = map->map(map->handle, LV2_ATOM__Object);
    f->blank = map->map(map->handle, LV2_ATOM__Blank);
    f->set = map->map(map->handle, LV2_PATCH__Set);
    f->put = map->map(map->handle, LV2_PATCH__Put);
    f->property = map->map(map->handle, LV2_PATCH__property);
    f->value = map->map(map->handle, LV2_PATCH__value);
    f->urid = map->map(map->handle, LV2_ATOM__URID);
    f->float_t = map->map(map->handle, LV2_ATOM__Float);
    f->volume = map->map(map->handle, FX_PREFIX "volume");
    f->drive = map->map(map->handle, FX_PREFIX "drive");
    f->volume_value = 0.5f;
    f->drive_value = 1.0f;
    f->rate = rate;
    return f;
}
static void synth_connect_port(LV2_Handle h, uint32_t port, void *data)
{
    struct sfx *f = h;

    if (port == 1) f->out = data; else if (port == 2) f->events = data; else if (port == 3) f->notify = data;
}
static void synth_run(LV2_Handle h, uint32_t n)
{
    struct sfx *f = h;
    uint32_t i;

    if (f->events)
        LV2_ATOM_SEQUENCE_FOREACH(f->events, ev)
        {
            if (ev->body.type == f->midi && ev->body.size >= 3)
            {
                const uint8_t *m = (const uint8_t *)(&ev->body + 1);

                if ((m[0] & 0xf0) == 0x90 && m[2])
                    f->held++;
                else if (((m[0] & 0xf0) == 0x80 || (m[0] & 0xf0) == 0x90) && f->held > 0)
                    f->held--;
            }
            else if (ev->body.type == f->object || ev->body.type == f->blank)
            {
                const LV2_Atom_Object *obj = (const LV2_Atom_Object *)&ev->body;
                const LV2_Atom *prop = NULL, *val = NULL;

                if (obj->body.otype != f->set && obj->body.otype != f->put)
                    continue;
                lv2_atom_object_get(obj, f->property, &prop, f->value, &val, 0);
                if (!prop || !val || prop->type != f->urid || val->type != f->float_t)
                    continue;
                if (((const LV2_Atom_URID *)prop)->body == f->volume)
                    f->volume_value = ((const LV2_Atom_Float *)val)->body;
                else if (((const LV2_Atom_URID *)prop)->body == f->drive)
                    f->drive_value = ((const LV2_Atom_Float *)val)->body;
            }
        }
    for (i = 0; i < n; i++)
    {
        f->out[i] = f->held ? (float)sin(f->phase) * f->volume_value * f->drive_value : 0.0f;
        f->phase += 2.0 * M_PI * 440.0 / f->rate;
    }
    // reports nothing: the host cannot read back this plugin's own values, as with the plugin this fixture stands for
    if (f->notify)
        f->notify->atom.size = sizeof(LV2_Atom_Sequence_Body);
}
static void synth_cleanup(LV2_Handle h) { free(h); }
static LV2_State_Status synth_save(LV2_Handle h, LV2_State_Store_Function store, LV2_State_Handle state, uint32_t flags,
                                   const LV2_Feature *const *features)
{
    struct sfx *f = h;

    (void)flags; (void)features;
    return store(state, f->volume, &f->volume_value, sizeof(float), f->float_t, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
}
static LV2_State_Status synth_restore(LV2_Handle h, LV2_State_Retrieve_Function retrieve, LV2_State_Handle state, uint32_t flags,
                                      const LV2_Feature *const *features)
{
    struct sfx *f = h;
    size_t size = 0;
    uint32_t type = 0, got = 0;
    const float *v = retrieve(state, f->volume, &size, &type, &got);

    (void)flags; (void)features;
    if (v && size == sizeof(float) && type == f->float_t)
        f->volume_value = *v;
    return LV2_STATE_SUCCESS;
}
static const void *synth_extension_data(const char *uri)
{
    static const LV2_State_Interface state = { synth_save, synth_restore };

    return strcmp(uri, LV2_STATE__interface) == 0 ? &state : NULL;
}
static const LV2_Descriptor synth_descriptor = {
    FX_URI "#synth", synth_instantiate, synth_connect_port, NULL, synth_run, NULL, synth_cleanup, synth_extension_data,
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : index == 1 ? &midi_descriptor : index == 2 ? &synth_descriptor : NULL;
}
