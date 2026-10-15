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
 * lv2_path_fixture.c: the state:makePath plugins of omx-path-fixture.lv2, test fixtures and never shipped plugins.
 *
 *   urn:openmixer:test:path-fixture   out = in x gain. Its save writes the `level` control's value into a file it asks
 *                                     makePath for (takes/level.txt), and stores the path mapPath abstracts from it as
 *                                     an atom:Path; its restore reads that file and makes what it holds the gain. So
 *                                     the gain after a load is the level saved, read back through the file.
 *   ...#escape                        asks makePath for "../outside.txt" and for an absolute path at instantiate, and
 *                                     writes a file wherever it is answered a path; and for never/saved.txt, which it
 *                                     writes and never saves.
 *
 * What either saw is in lv2_path_probe (lv2_path_probe.h). Nothing is allocated or locked in run().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lv2/atom/atom.h>
#include <lv2/core/lv2.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>

#include "lv2_path_probe.h"

LV2_SYMBOL_EXPORT struct lv2_path_probe lv2_path_probe;

struct pf
{
    const float *in, *level;
    float *out;
    float gain;
    LV2_State_Make_Path *make_path;
    LV2_URID file, path_t;
};

static const void *feature(const LV2_Feature *const *features, const char *uri)
{
    for (uint32_t i = 0; features && features[i]; i++)
        if (strcmp(features[i]->URI, uri) == 0)
            return features[i]->data;
    return NULL;
}

/* a file at `path` holding `text`: 0, or -1 */
static int write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    int ok;

    if (!f)
        return -1;
    ok = fputs(text, f) >= 0;
    return fclose(f) == 0 && ok ? 0 : -1;
}

static LV2_Handle instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *features)
{
    const LV2_URID_Map *map = feature(features, LV2_URID__map);
    struct pf *p;

    (void)rate;
    (void)path;
    if (!map || !(p = calloc(1, sizeof(*p))))
        return NULL;
    p->make_path = (LV2_State_Make_Path *)feature(features, LV2_STATE__makePath);
    p->file = map->map(map->handle, LV2_PATH_FIXTURE_URI "#file");
    p->path_t = map->map(map->handle, LV2_ATOM__Path);
    p->gain = 1.0f;
    lv2_path_probe.had_make_path = p->make_path != NULL;
    if (strcmp(d->URI, LV2_PATH_FIXTURE_URI "#escape") == 0 && p->make_path)
    {
        char *out = p->make_path->path(p->make_path->handle, "../outside.txt");
        char *abs = p->make_path->path(p->make_path->handle, "/tmp/omx-path-fixture-absolute.txt");

        char *inside = p->make_path->path(p->make_path->handle, "never/saved.txt");

        // one path inside, written and never saved: nothing of it may outlive the instance
        snprintf(lv2_path_probe.made, sizeof(lv2_path_probe.made), "%s", inside ? inside : "");
        if (inside)
            write_file(inside, "unsaved\n");
        free(inside);
        lv2_path_probe.escape_null = out == NULL;
        lv2_path_probe.absolute_null = abs == NULL;
        if (out)
            write_file(out, "escaped\n");
        if (abs)
            write_file(abs, "escaped\n");
        free(out);
        free(abs);
    }
    return p;
}

static void connect_port(LV2_Handle h, uint32_t port, void *data)
{
    struct pf *p = h;

    switch (port)
    {
    case 0: p->in = data; break;
    case 1: p->out = data; break;
    case 2: p->level = data; break;
    default: break;
    }
}

static void run(LV2_Handle h, uint32_t n)
{
    struct pf *p = h;
    uint32_t i;

    for (i = 0; i < n; i++)
        p->out[i] = p->in[i] * p->gain;
}

static void cleanup(LV2_Handle h)
{
    free(h);
}

static LV2_State_Status save(LV2_Handle h, LV2_State_Store_Function store, LV2_State_Handle sh, uint32_t flags,
                             const LV2_Feature *const *features)
{
    struct pf *p = h;
    const LV2_State_Map_Path *map_path = feature(features, LV2_STATE__mapPath);
    char text[32], *made, *stored;
    LV2_State_Status st;

    (void)flags;
    lv2_path_probe.made[0] = lv2_path_probe.stored[0] = '\0';
    if (!p->make_path || !map_path)
        return LV2_STATE_ERR_NO_FEATURE;
    made = p->make_path->path(p->make_path->handle, "takes/level.txt");
    if (!made)
        return LV2_STATE_ERR_UNKNOWN;
    snprintf(lv2_path_probe.made, sizeof(lv2_path_probe.made), "%s", made);
    snprintf(text, sizeof(text), "%.9g\n", (double)*p->level);
    if (write_file(made, text) != 0)
    {
        free(made);
        return LV2_STATE_ERR_UNKNOWN;
    }
    stored = map_path->abstract_path(map_path->handle, made);
    free(made);
    if (!stored)
        return LV2_STATE_ERR_UNKNOWN;
    snprintf(lv2_path_probe.stored, sizeof(lv2_path_probe.stored), "%s", stored);
    st = store(sh, p->file, stored, strlen(stored) + 1u, p->path_t, LV2_STATE_IS_POD);
    free(stored);
    return st;
}

static LV2_State_Status restore(LV2_Handle h, LV2_State_Retrieve_Function retrieve, LV2_State_Handle sh, uint32_t flags,
                                const LV2_Feature *const *features)
{
    struct pf *p = h;
    const LV2_State_Map_Path *map_path = feature(features, LV2_STATE__mapPath);
    size_t size = 0;
    uint32_t type = 0, vflags = 0;
    const char *value = retrieve(sh, p->file, &size, &type, &vflags);
    char *path;
    FILE *f;

    (void)flags;
    lv2_path_probe.restored = -1.0f;
    lv2_path_probe.restored_from[0] = '\0';
    if (!value || type != p->path_t || !map_path)
        return LV2_STATE_ERR_NO_PROPERTY;
    path = map_path->absolute_path(map_path->handle, value);
    if (!path)
        return LV2_STATE_ERR_UNKNOWN;
    snprintf(lv2_path_probe.restored_from, sizeof(lv2_path_probe.restored_from), "%s", path);
    f = fopen(path, "r");
    free(path);
    if (f)
    {
        float v;

        if (fscanf(f, "%f", &v) == 1)
        {
            p->gain = v;
            lv2_path_probe.restored = v;
        }
        fclose(f);
    }
    return LV2_STATE_SUCCESS;
}

static const void *extension_data(const char *uri)
{
    static const LV2_State_Interface state = { save, restore };

    return strcmp(uri, LV2_STATE__interface) == 0 ? &state : NULL;
}

static const LV2_Descriptor g_descriptors[2] = {
    { LV2_PATH_FIXTURE_URI, instantiate, connect_port, NULL, run, NULL, cleanup, extension_data },
    { LV2_PATH_FIXTURE_URI "#escape", instantiate, connect_port, NULL, run, NULL, cleanup, extension_data },
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    return index < 2u ? &g_descriptors[index] : NULL;
}
