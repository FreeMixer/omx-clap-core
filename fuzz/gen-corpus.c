/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */

/* Writes the seed corpus of every harness in fuzz/ (`make fuzz-corpus`), from what the tree already holds: the preset
 * example of docs/presets.md, the JSON documents of data/, the Turtle of the LV2 fixture bundles
 * (lv2/tests/fixtures/), the RFC 4648 vectors, the spellings tests/lv2_locate_test.c reads, and what the reader's own
 * writer and encoder (src/preset_file.c) make of them. Beside what a reader accepts it writes one input per refusal the
 * reader makes, so the corpus reaches the refusals as well. Nothing is fetched, directories are read in name order, and
 * the corpus is the same bytes on any machine.
 *
 * Each seed is shaped as its harness reads its input; the header comment of the harness is the grammar.
 * Usage (from the top of the tree): fuzz-gen-corpus <corpus dir> */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "lv2_locate.h"
#include "preset_file.h"

#define FIXTURES "lv2/tests/fixtures"

static const char *g_root;

static void die(const char *what)
{
    fprintf(stderr, "fuzz-gen-corpus: %s: %s\n", what, strerror(errno));
    exit(1);
}

static void seed(const char *harness, const char *name, const void *p, size_t n)
{
    char path[4096];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s/%s", g_root, harness, name);
    if (!(f = fopen(path, "wb")) || (n && fwrite(p, 1, n, f) != n) || fclose(f) != 0)
        die(path);
}

static void seed_str(const char *harness, const char *name, const char *s)
{
    seed(harness, name, s, strlen(s));
}

/* a file of the tree, whole and NUL-terminated */
static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *b = NULL;
    size_t n = 0, cap = 0, got;

    if (!f)
        die(path);
    do
    {
        if (n + 4096 + 1 > cap && !(b = realloc(b, cap = (n + 4096 + 1) * 2)))
            die("realloc");
        got = fread(b + n, 1, 4096, f);
        n += got;
    } while (got == 4096);
    if (ferror(f))
        die(path);
    fclose(f);
    b[n] = '\0';
    if (len)
        *len = n;
    return b;
}

/* the names in a directory that end in `suffix` (or that are directories, for "/"), in name order */
static int list(const char *dir, const char *suffix, char ***out)
{
    struct dirent **e;
    int n = scandir(dir, &e, NULL, alphasort), i, k = 0;

    if (n < 0)
        die(dir);
    *out = calloc((size_t)n + 1, sizeof(**out));
    for (i = 0; i < n; i++)
    {
        const size_t l = strlen(e[i]->d_name), s = strlen(suffix);

        if (e[i]->d_name[0] != '.' && l > s && strcmp(e[i]->d_name + l - s, suffix) == 0)
            (*out)[k++] = strdup(e[i]->d_name);
        free(e[i]);
    }
    free(e);
    return k;
}

/* ---------------------------------------------------------------- preset */

static void put_str(struct pj *obj, const char *key, const char *s)
{
    struct pj v;

    if (pj_string(&v, s) != 0 || pj_set(obj, key, &v) != 0)
        die("pj_set");
}

static void put_num(struct pj *obj, const char *key, double d)
{
    struct pj v;

    if (pj_number(&v, d) != 0 || pj_set(obj, key, &v) != 0)
        die("pj_set");
}

static void seed_tree(const char *name, const struct pj *doc, int pretty)
{
    char *text = pj_write(doc, pretty);

    if (!text)
        die("pj_write");
    seed_str("preset", name, text);
    free(text);
}

