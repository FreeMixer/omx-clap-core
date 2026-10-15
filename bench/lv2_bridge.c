/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 */
/*
 * lv2_bridge.c: a bench-only .clap that hands omx-clap-host the LV2 plugins of the bundles named in
 * OMX_BENCH_LV2_BUNDLES (absolute directories, ':'-separated) through libomx-clap-lv2, so the bench can measure
 * "omx-clap-host + the adapter" with the host as it is. Its factory lists every plugin of every bundle, each by its LV2
 * URI, and creates one through the bundle's own entry. Built into build/bench only: the adapter is never a .clap a
 * third-party host loads (omx_clap_lv2.h), and this file is no exception outside the bench.
 */
#include <stdlib.h>
#include <string.h>

#include <clap/clap.h>

#include "omx_clap_lv2.h"

#define MAX_BUNDLES 16

static const clap_plugin_entry_t *entries[MAX_BUNDLES];
static const clap_plugin_factory_t *factories[MAX_BUNDLES];
static unsigned n_entries;

static uint32_t count(const clap_plugin_factory_t *f)
{
    uint32_t c = 0;

    (void)f;
    for (unsigned i = 0; i < n_entries; i++)
        c += factories[i]->get_plugin_count(factories[i]);
    return c;
}

static const clap_plugin_descriptor_t *descriptor(const clap_plugin_factory_t *f, uint32_t index)
{
    (void)f;
    for (unsigned i = 0; i < n_entries; i++)
    {
        const uint32_t c = factories[i]->get_plugin_count(factories[i]);

        if (index < c)
            return factories[i]->get_plugin_descriptor(factories[i], index);
        index -= c;
    }
    return NULL;
}

static const clap_plugin_t *create(const clap_plugin_factory_t *f, const clap_host_t *host, const char *id)
{
    (void)f;
    for (unsigned i = 0; i < n_entries; i++)
    {
        const uint32_t c = factories[i]->get_plugin_count(factories[i]);

        for (uint32_t k = 0; k < c; k++)
        {
            const clap_plugin_descriptor_t *d = factories[i]->get_plugin_descriptor(factories[i], k);

            if (d && d->id && !strcmp(d->id, id))
                return factories[i]->create_plugin(factories[i], host, id);
        }
    }
    return NULL;
}

static const clap_plugin_factory_t factory = {count, descriptor, create};

static bool entry_init(const char *path)
{
    static const omx_clap_lv2_config_t config = {omx_clap_lv2_provided, 1000u, 4096u, 0u, 100u, NULL};
    char why[OMX_CLAP_LV2_WHY_MAX];
    const char *list = getenv("OMX_BENCH_LV2_BUNDLES");
    char *copy, *save = NULL;

    (void)path;
    if (!list || omx_clap_lv2_configure(&config, why) != 0)
        return false;
    copy = strdup(list);
    for (char *b = strtok_r(copy, ":", &save); b && n_entries < MAX_BUNDLES; b = strtok_r(NULL, ":", &save))
    {
        const clap_plugin_entry_t *e = omx_clap_lv2_entry(b, why);

        if (!e || !e->init(b))
            continue;
        factories[n_entries] = e->get_factory(CLAP_PLUGIN_FACTORY_ID);
        if (!factories[n_entries])
            continue;
        entries[n_entries++] = e;
    }
    free(copy);
    return n_entries > 0;
}

static void entry_deinit(void)
{
    for (unsigned i = 0; i < n_entries; i++)
    {
        entries[i]->deinit();
        omx_clap_lv2_entry_release(entries[i]);
    }
    n_entries = 0;
}

static const void *entry_factory(const char *id)
{
    return strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? NULL : &factory;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entry_init, entry_deinit, entry_factory};
