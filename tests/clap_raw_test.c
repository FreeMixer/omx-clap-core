// SPDX-License-Identifier: GPL-3.0-or-later
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
 * clap_raw_test.c: the raw CLAP open of src/omx_clap_raw.h against the fixtures of this tree.
 *
 *   make test-raw
 *
 * argument 1 is the fake effect (tests/fake.clap), argument 2 the plugin whose init fails
 * (tests/fault-2.clap). The fake reads the host's preset-load extension when it is created and
 * reports it as parameter 3, so the null host's absent extensions are read back from the plugin.
 */
#include "../src/omx_clap_raw.h"
#include "test_util.h"

static double param(const clap_plugin_t *plugin, clap_id id)
{
    const clap_plugin_params_t *params = plugin->get_extension(plugin, CLAP_EXT_PARAMS);
    double value = -1.0;

    if (!params || !params->get_value(plugin, id, &value))
        return -1.0;
    return value;
}

int main(int argc, char **argv)
{
    static const clap_host_t host = OMX_CLAP_RAW_NULL_HOST("clap_raw_test");
    omx_clap_raw_t raw;
    const clap_plugin_t *plugin;
    const char *why;

    if (argc != 3)
    {
        fprintf(stderr, "usage: %s <fake.clap> <fault-2.clap>\n", argv[0]);
        return 2;
    }

    why = omx_clap_raw_open(&raw, argv[1]);
    CHECK(!why && raw.lib && raw.entry && raw.factory, "the fake opens: dlopen, clap_entry inited, the plugin factory (%s)",
          why ? why : "no error");
    if (why)
        return report("FAILED");
    CHECK(raw.factory->get_plugin_count(raw.factory) > 1, "the factory lists the fake's plugins");

    plugin = omx_clap_raw_create(&raw, &host, FAKE_PASSTHROUGH);
    CHECK(plugin && !strcmp(plugin->desc->id, FAKE_PASSTHROUGH), "the plugin named is created and inited");
    if (plugin)
    {
        CHECK(param(plugin, FAKE_PARAM_HOST_PRESET) == 0.0, "the null host offers no preset-load extension");
        CHECK(param(plugin, FAKE_PARAM_ACTIVATIONS) == 0.0, "the plugin is inited, never activated");
        CHECK(!strcmp(host.name, "clap_raw_test") && host.host_data == NULL && host.get_extension(&host, CLAP_EXT_LOG) == NULL,
              "the null host carries its name and answers no extension");
        host.request_restart(&host);
        host.request_process(&host);
        host.request_callback(&host);
        plugin->destroy(plugin);
    }

    plugin = omx_clap_raw_create(&raw, &host, NULL);
    CHECK(plugin && !strcmp(plugin->desc->id, raw.factory->get_plugin_descriptor(raw.factory, 0)->id),
          "no id creates the factory's first plugin");
    if (plugin)
        plugin->destroy(plugin);
    CHECK(omx_clap_raw_create(&raw, &host, "org.omx-clap-host.test.none") == NULL, "an id the factory does not list creates nothing");

    omx_clap_raw_close(&raw);
    CHECK(!raw.lib && !raw.entry && !raw.factory, "close deinits the entry, dlcloses the file and zeroes the handle");
    omx_clap_raw_close(&raw);
    CHECK(!raw.lib, "a second close is harmless");

    why = omx_clap_raw_open(&raw, argv[2]);
    plugin = why ? NULL : omx_clap_raw_create(&raw, &host, NULL);
    CHECK(!why && !plugin, "a plugin whose init fails is destroyed and not returned");
    omx_clap_raw_close(&raw);

    why = omx_clap_raw_open(&raw, "libm.so.6");
    CHECK(why && !strcmp(why, "no clap_entry") && !raw.lib && !raw.entry, "a library without clap_entry is refused and unloaded");
    why = omx_clap_raw_open(&raw, "/nonexistent/none.clap");
    CHECK(why && !raw.lib && !raw.entry && !raw.factory, "a file that does not exist is refused with dlerror's text (%s)",
          why ? why : "none");

    return report("clap_raw_test: ok");
}