/* a preset as docs/presets.md describes it, written by the reader's own writer */
static void built_presets(void)
{
    static const struct { const char *symbol, *unit; double value; } params[] = {
        { "gain", "dB", -6.0 }, { "mix", "", 0.25 }, { "time", "ms", 0.1 }, { "3", "", 1e-3 },
    };
    struct pj doc, plugin, rows, state, row;
    char *ttl, *body;
    size_t i, len;

    pj_object(&doc);
    put_str(&doc, "format", PRESET_FORMAT_PARAMS);
    pj_object(&plugin);
    put_str(&plugin, "id", "org.openmixer.omx-delay");
    put_str(&plugin, "version", "1.2.0");
    put_str(&plugin, "fingerprint", "omx-layout/1:0f1e2d3c4b5a69788796a5b4c3d2e1f00f1e2d3c4b5a69788796a5b4c3d2e1f0");
    if (pj_set(&doc, "plugin", &plugin) != 0)
        die("pj_set");
    put_str(&doc, "id", "org.openmixer.omx-delay@2026-10-22T21:00:00Z");
    put_str(&doc, "name", "Slap back");
    put_str(&doc, "createdAt", "2026-10-22T21:00:00Z");
    put_str(&doc, "modifiedAt", "2026-10-22T21:00:00Z");
    put_str(&doc, "carries", PRESET_CARRIES_PARAMS);
    pj_array(&rows);
    for (i = 0; i < sizeof(params) / sizeof(params[0]); i++)
    {
        pj_object(&row);
        put_str(&row, "symbol", params[i].symbol);
        put_str(&row, "unit", params[i].unit);
        put_num(&row, "value", params[i].value);
        if (pj_push(&rows, &row) != 0)
            die("pj_push");
    }
    if (pj_set(&doc, "params", &rows) != 0)
        die("pj_set");
    put_str(&doc, "alias", "Slap");
    seed_tree("built-params-v1", &doc, 1);

    // the same as /2 with a state body: an LV2 plugin's state through the adapter is Turtle
    put_str(&doc, "format", PRESET_FORMAT_STATE);
    put_str(&doc, "carries", PRESET_CARRIES_PARAMS_STATE);
    ttl = slurp(FIXTURES "/omx-host-fixture.lv2/omx-host-fixture-presets.ttl", &len);
    if (!(body = preset_base64_encode((const uint8_t *)ttl, len)))
        die("preset_base64_encode");
    pj_object(&state);
    put_str(&state, "encoding", PRESET_ENCODING_BASE64);
    put_str(&state, "context", PRESET_CONTEXT_LV2);
    put_str(&state, "bytes", body);
    if (pj_set(&doc, "state", &state) != 0)
        die("pj_set");
    seed_tree("built-state-v2", &doc, 1);
    seed_tree("built-state-v2-compact", &doc, 0);
    free(body);
    free(ttl);
    pj_free(&doc);
}

/* the indented example of docs/presets.md, the first block that opens with "    {" */
static void docs_example(void)
{
    char *md = slurp("docs/presets.md", NULL), *start = strstr(md, "\n    {\n"), *end;

    if (!start || !(end = strstr(start, "\n    }\n")))
    {
        fprintf(stderr, "fuzz-gen-corpus: docs/presets.md has no example block\n");
        exit(1);
    }
    start++;
    end += 6;
    seed("preset", "docs-presets-example", start, (size_t)(end - start + 1));
    // and cut halfway through: a file that ends inside a value
    seed("preset", "refuse-docs-example-cut", start, (size_t)(end - start) / 2);
    free(md);
}

static void data_documents(void)
{
    char **names, *text, name[512];
    size_t len;
    int n, i;

    text = slurp("data/hosting.json", &len);
    seed("preset", "data-hosting", text, len);
    free(text);
    n = list("data/hosting", ".json", &names);
    for (i = 0; i < n; i++)
    {
        char path[512];

        snprintf(path, sizeof(path), "data/hosting/%s", names[i]);
        snprintf(name, sizeof(name), "data-hosting-%.*s", (int)(strlen(names[i]) - 5), names[i]);
        text = slurp(path, &len);
        seed("preset", name, text, len);
        free(text);
        free(names[i]);
    }
    free(names);
}

