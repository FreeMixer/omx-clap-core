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
************************************************************************************************************************
*
* Which bundle on the LV2 path may hold a plugin URI, read from the text of each bundle's manifest.ttl and nothing else:
* no lilv, no world. A bundle is a candidate when its manifest names the URI whole (<uri>), or as a prefixed name whose
* prefix it declares (@prefix p: <ns> . and p:local, where ns + local is the URI). The caller opens a candidate to know;
* a manifest that spells the URI some other way (a relative IRI against @base, an escaped character) is not found.
*
************************************************************************************************************************
*/

#ifndef LV2_LOCATE_H
#define LV2_LOCATE_H

#include <stddef.h>

/* the path searched when LV2_PATH is not set, lilv's own default with the 64-bit directories added */
#define LV2_LOCATE_DEFAULT_PATH "~/.lv2:/usr/local/lib64/lv2:/usr/local/lib/lv2:/usr/lib64/lv2:/usr/lib/lv2"

/* Called with each candidate bundle, an absolute directory with no trailing '/', in the order of the path and, inside a
 * directory of it, in name order. A nonzero return ends the walk and is what lv2_locate returns. */
typedef int (*lv2_locate_fn)(const char *bundle, void *ctx);

/* Walk `path` (':'-separated, a leading "~/" the home directory; NULL: LV2_PATH, else LV2_LOCATE_DEFAULT_PATH) for the
 * bundles whose manifest may hold `uri`: 0 when the walk ended with no callback taking a bundle, or the callback's
 * nonzero return. */
int lv2_locate(const char *path, const char *uri, lv2_locate_fn fn, void *ctx);

/* Whether the text of a manifest names `uri`, as lv2_locate reads it: 1 or 0. */
int lv2_locate_manifest_names(const char *text, const char *uri);

#endif
