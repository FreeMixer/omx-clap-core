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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lv2_locate.h"
#include "test_util.h"

#define URI "urn:example:plugins#gain"

/* the allocator: malloc of exactly `g_fail_size` bytes fails once, armed by the test right before the one call whose
 * allocation it means to fail, so no allocation a libc call of its own makes can be mistaken for it */
static size_t g_fail_size;
static int g_failed;
extern void *__libc_malloc(size_t);
void *malloc(size_t n)
{
    if (g_fail_size && n == g_fail_size)
    {
        g_fail_size = 0;
        g_failed++;
        return NULL;
    }
    return __libc_malloc(n);
}

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

/* A manifest that declares the URI's namespace again and again: each declaration read the whole text for its name, so
 * one of the largest size read (4 MiB) cost its size squared, a stall of every lookup on its path. The work is counted
 * in readings of the whole text, never timed: a reader that reads once per declaration reads this one n times. */
static void many_prefixes(void)
{
    static const char line[] = "@prefix a: <urn:example:plugins#> .\n";
    const size_t len = sizeof(line) - 1, n = 1024;
    char *text = malloc(n * len + 1);
    size_t i, readings = 0, compared = 0;
    int got;

    if (!text)
    {
        CHECK(0, "manifest: memory for a manifest of %zu declarations", n);
        return;
    }
    for (i = 0; i < n; i++)
        memcpy(text + i * len, line, len);
    text[n * len] = '\0';
    got = lv2_locate_manifest_readings(text, URI, &readings, &compared);
    CHECK(got == 1 && readings <= LV2_LOCATE_PREFIXES_MAX,
          "manifest: %zu declarations of the URI's namespace and no name: a candidate, read %zu times (at most %d)", n,
          readings, LV2_LOCATE_PREFIXES_MAX);
    text[LV2_LOCATE_PREFIXES_MAX * len] = '\0';
    readings = compared = 0;
    got = lv2_locate_manifest_readings(text, URI, &readings, &compared);
    CHECK(got == 0 && readings == LV2_LOCATE_PREFIXES_MAX,
          "manifest: %d declarations of the URI's namespace and no name: read whole for each (%zu times), not a candidate",
          LV2_LOCATE_PREFIXES_MAX, readings);
    free(text);
}

/* `n` copies of `s` at text + *len, and the NUL after them; nothing when the memory for the text was not had */
static void put(char *text, size_t *len, const char *s, size_t n)
{
    const size_t k = strlen(s);

    if (!text)
        return;
    for (; n > 0; n--, *len += k)
        memcpy(text + *len, s, k);
    text[*len] = '\0';
}

/* One lookup of `uri` in `text`, which it frees: no candidate, read `want` times, and at most twice the text's length
 * compared, and some when `some`. The work is counted in the bytes compared, never timed. */
static void counted(const char *what, char *text, const char *uri, size_t want, int some)
{
    size_t readings = 0, compared = 0, len;

    if (!text)
    {
        CHECK(0, "manifest: memory for %s", what);
        return;
    }
    len = strlen(text);
    CHECK(lv2_locate_manifest_readings(text, uri, &readings, &compared) == 0 && readings == want
              && (!some || compared > 0) && compared <= 2 * len,
          "manifest: %s: read %zu times, %zu bytes compared of %zu", what, readings, compared, len);
    free(text);
}

/* A manifest that declares a long prefix for the URI's namespace, then its letters again and the local name after each
 * of many ':': one reading compared up to the whole prefix against the text at each of them, so the reading cost the
 * prefix's length for each. */
static void long_prefix(void)
{
    static const char decl[] = ": <urn:example:plugins#> .\n", name[] = ":gain";
    const size_t p = 4096, n = 1024;
    char *text = malloc(sizeof("@prefix ") + sizeof(decl) + 2 * p + n * (sizeof(name) - 1));
    size_t len = 0;

    put(text, &len, "@prefix ", 1);
    put(text, &len, "a", p);
    put(text, &len, decl, 1);
    put(text, &len, "a", p - 1);
    put(text, &len, name, n);
    counted("a long prefix and prefixed names that are not it", text, URI, 1, 1);
}

/* A URI whose local name repeats `unit`, and a manifest of it after the declaration of its namespace, then the name
 * once under another prefix: the local name was looked for again one byte after each place it was found, so one reading
 * compared the local name's length at each. Each ":a:a..." found is turned down, and so is each after it in the run;
 * each ":::..." is a name. */
static void repeated_local(const char *unit)
{
    enum { LOCAL = 64, N = 16384 };
    static const char ns[] = "urn:example:plugins#", decl[] = "@prefix ex: <urn:example:plugins#> .\n";
    static const char other[] = "\nzx:";
    const size_t k = strlen(unit);
    char uri[sizeof(ns) + LOCAL], what[64], *text = malloc(sizeof(decl) + N + sizeof(other) + LOCAL);
    size_t ulen = sizeof(ns) - 1, len = 0;

    memcpy(uri, ns, ulen);
    put(uri, &ulen, unit, LOCAL / k);
    put(text, &len, decl, 1);
    put(text, &len, unit, N / k);
    put(text, &len, other, 1);
    put(text, &len, unit, LOCAL / k);
    snprintf(what, sizeof(what), "a local name of \"%s\" again and again, and a run of it", unit);
    counted(what, text, uri, 1, 1);
}