static void preset_corpus(void)
{
    static const struct { const char *name, *text; } cases[] = {
        // what the reader accepts
        { "escapes", "{\"name\": \"quote \\\" backslash \\\\ slash \\/ \\b\\f\\n\\r\\t e\\u0301 \\u00e9 \\ud83c\\udfb9 "
                     "\\u001f \\u007f \xc3\xa9\", \"alias\": \"tab\\there\"}\n" },
        { "numbers", "[0, -0, 1.5, -2.5e-3, 1E10, 0.1, 123456789012345678, 1e-400, 2.2250738585072014e-308, 17]" },
        { "escapes-upper-hex", "{\"name\": \"\\u00C9t\\u00E9 \\u20AC \\u20ac \\uD83C\\uDFB9\"}" },
        { "number-alone", "17" },
        { "zero-alone", "0" },
        { "minus-zero-alone", "-0" },
        { "lax-trailing-dot", "[1., 2.e3]" },
        { "literals", "[true, false, null, {}, [], \"\", {\"\": \"\"}]" },
        { "string-alone", "\"org.openmixer.preset/2\"" },
        { "duplicate-keys", "{\"modifiedAt\": \"a\", \"x\": 1, \"modifiedAt\": \"b\"}" },
        { "no-modified-at", "{\"format\": \"org.openmixer.preset/1\", \"tags\": [\"a\", \"b\"]}" },
        { "whitespace", " \t\r\n{ \"a\" : [ 1 , 2 ] }\r\n\t " },
        { "many-small-containers", "[[1],[2],[3],[4],[5],[6],[7],[8],[9],{\"a\":1},{\"b\":[2]},[[3]]]" },
        // what it refuses, one input each
        { "refuse-empty", "" },
        { "refuse-blank", " \n\t" },
        { "refuse-trailing", "{} x" },
        { "refuse-two-values", "1 2" },
        { "refuse-unterminated-string", "\"abc" },
        { "refuse-escape-at-end", "\"\\" },
        { "refuse-escape", "\"\\x\"" },
        { "refuse-short-u", "\"\\u12\"" },
        { "refuse-u-not-hex", "\"\\u12g4\"" },
        { "refuse-u0000", "\"\\u0000\"" },
        { "refuse-low-surrogate", "\"\\udc00\"" },
        { "refuse-high-surrogate-alone", "\"\\ud83c\"" },
        { "refuse-high-then-not-low", "\"\\ud83c\\u0041\"" },
        { "refuse-control", "\"a\tb\"" },
        { "refuse-leading-zero", "01" },
        { "refuse-minus", "-" },
        { "refuse-plus", "+1" },
        { "refuse-dot", ".5" },
        { "refuse-infinite", "1e999" },
        { "refuse-exponent", "1e" },
        { "refuse-two-dots", "1.2.3" },
        { "refuse-true", "tru" },
        { "refuse-false", "fals" },
        { "refuse-null", "nul" },
        { "refuse-word", "nan" },
        { "refuse-colon", "{\"a\" 1}" },
        { "refuse-comma", "[1 2]" },
        { "refuse-trailing-comma", "[1,]" },
        { "refuse-key", "{1: 2}" },
        { "refuse-key-cut", "{\"a" },
        { "refuse-key-alone", "{\"a\"" },
        { "refuse-value-cut", "{\"a\":" },
        { "refuse-member-cut", "{\"format\": \"org.openmixer.preset/2\", \"params\": [{\"symbol\": \"gain\"}]," },
        { "refuse-array-cut", "[1, [2, \"three\"" },
        { "refuse-close", "[1}" },
        { "refuse-bad-item", "[1, x]" },
    };
    char deep[2 * PRESET_JSON_DEPTH_MAX + 8];
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        seed_str("preset", cases[i].name, cases[i].text);
    // as deep as a file may nest, and one deeper
    memset(deep, '[', PRESET_JSON_DEPTH_MAX);
    deep[PRESET_JSON_DEPTH_MAX] = '1';
    memset(deep + PRESET_JSON_DEPTH_MAX + 1, ']', PRESET_JSON_DEPTH_MAX);
    seed("preset", "deepest", deep, 2 * PRESET_JSON_DEPTH_MAX + 1);
    memset(deep, '[', PRESET_JSON_DEPTH_MAX + 1);
    deep[PRESET_JSON_DEPTH_MAX + 1] = '1';
    memset(deep + PRESET_JSON_DEPTH_MAX + 2, ']', PRESET_JSON_DEPTH_MAX + 1);
    seed("preset", "refuse-too-deep", deep, 2 * PRESET_JSON_DEPTH_MAX + 3);
    // and never closed: every level open at once, the most the reader holds for the fewest bytes
    seed("preset", "refuse-too-deep-open", deep, PRESET_JSON_DEPTH_MAX + 2);
    docs_example();
    data_documents();
    built_presets();
}

