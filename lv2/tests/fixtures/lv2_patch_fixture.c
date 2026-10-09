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

#include <stdlib.h>
#include <string.h>

#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/patch/patch.h>
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

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : NULL;
}