/* The URI's namespace declared LV2_LOCATE_PREFIXES_MAX times, then ':' to the end: no ':' is followed by the local
 * name, so there is no name to compare, and the reader before compared none. A reader that compares at every ':' of
 * each reading compares the text's length once per declaration. */
static void colon_run(void)
{
    static const char line[] = "@prefix p: <urn:example:plugins#> .\n";
    const size_t n = 65536;
    char *text = malloc(LV2_LOCATE_PREFIXES_MAX * (sizeof(line) - 1) + n + 1);
    size_t len = 0;

    put(text, &len, line, LV2_LOCATE_PREFIXES_MAX);
    put(text, &len, ":", n);
    counted("the URI's namespace declared as often as a manifest is read for, then ':' alone", text, URI,
            LV2_LOCATE_PREFIXES_MAX, 0);
}

/* A URI whose local name is long, "a:" again and again and then "b", and a manifest that declares its namespace and
 * holds the name once, not after a ':': the work is linear in the local name too. A reader that compares the local
 * name with itself at each of its ':' compares about a quarter of its length squared. */
static void long_local(void)
{
    enum { LOCAL = 65536 };
    static const char ns[] = "urn:example:plugins#", decl[] = "@prefix ex: <urn:example:plugins#> .\n ";
    static char uri[sizeof(ns) + LOCAL];
    char *text = malloc(sizeof(decl) + LOCAL);
    size_t ulen = sizeof(ns) - 1, len = 0;

    memcpy(uri, ns, ulen);
    put(uri, &ulen, "a:", LOCAL / 2 - 1);
    put(uri, &ulen, "ab", 1);
    put(text, &len, decl, 1);
    put(text, &len, uri + sizeof(ns) - 1, 1);
    counted("a long local name that repeats \"a:\", held once", text, uri, 1, 1);
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

/* lv2_locate_manifest_names's one allocation (src/lv2_locate.c's "whole") failing, as when memory is out, is a read
 * failure, not the answer that the manifest does not name the URI: before the fix, it returned 0, the same it gives
 * a manifest that truly does not name the URI, which turned an out-of-memory into a wrong negative answer. */
static void alloc_fail_names(void)
{
    const size_t uri_len = strlen(URI);
    int got;

    g_failed = 0;
    g_fail_size = uri_len + 3;
    got = lv2_locate_manifest_names("<" URI "> a lv2:Plugin .", URI);
    CHECK(g_failed == 1, "alloc fail: the allocation armed for was the one that failed");
    CHECK(got == LV2_LOCATE_READ_FAILED,
          "alloc fail: the manifest's allocation failing answers LV2_LOCATE_READ_FAILED, not 0 or 1 (got %d)", got);
    g_fail_size = 0;
}

/* The same allocation failing inside a walk (src/lv2_locate.c's walk_dir): the walk must answer LV2_LOCATE_READ_FAILED
 * too, and never call back with the bundle whose manifest could not be read, which would be the answer that it does
 * not hold the URI. */
static void alloc_fail_walk(void)
{
    char root[] = "/tmp/lv2-locate-alloc-fail.XXXXXX";
    struct seen s;
    int ret;

    if (!mkdtemp(root))
    {
        CHECK(0, "alloc fail: a temporary directory");
        return;
    }
    bundle(root, "a.lv2", "<" URI "> a lv2:Plugin .");

    memset(&s, 0, sizeof(s));
    g_failed = 0;
    g_fail_size = strlen(URI) + 3;
    ret = lv2_locate(root, URI, collect, &s);
    CHECK(g_failed == 1, "alloc fail: walk: the allocation armed for was the one that failed");
    CHECK(ret == LV2_LOCATE_READ_FAILED && s.calls == 0,
          "alloc fail: walk: a manifest's allocation failing answers LV2_LOCATE_READ_FAILED and calls back for no "
          "bundle, not the answer that none holds the URI (got %d, %d calls)", ret, s.calls);
    g_fail_size = 0;

    {
        char cmd[64];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
        if (system(cmd) != 0)
            fprintf(stderr, "could not remove %s\n", root);
    }
}

int main(void)
{
    names();
    many_prefixes();
    long_prefix();
    repeated_local(":a");
    repeated_local(":");
    colon_run();
    long_local();
    walk();
    alloc_fail_names();
    alloc_fail_walk();
    if (g_failures)
    {
        printf("lv2 locate test FAILED (%d)\n", g_failures);
        return 1;
    }
    printf("lv2 locate test ok\n");
    return 0;
}