/* ---------------------------------------------------------------- state */

static void seed_body(const char *name, const uint8_t *p, size_t n, int wrap)
{
    char *text = preset_base64_encode(p, n);
    size_t i, len;

    if (!text)
        die("preset_base64_encode");
    if (!wrap)
    {
        seed_str("state", name, text);
        free(text);
        return;
    }
    // as a person or another tool may write it: 76 characters a line, CRLF
    {
        char *w = malloc(strlen(text) / 76 * 2 + strlen(text) + 3), *o = w;

        if (!w)
            die("malloc");
        len = strlen(text);
        for (i = 0; i < len; i++)
        {
            if (i && i % 76 == 0)
            {
                *o++ = '\r';
                *o++ = '\n';
            }
            *o++ = text[i];
        }
        *o = '\0';
        seed_str("state", name, w);
        free(w);
    }
    free(text);
}

static void state_corpus(void)
{
    static const struct { const char *name, *text; } cases[] = {
        { "rfc4648-empty", "" },
        { "rfc4648-f", "Zg==" },
        { "rfc4648-fo", "Zm8=" },
        { "rfc4648-foo", "Zm9v" },
        { "rfc4648-foob", "Zm9vYg==" },
        { "rfc4648-fooba", "Zm9vYmE=" },
        { "rfc4648-foobar", "Zm9vYmFy" },
        { "unpadded-fo", "Zm8" },
        { "unpadded-f", "Zg" },
        { "spaced", " Zm9v\tYmFy" },
        { "trailing-newline", "Zm9vYmFy\n" },
        { "rfc4648-f-newline", "Zg==\n" },
        { "refuse-letter", "Zm9v!" },
        { "refuse-after-pad", "Zg==Zg==" },
        { "refuse-three-pads", "Zg===" },
        { "refuse-bits", "Zh==" },
        { "refuse-one", "Z" },
        { "refuse-five", "Zm9vY" },
    };
    char **bundles, **files, path[1024], name[1024];
    uint8_t all[256];
    size_t i, len;
    int nb, nf, b, f;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        seed_str("state", cases[i].name, cases[i].text);
    // a text whose NUL is not its end: the reader is handed the C string, what follows is not it
    seed("state", "nul-inside", "Zm9v\0Zg==", 9);
    for (i = 0; i < 256; i++)
        all[i] = (uint8_t)i;
    seed_body("bytes-0-255", all, sizeof(all), 0);
    seed_body("bytes-0-255-wrapped", all, sizeof(all), 1);
    // every Turtle file of the fixture bundles, as the state an LV2 plugin's body is
    nb = list(FIXTURES, ".lv2", &bundles);
    for (b = 0; b < nb; b++)
    {
        snprintf(path, sizeof(path), FIXTURES "/%s", bundles[b]);
        nf = list(path, ".ttl", &files);
        for (f = 0; f < nf; f++)
        {
            char *ttl;

            snprintf(path, sizeof(path), FIXTURES "/%s/%s", bundles[b], files[f]);
            snprintf(name, sizeof(name), "ttl-%.*s-%.*s", (int)(strlen(bundles[b]) - 4), bundles[b],
                     (int)(strlen(files[f]) - 4), files[f]);
            ttl = slurp(path, &len);
            seed_body(name, (const uint8_t *)ttl, len, b == 0 && f == 0);
            free(ttl);
            free(files[f]);
        }
        free(files);
        free(bundles[b]);
    }
    free(bundles);
}

