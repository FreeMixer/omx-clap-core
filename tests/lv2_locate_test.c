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

/* Which bundles on an LV2 path the host opens for a URI (src/lv2_locate.c): the manifest's text against each way a URI
 * is spelled in it, then a walk over a path of temporary directories, its order, its stop and its "~/". */

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lv2_locate.h"
#include "test_util.h"

#define URI "urn:example:plugins#gain"

static void names(void)
{
    static const struct { const char *text; int want; const char *what; } cases[] =
    {
        { "<" URI "> a lv2:Plugin .", 1, "the URI whole" },
        { "@prefix ex: <urn:example:plugins#> .\nex:gain a lv2:Plugin .", 1, "a prefixed name of a declared prefix" },
        { "PREFIX ex: <urn:example:plugins#>\nex:gain a lv2:Plugin .", 1, "a SPARQL PREFIX" },
        { "@prefix : <urn:example:plugins#> .\n:gain a lv2:Plugin .", 1, "the empty prefix" },
        { "@prefix ex: <urn:example:plugins#> .\nex:gain2 a lv2:Plugin .", 0, "a longer local name is another plugin" },
        { "@prefix ex: <urn:example:plugins#> .\nxex:gain a lv2:Plugin .", 0, "a longer prefix is another prefix" },
        { "@prefix ex: <urn:example:other#> .\nex:gain a lv2:Plugin .", 0, "the local name under another namespace" },
        { "<urn:example:plugins#gain2> a lv2:Plugin .", 0, "a URI the given one is a prefix of" },
        { "@prefix ex: <urn:example:plugins#> .\nex:gain.", 1, "a prefixed name that ends its statement" },
        { "@base <urn:example:plugins> .\n<#gain> a lv2:Plugin .", 0, "a relative IRI against @base: not read" },
        { "", 0, "an empty manifest" },
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        CHECK(lv2_locate_manifest_names(cases[i].text, URI) == cases[i].want, "manifest: %s -> %d", cases[i].what, cases[i].want);
    CHECK(lv2_locate_manifest_names("<urn:a:b>", "") == 0, "manifest: an empty URI is named by nothing");
}

static void on_alarm(int sig)
{
    static const char m[] = "FAIL manifest: many declarations of the URI's namespace, still reading after 20 s\n";

    (void)sig;
    if (write(STDOUT_FILENO, m, sizeof(m) - 1) < 0)
        _exit(2);
    _exit(1);
}

/* A manifest that declares the URI's namespace again and again: each declaration reads the whole text for its name, so
 * one of the largest size read (4 MiB) took minutes, a stall of every lookup on its path. */
static void many_prefixes(void)
{
    static const char line[] = "@prefix a: <urn:example:plugins#> .\n";
    const size_t len = sizeof(line) - 1, n = (4u << 20) / len;
    char *text = malloc(n * len + 1);
    size_t i;
    int got;

    if (!text)
    {
        CHECK(0, "manifest: memory for a 4 MiB manifest");
        return;
    }
    for (i = 0; i < n; i++)
        memcpy(text + i * len, line, len);
    text[n * len] = '\0';
    signal(SIGALRM, on_alarm);
    alarm(20);
    got = lv2_locate_manifest_names(text, URI);
    alarm(0);
    CHECK(got == 1, "manifest: %zu declarations of the URI's namespace and no name: a candidate, read in bounded work", n);
    text[LV2_LOCATE_PREFIXES_MAX * len] = '\0';
    CHECK(lv2_locate_manifest_names(text, URI) == 0,
          "manifest: %d declarations of the URI's namespace and no name: read whole, not a candidate", LV2_LOCATE_PREFIXES_MAX);
    free(text);
}

static void bundle(const char *dir, const char *name, const char *manifest)
{
    char path[512];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    mkdir(path, 0755);
    if (!manifest)
        return;
    snprintf(path, sizeof(path), "%s/%s/manifest.ttl", dir, name);
    f = fopen(path, "w");
    fputs(manifest, f);
    fclose(f);
}

struct seen
{
    char list[2048];
    int stop_at;
    int calls;
};

static int collect(const char *found, void *ctx)
{
    struct seen *s = ctx;
    const char *base = strrchr(found, '/');

    s->calls++;
    strncat(s->list, base ? base + 1 : found, sizeof(s->list) - strlen(s->list) - 2);
    strcat(s->list, " ");
    return s->calls == s->stop_at ? 7 : 0;
}

static void walk(void)
{
    char root[] = "/tmp/lv2-locate-test.XXXXXX";
    char one[600], two[600], home[600], path[2048], file[700];
    struct seen s;
    FILE *f;
    int ret;

    if (!mkdtemp(root))
    {
        CHECK(0, "walk: a temporary directory");
        return;
    }
    snprintf(one, sizeof(one), "%s/one", root);
    snprintf(two, sizeof(two), "%s/two", root);
    snprintf(home, sizeof(home), "%s/home", root);
    mkdir(one, 0755);
    mkdir(two, 0755);
    mkdir(home, 0755);
    snprintf(path, sizeof(path), "%s/.lv2", home);
    mkdir(path, 0755);
    bundle(one, "c.lv2", "<" URI "> a lv2:Plugin .");
    bundle(one, "a.lv2", "<urn:example:plugins#other> a lv2:Plugin .");
    bundle(one, "b.lv2", "@prefix ex: <urn:example:plugins#> .\nex:gain a lv2:Plugin .");
    bundle(one, "empty.lv2", NULL);
    bundle(one, ".hidden.lv2", "<" URI "> a lv2:Plugin .");
    snprintf(file, sizeof(file), "%s/plain-file", one);
    f = fopen(file, "w");
    fputs("<" URI ">", f);
    fclose(f);
    bundle(two, "d.lv2", "<" URI "> a lv2:Plugin .");
    bundle(path, "e.lv2", "<" URI "> a lv2:Plugin .");

    memset(&s, 0, sizeof(s));
    snprintf(path, sizeof(path), "%s:%s/:relative/dir", one, two);
    ret = lv2_locate(path, URI, collect, &s);
    CHECK(ret == 0 && strcmp(s.list, "b.lv2 c.lv2 d.lv2 ") == 0,
          "walk: the candidates in path and name order, none hidden, empty or plain, a relative directory skipped (%s)", s.list);

    memset(&s, 0, sizeof(s));
    s.stop_at = 2;
    ret = lv2_locate(path, URI, collect, &s);
    CHECK(ret == 7 && strcmp(s.list, "b.lv2 c.lv2 ") == 0, "walk: a callback that takes a bundle ends the walk, its return returned (%d, %s)", ret, s.list);

    memset(&s, 0, sizeof(s));
    setenv("HOME", home, 1);
    snprintf(path, sizeof(path), "~/.lv2:%s", two);
    ret = lv2_locate(path, URI, collect, &s);
    CHECK(ret == 0 && strcmp(s.list, "e.lv2 d.lv2 ") == 0, "walk: ~/ is the home directory (%s)", s.list);

    memset(&s, 0, sizeof(s));
    setenv("LV2_PATH", two, 1);
    ret = lv2_locate(NULL, URI, collect, &s);
    CHECK(ret == 0 && strcmp(s.list, "d.lv2 ") == 0, "walk: no path given is LV2_PATH (%s)", s.list);

    memset(&s, 0, sizeof(s));
    ret = lv2_locate(path, "urn:example:plugins#none", collect, &s);
    CHECK(ret == 0 && s.calls == 0, "walk: a URI no manifest names reaches no callback");

    snprintf(path, sizeof(path), "rm -rf '%s'", root);
    if (system(path) != 0)
        fprintf(stderr, "could not remove %s\n", root);
}

int main(void)
{
    names();
    many_prefixes();
    walk();
    if (g_failures)
    {
        printf("lv2 locate test FAILED (%d)\n", g_failures);
        return 1;
    }
    printf("lv2 locate test ok\n");
    return 0;
}
