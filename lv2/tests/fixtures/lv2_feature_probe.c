// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The feature-probe test plugin (lv2_feature_probe.h): instantiate records what the host handed it and never refuses,
// so a host's feature options are measured from the plugin's side.

#include <stdlib.h>
#include <string.h>

#include <lv2/atom/atom.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/log/log.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#include "lv2_feature_probe.h"

#define EXPORT __attribute__((visibility("default")))

EXPORT struct lv2_feature_probe lv2_feature_probe;

struct fp
{
    const float *in;
    float *out;
};

static const struct
{
    const char *uri;
    uint32_t bit;
} g_known[] =
{
    { LV2_URID__map, LV2_FEATURE_PROBE_MAP },
    { LV2_URID__unmap, LV2_FEATURE_PROBE_UNMAP },
    { LV2_OPTIONS__options, LV2_FEATURE_PROBE_OPTIONS },
    { LV2_BUF_SIZE__boundedBlockLength, LV2_FEATURE_PROBE_BOUNDED },
    { LV2_WORKER__schedule, LV2_FEATURE_PROBE_WORKER },
    { LV2_STATE__loadDefaultState, LV2_FEATURE_PROBE_DEFAULT_STATE },
    { LV2_LOG__log, LV2_FEATURE_PROBE_LOG },
    { LV2_BUF_SIZE__fixedBlockLength, LV2_FEATURE_PROBE_FIXED },
    { LV2_BUF_SIZE__powerOf2BlockLength, LV2_FEATURE_PROBE_POW2 },
};

static void *feature(const LV2_Feature *const *features, const char *uri)
{
    for (int i = 0; features && features[i]; i++)
        if (!strcmp(features[i]->URI, uri))
            return features[i]->data;
    return NULL;
}

static void read_options(struct lv2_feature_probe *p, const LV2_URID_Map *map, const LV2_Options_Option *o)
{
    const LV2_URID rate = map->map(map->handle, LV2_PARAMETERS__sampleRate);
    const LV2_URID min = map->map(map->handle, LV2_BUF_SIZE__minBlockLength);
    const LV2_URID max = map->map(map->handle, LV2_BUF_SIZE__maxBlockLength);
    const LV2_URID nominal = map->map(map->handle, LV2_BUF_SIZE__nominalBlockLength);

    for (; o && o->key; o++)
    {
        if (o->key == rate && o->size == sizeof(float))
            p->has_rate = 1, p->rate = *(const float *)o->value;
        else if (o->key == min && o->size == sizeof(int32_t))
            p->has_min = 1, p->min_block = *(const int32_t *)o->value;
        else if (o->key == max && o->size == sizeof(int32_t))
            p->has_max = 1, p->max_block = *(const int32_t *)o->value;
        else if (o->key == nominal && o->size == sizeof(int32_t))
            p->has_nominal = 1, p->nominal_block = *(const int32_t *)o->value;
    }
}

static LV2_Handle instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *features)
{
    struct lv2_feature_probe *p = &lv2_feature_probe;
    const LV2_URID_Map *map = feature(features, LV2_URID__map);
    const LV2_URID_Unmap *unmap = feature(features, LV2_URID__unmap);
    const LV2_Log_Log *log = feature(features, LV2_LOG__log);
    const unsigned calls = p->instantiate_calls + 1u;

    (void)path;
    memset(p, 0, sizeof(*p));
    p->instantiate_calls = calls;
    strncpy(p->uri, d->URI, sizeof(p->uri) - 1);
    for (int i = 0; features && features[i]; i++)
    {
        uint32_t bit = LV2_FEATURE_PROBE_UNKNOWN;

        for (size_t k = 0; k < sizeof(g_known) / sizeof(g_known[0]); k++)
            if (!strcmp(features[i]->URI, g_known[k].uri))
                bit = g_known[k].bit;
        p->seen |= bit;
        if (i < 12)
            strncpy(p->order[i], features[i]->URI, sizeof(p->order[i]) - 1);
        p->n_features++;
    }
    if (map)
        read_options(p, map, feature(features, LV2_OPTIONS__options));
    if (map && unmap)
    {
        const char *back = unmap->unmap(unmap->handle, map->map(map->handle, "urn:openmixer:test:feature-probe#round"));

        p->unmap_round_trip = back && !strcmp(back, "urn:openmixer:test:feature-probe#round");
    }
    p->log_answer = -1;
    if (log)
        p->log_answer = log->printf(log->handle, map ? map->map(map->handle, LV2_LOG__Note) : 0, LV2_FEATURE_PROBE_LINE,
                                    (int)rate);
    return calloc(1, sizeof(struct fp));
}

static void connect_port(LV2_Handle h, uint32_t port, void *data)
{
    struct fp *f = h;

    if (port == 0)
        f->in = data;
    else if (port == 1)
        f->out = data;
}

static void run(LV2_Handle h, uint32_t n)
{
    struct fp *f = h;

    if (f->in && f->out)
        memmove(f->out, f->in, n * sizeof(float));
}

static void cleanup(LV2_Handle h)
{
    free(h);
}

static const LV2_Descriptor g_probe =
{
    LV2_FEATURE_PROBE_URI, instantiate, connect_port, NULL, run, NULL, cleanup, NULL
};

static const LV2_Descriptor g_needs_worker =
{
    LV2_FEATURE_PROBE_WORKER_URI, instantiate, connect_port, NULL, run, NULL, cleanup, NULL
};

EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    return index == 0 ? &g_probe : index == 1 ? &g_needs_worker : NULL;
}
