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
* a manifest that spells the URI some other way (a relative IRI against @base, an escaped character) is not found. Each
* declaration of a namespace the URI starts with is a reading of the whole text for its name; a manifest that makes more
* than LV2_LOCATE_PREFIXES_MAX of them is a candidate without the rest, so a manifest's cost is linear in its size.
*
************************************************************************************************************************
*/

#ifndef LV2_LOCATE_H
#define LV2_LOCATE_H

#include <stddef.h>

/* the path searched when LV2_PATH is not set, lilv's own default with the 64-bit directories added */
#define LV2_LOCATE_DEFAULT_PATH "~/.lv2:/usr/local/lib64/lv2:/usr/local/lib/lv2:/usr/lib64/lv2:/usr/lib/lv2"

/* the declarations of a namespace the URI starts with a manifest is read for, at most; one more makes it a candidate */
#define LV2_LOCATE_PREFIXES_MAX 16

/* lv2_locate_manifest_names, lv2_locate_manifest_readings and lv2_locate return this when a manifest could not be
 * read because an allocation of the reader's own failed, as when memory is out: a read failure, never the answer
 * that no manifest names the URI. It is not 0 (not named) or 1 (named), and not a callback's nonzero return, which
 * lv2_locate_fn gives only positive; a caller that tests the result as a boolean must test for this first. */
#define LV2_LOCATE_READ_FAILED (-1)

/* Called with each candidate bundle, an absolute directory with no trailing '/', in the order of the path and, inside a
 * directory of it, in name order. A nonzero return ends the walk and is what lv2_locate returns; it must be positive,
 * so that it is never read as LV2_LOCATE_READ_FAILED. */
typedef int (*lv2_locate_fn)(const char *bundle, void *ctx);

/* Walk `path` (':'-separated, a leading "~/" the home directory; NULL: LV2_PATH, else LV2_LOCATE_DEFAULT_PATH) for the
 * bundles whose manifest may hold `uri`: 0 when the walk ended with no callback taking a bundle, the callback's
 * nonzero return, or LV2_LOCATE_READ_FAILED when a manifest's own allocation failed before every bundle was looked at. */
int lv2_locate(const char *path, const char *uri, lv2_locate_fn fn, void *ctx);

/* Whether the text of a manifest names `uri`, as lv2_locate reads it: 1, 0, or LV2_LOCATE_READ_FAILED when the
 * reader's own allocation failed. */
int lv2_locate_manifest_names(const char *text, const char *uri);

/* lv2_locate_manifest_names, adding to *readings each reading of the whole text for a prefixed name, and to
 * *compared the bytes of each local name a reading finds, of each prefix it compares and of each repeat it reads: the
 * work a manifest costs, which no manifest makes more than LV2_LOCATE_PREFIXES_MAX readings of. Nothing is added to
 * either when the reader's own allocation failed and it answers LV2_LOCATE_READ_FAILED. */
int lv2_locate_manifest_readings(const char *text, const char *uri, size_t *readings, size_t *compared);

#endif
