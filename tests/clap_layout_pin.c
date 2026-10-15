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

#include <stdio.h>
#include <string.h>
#include <plugin-hostd/protocol.h>

#include "layout_pin.h"
#include "omx_clap_raw.h"

static void print_sink(void *ctx, const char *data, size_t size)
{
    fwrite(data, 1, size, (FILE *)ctx);
}

int main(int argc, char **argv)
{
    static const clap_host_t host = OMX_CLAP_RAW_NULL_HOST("clap_layout_pin");
    char hex[PHD_SHA256_HEX_LEN + 1];
    omx_clap_raw_t raw;
    const clap_plugin_t *plugin;
    int bytes = argc == 4 && !strcmp(argv[1], "-b"), ret;
    const char *why;

    if (argc != 3 + bytes)
    {
        fprintf(stderr, "usage: %s [-b] <file.clap> <plugin id>\n", argv[0]);
        return 2;
    }
    if ((why = omx_clap_raw_open(&raw, argv[1 + bytes])))
    {
        fprintf(stderr, "%s: %s\n", argv[1 + bytes], why);
        return 1;
    }
    if (!(plugin = omx_clap_raw_create(&raw, &host, argv[2 + bytes])))
    {
        fprintf(stderr, "%s: cannot create or init %s\n", argv[1 + bytes], argv[2 + bytes]);
        omx_clap_raw_close(&raw);
        return 1;
    }
    if (bytes)
        ret = layout_pin_write(plugin, plugin->get_extension(plugin, CLAP_EXT_PARAMS), print_sink, stdout, NULL);
    else if ((ret = layout_pin_write(plugin, plugin->get_extension(plugin, CLAP_EXT_PARAMS), NULL, NULL, hex)) == 0)
        printf("%s%s%s\n", PHD_PIN_LAYOUT_SCHEME, PHD_PIN_SCHEME_SEPARATOR, hex);
    plugin->destroy(plugin);
    omx_clap_raw_close(&raw);
    return ret == 0 ? 0 : 1;
}
