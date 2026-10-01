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

/* The layout pin of one plugin of a .clap, as a pin_set or a pin_expect carries it:
 *
 *   clap_layout_pin <file.clap> <plugin id>        prints omx-layout/1:<sha256>
 *   clap_layout_pin -b <file.clap> <plugin id>     prints the omx-layout/1 bytes the digest is taken over
 *
 * The plugin is created and initialised through CLAP alone, never activated, and destroyed. */

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <plugin-hostd/protocol.h>

#include "layout_pin.h"

static void print_sink(void *ctx, const char *data, size_t size)
{
    fwrite(data, 1, size, (FILE *)ctx);
}

static const void *no_extension(const clap_host_t *host, const char *id)
{
    (void)host; (void)id;
    return NULL;
}

static void no_request(const clap_host_t *host)
{
    (void)host;
}

int main(int argc, char **argv)
{
    static const clap_host_t host = {
        CLAP_VERSION_INIT, NULL, "clap_layout_pin", "omx-clap-host", "", "0", no_extension, no_request, no_request, no_request
    };
    char hex[PHD_SHA256_HEX_LEN + 1];
    const clap_plugin_entry_t *entry;
    const clap_plugin_factory_t *factory;
    const clap_plugin_t *plugin;
    int bytes = argc == 4 && !strcmp(argv[1], "-b"), ret;
    void *lib;

    if (argc != 3 + bytes)
    {
        fprintf(stderr, "usage: %s [-b] <file.clap> <plugin id>\n", argv[0]);
        return 2;
    }
    lib = dlopen(argv[1 + bytes], RTLD_NOW | RTLD_LOCAL);
    entry = lib ? dlsym(lib, "clap_entry") : NULL;
    if (!entry || !entry->init(argv[1 + bytes]))
    {
        fprintf(stderr, "%s: %s\n", argv[1 + bytes], lib ? "no clap_entry" : dlerror());
        return 1;
    }
    factory = entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    plugin = factory ? factory->create_plugin(factory, &host, argv[2 + bytes]) : NULL;
    if (!plugin || !plugin->init(plugin))
    {
        fprintf(stderr, "%s: cannot create or init %s\n", argv[1 + bytes], argv[2 + bytes]);
        return 1;
    }
    if (bytes)
        ret = layout_pin_write(plugin, plugin->get_extension(plugin, CLAP_EXT_PARAMS), print_sink, stdout, NULL);
    else if ((ret = layout_pin_write(plugin, plugin->get_extension(plugin, CLAP_EXT_PARAMS), NULL, NULL, hex)) == 0)
        printf("%s%s%s\n", PHD_PIN_LAYOUT_SCHEME, PHD_PIN_SCHEME_SEPARATOR, hex);
    plugin->destroy(plugin);
    entry->deinit();
    return ret == 0 ? 0 : 1;
}