/* ---------------------------------------------------------------- manifest */

static void seed_names(const char *name, const char *uri, const char *text)
{
    const size_t un = strlen(uri), tn = strlen(text);
    uint8_t *b = malloc(1 + un + tn);

    if (!b || un > 255)
        die(name);
    b[0] = (uint8_t)un;
    memcpy(b + 1, uri, un);
    memcpy(b + 1 + un, text, tn);
    seed("manifest", name, b, 1 + un + tn);
    free(b);
}

/* the first absolute IRI between angle brackets: the first plugin a fixture manifest lists */
static char *first_iri(const char *text)
{
    const char *p, *e;

    for (p = text; (p = strchr(p, '<')) != NULL; p = e)
    {
        if (!(e = strchr(p, '>')))
            break;
        if (memchr(p, ':', (size_t)(e - p)) && !memchr(p, ' ', (size_t)(e - p)) && strncmp(p + 1, "http", 4) != 0)
            return strndup(p + 1, (size_t)(e - p - 1));
    }
    return NULL;
}

static void manifest_corpus(void)
{
    // the spellings of tests/lv2_locate_test.c, and one input per way the reader turns a declaration down
    static const struct { const char *name, *text; } cases[] = {
        { "whole", "<urn:example:plugins#gain> a lv2:Plugin ." },
        { "prefixed", "@prefix ex: <urn:example:plugins#> .\nex:gain a lv2:Plugin ." },
        { "sparql", "PREFIX ex: <urn:example:plugins#>\nex:gain a lv2:Plugin ." },
        { "sparql-lower", "prefix ex: <urn:example:plugins#>\nex:gain a lv2:Plugin ." },
        { "empty-prefix", "@prefix : <urn:example:plugins#> .\n:gain a lv2:Plugin ." },
        { "ends-statement", "@prefix ex: <urn:example:plugins#> .\nex:gain." },
        { "name-at-end", "@prefix ex: <urn:example:plugins#> .\nex:gain" },
        { "longer-local", "@prefix ex: <urn:example:plugins#> .\nex:gain2 a lv2:Plugin ." },
        { "longer-prefix", "@prefix ex: <urn:example:plugins#> .\nxex:gain a lv2:Plugin ." },
        { "dotted-prefix", "@prefix ex: <urn:example:plugins#> .\n.ex:gain a lv2:Plugin ." },
        { "other-namespace", "@prefix ex: <urn:example:other#> .\nex:gain a lv2:Plugin ." },
        { "uri-prefix-of-other", "<urn:example:plugins#gain2> a lv2:Plugin ." },
        { "base-relative", "@base <urn:example:plugins> .\n<#gain> a lv2:Plugin ." },
        { "empty", "" },
        { "sparql-inside-word", "XPREFIX ex: <urn:example:plugins#>\nex:gain a lv2:Plugin ." },
        { "prefix-no-space", "@prefixex: <urn:example:plugins#> .\nex:gain a lv2:Plugin ." },
        { "prefix-no-colon", "@prefix ex <urn:example:plugins#> .\nex:gain a lv2:Plugin ." },
        { "prefix-no-angle", "@prefix ex: urn:example:plugins# .\nex:gain a lv2:Plugin ." },
        { "prefix-unclosed", "@prefix ex: <urn:example:plugins#" },
        { "prefix-cut", "@prefix ex:" },
        { "prefix-at-end", "@prefix" },
        { "namespace-is-uri", "@prefix ex: <urn:example:plugins#gain> .\nex: a lv2:Plugin ." },
        { "local-first", "gain\n@prefix ex: <urn:example:plugins#> .\nex:gain a lv2:Plugin ." },
        { "two-prefixes","@prefix a: <urn:example:other#> .\n@prefix ex: <urn:example:plugins#> .\nex:gain a lv2:Plugin ." },
    };
    char **bundles, path[1024], name[1024];
    size_t i;
    int nb, b;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        seed_names(cases[i].name, "urn:example:plugins#gain", cases[i].text);
    seed_names("empty-uri", "", "<urn:a:b> a lv2:Plugin .");
    // a URI whose local name a prefixed name cannot spell unescaped
    seed_names("local-dot-last", "urn:example:plugins#gain.", "@prefix ex: <urn:example:plugins#> .\nex:gain. a lv2:Plugin .");
    seed_names("local-escaped", "urn:example:plugins#ga%in", "@prefix ex: <urn:example:plugins#> .\nex:ga%in a lv2:Plugin .");
    // the URI's namespace declared as often as a manifest is read for, and once more: a candidate unread
    {
        static const char line[] = "@prefix a: <urn:example:plugins#> .\n";
        char many[(LV2_LOCATE_PREFIXES_MAX + 1) * (sizeof(line) - 1) + 1];
        int k;

        for (k = 0; k <= LV2_LOCATE_PREFIXES_MAX; k++)
            memcpy(many + k * (sizeof(line) - 1), line, sizeof(line) - 1);
        many[LV2_LOCATE_PREFIXES_MAX * (sizeof(line) - 1)] = '\0';
        seed_names("prefixes-most-read", "urn:example:plugins#gain", many);
        many[LV2_LOCATE_PREFIXES_MAX * (sizeof(line) - 1)] = line[0];
        many[(LV2_LOCATE_PREFIXES_MAX + 1) * (sizeof(line) - 1)] = '\0';
        seed_names("prefixes-past-most-read", "urn:example:plugins#gain", many);
    }
    // each fixture bundle's manifest, for its first plugin, for lv2:Plugin through its lv2: prefix, and for a URI it
    // does not name
    nb = list(FIXTURES, ".lv2", &bundles);
    for (b = 0; b < nb; b++)
    {
        char *text, *iri, *absent;

        snprintf(path, sizeof(path), FIXTURES "/%s/manifest.ttl", bundles[b]);
        text = slurp(path, NULL);
        if (!(iri = first_iri(text)))
        {
            fprintf(stderr, "fuzz-gen-corpus: %s names no plugin\n", path);
            exit(1);
        }
        snprintf(name, sizeof(name), "fixture-%.*s", (int)(strlen(bundles[b]) - 4), bundles[b]);
        seed_names(name, iri, text);
        snprintf(name, sizeof(name), "fixture-%.*s-lv2-plugin", (int)(strlen(bundles[b]) - 4), bundles[b]);
        seed_names(name, "http://lv2plug.in/ns/lv2core#Plugin", text);
        if (asprintf(&absent, "%s-absent", iri) < 0)
            die("asprintf");
        snprintf(name, sizeof(name), "fixture-%.*s-absent", (int)(strlen(bundles[b]) - 4), bundles[b]);
        seed_names(name, absent, text);
        free(absent);
        free(iri);
        free(text);
        free(bundles[b]);
    }
    free(bundles);
}

int main(int argc, char **argv)
{
    static const char *const harnesses[] = { "preset", "state", "manifest" };
    char path[4096];
    size_t i;

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <corpus dir>   (from the top of the tree)\n", argv[0]);
        return 2;
    }
    g_root = argv[1];
    if (mkdir(g_root, 0755) != 0 && errno != EEXIST)
        die(g_root);
    for (i = 0; i < sizeof(harnesses) / sizeof(harnesses[0]); i++)
    {
        snprintf(path, sizeof(path), "%s/%s", g_root, harnesses[i]);
        if (mkdir(path, 0755) != 0 && errno != EEXIST)
            die(path);
    }
    preset_corpus();
    state_corpus();
    manifest_corpus();
    return 0;
}
