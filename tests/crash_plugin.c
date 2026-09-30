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

/* A .clap whose entry point takes the process down, for the scanner's test. The crash is deliberate and
 * leaves no core dump and no coredumpctl entry: the process is made non-dumpable first. */

#include <stdlib.h>
#include <sys/prctl.h>
#include <clap/clap.h>

static bool entry_init(const char *path)
{
    (void)path;
    prctl(PR_SET_DUMPABLE, 0);
    abort();
}

static void entry_deinit(void)
{
}

static const void *entry_get_factory(const char *factory_id)
{
    (void)factory_id;
    return NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory
};
