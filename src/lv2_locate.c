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

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "lv2_locate.h"

// a manifest longer than this is not read: a manifest lists plugins, it does not describe them
#define MANIFEST_MAX_BYTES      (4u * 1024u * 1024u)

/* a character a prefixed name may continue with: what keeps "p:gain" from matching inside "p:gain2" or "xp:gain" */
static int name_char(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '-' || c == ':' || c == '%' || c == '\\' || (unsigned char)c >= 0x80;
}

/* `local` as a prefixed name may spell it unescaped: letters, digits, '_', '-', '.' (not last) and ':' */
static int plain_local(const char *local)
{
    size_t i, n = strlen(local);

    if (n == 0 || local[n - 1] == '.')
        return 0;
    for (i = 0; i < n; i++)
        if (!isalnum((unsigned char)local[i]) && local[i] != '_' && local[i] != '-' && local[i] != '.' && local[i] != ':')
            return 0;
    return 1;
}

/* whether `text`, which holds a declaration, holds the prefixed name `prefix`:`local` as a whole token, adding to
 * *compared the bytes of each local name it finds, of each prefix it compares and of each repeat it reads */
static int has_prefixed(const char *text, const char *prefix, size_t prefix_len, const char *local, size_t *compared)
{
    const size_t local_len = strlen(local);
    const char *at = strstr(text + 1, local), *end = text, *next, *p, *start;
    size_t n = 0;

    while (at)
    {
        if (at[-1] != ':')
        {
            n += local_len;
            next = strstr(at + 1, local);
            // the next local name overlaps this one and is not taken either: the text repeats with their distance as
            // period, so every local name in the repeat follows what the next one follows, not a ':', and one that
            // crosses its end has a byte of the repeat, a name character or '.', before its prefix
            if (next && next - at < (ptrdiff_t)local_len && next[-1] != ':')
            {
                for (p = next + local_len; *p == p[at - next]; p++)
                    ;
                n += (size_t)(p - next);
                next = strstr(p, local);
            }
            at = next;
            continue;
        }
        n += 1 + local_len;
        // a name whose prefix starts before `end` has the last name's ':' in its prefix, which holds none, or one of
        // its bytes, a name character or '.', before it
        if (at - end > (ptrdiff_t)prefix_len)
        {
            start = at - 1 - prefix_len;
            n += prefix_len;
            if (strncmp(start, prefix, prefix_len) == 0 && !(start > text && (name_char(start[-1]) || start[-1] == '.'))
                && !name_char(at[local_len]))
                break;
        }
        end = at += local_len;
        at = strstr(at, local);
    }
    *compared += n;
    return at != NULL;
}

int lv2_locate_manifest_names(const char *text, const char *uri)
{
    size_t readings = 0, compared = 0;

    return lv2_locate_manifest_readings(text, uri, &readings, &compared);
}

int lv2_locate_manifest_readings(const char *text, const char *uri, size_t *readings, size_t *compared)
{
    const size_t uri_len = strlen(uri);
    const char *at;
    char *whole;
    int found, read_again = 0;

    if (uri_len == 0)
        return 0;
    whole = malloc(uri_len + 3);
    if (!whole)
        return 0;
    snprintf(whole, uri_len + 3, "<%s>", uri);
    found = strstr(text, whole) != NULL;
    free(whole);
    if (found)
        return 1;

    // @prefix p: <ns> . (Turtle) and PREFIX p: <ns> (SPARQL), each against the URI
    for (at = text; *at; at++)
    {
        const char *p, *name, *ns, *ns_end;
        size_t name_len, ns_len;

        if (*at == '@' && strncmp(at + 1, "prefix", 6) == 0)
            p = at + 7;
        else if (strncasecmp(at, "PREFIX", 6) == 0 && (at == text || isspace((unsigned char)at[-1])))
            p = at + 6;
        else
            continue;
        if (!isspace((unsigned char)*p))
            continue;
        while (isspace((unsigned char)*p))
            p++;
        name = p;
        while (*p && *p != ':' && !isspace((unsigned char)*p))
            p++;
        if (*p != ':')
            continue;
        name_len = (size_t)(p - name);
        p++;
        while (isspace((unsigned char)*p))
            p++;
        if (*p != '<')
            continue;
        ns = p + 1;
        ns_end = strchr(ns, '>');
        if (!ns_end)
            break;
        ns_len = (size_t)(ns_end - ns);
        if (ns_len < uri_len && strncmp(uri, ns, ns_len) == 0 && plain_local(uri + ns_len))
        {
            // each such declaration reads the whole text for its name: past the most a manifest is read for, it is a
            // candidate unread, so no manifest costs more than LV2_LOCATE_PREFIXES_MAX readings of it
            if (++read_again > LV2_LOCATE_PREFIXES_MAX)
                return 1;
            ++*readings;
            if (has_prefixed(text, name, name_len, uri + ns_len, compared))
                return 1;
        }
        at = ns_end;
    }
    return 0;
}

static char *read_manifest(const char *bundle)
{
    char file[PATH_MAX];
    struct stat st;
    char *text;
    FILE *f;
    size_t got;

    if ((size_t)snprintf(file, sizeof(file), "%s/manifest.ttl", bundle) >= sizeof(file))
        return NULL;
    if (stat(file, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || (unsigned long long)st.st_size > MANIFEST_MAX_BYTES)
        return NULL;
    f = fopen(file, "r");
    if (!f)
        return NULL;
    text = malloc((size_t)st.st_size + 1);
    got = text ? fread(text, 1, (size_t)st.st_size, f) : 0;
    fclose(f);
    if (!text)
        return NULL;
    text[got] = '\0';
    return text;
}

/* one directory of the path: each bundle in it, in name order */
static int walk_dir(const char *dir, const char *uri, lv2_locate_fn fn, void *ctx)
{
    struct dirent **names = NULL;
    int n, i, ret = 0;

    n = scandir(dir, &names, NULL, alphasort);
    for (i = 0; i < n; i++)
    {
        char bundle[PATH_MAX];
        struct stat st;
        char *text;

        if (ret == 0 && names[i]->d_name[0] != '.'
            && (size_t)snprintf(bundle, sizeof(bundle), "%s/%s", dir, names[i]->d_name) < sizeof(bundle)
            && stat(bundle, &st) == 0 && S_ISDIR(st.st_mode) && (text = read_manifest(bundle)) != NULL)
        {
            const int names_it = lv2_locate_manifest_names(text, uri);

            free(text);
            if (names_it)
                ret = fn(bundle, ctx);
        }
        free(names[i]);
    }
    free(names);
    return ret;
}

int lv2_locate(const char *path, const char *uri, lv2_locate_fn fn, void *ctx)
{
    const char *home = getenv("HOME");
    char *copy, *save = NULL, *dir;
    int ret = 0;

    if (!path)
        path = getenv("LV2_PATH");
    if (!path || !*path)
        path = LV2_LOCATE_DEFAULT_PATH;
    copy = strdup(path);
    if (!copy)
        return 0;
    for (dir = strtok_r(copy, ":", &save); dir && ret == 0; dir = strtok_r(NULL, ":", &save))
    {
        char expanded[PATH_MAX];
        size_t n;

        if (dir[0] == '~' && (dir[1] == '/' || dir[1] == '\0'))
        {
            if (!home || (size_t)snprintf(expanded, sizeof(expanded), "%s%s", home, dir + 1) >= sizeof(expanded))
                continue;
        }
        else if ((size_t)snprintf(expanded, sizeof(expanded), "%s", dir) >= sizeof(expanded))
            continue;
        n = strlen(expanded);
        while (n > 1 && expanded[n - 1] == '/')
            expanded[--n] = '\0';
        if (expanded[0] != '/')
            continue;       // a relative directory would depend on where the host was started
        ret = walk_dir(expanded, uri, fn, ctx);
    }
    free(copy);
    return ret;
}
