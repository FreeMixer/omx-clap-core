/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */

/* fuzz_manifest: the one Turtle reader of the tree that reads the bytes itself, lv2_locate_manifest_names of
 * src/lv2_locate.c, over the text of a bundle's manifest.ttl and the plugin URI it is asked for. Every other ttl the
 * host and the LV2 adapter read goes through lilv (lv2/core/lv2_world.c), which is not the target here.
 *
 * Input: one byte k, then k bytes of URI, then the manifest; each read as C strings are, up to a first NUL. A manifest
 * larger than lv2_locate's 4 MiB is not read.
 *
 * Properties, each in a check_ function of its own, `if (!cond) abort();`:
 *   no_overread    the answer is the one the text and the URI give: both with a name character after their NUL and
 *                  with a NUL there answer the same. The call that follows reads copies of exactly each string and its
 *                  NUL, so a read past them is AddressSanitizer's to report.
 *   reject_clean   no answer, a refusal ("does not name it") above all, leaves a byte of the reader's held; asked with
 *                  its allocation failing, it holds nothing either.
 *   round_trip     the answer written back as a manifest reads back as the same answer: one that names the URI, as
 *                  "<uri>", and at every split into a namespace and a plain local name as "@prefix p: <ns> . p:local",
 *                  "PREFIX p: <ns> p:local" and "@prefix : <ns> . :local", names it; one that does not, "", does not.
 *   bounded_alloc  the reader holds at most the URI's length and three bytes.
 */

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz.h"
#include "lv2_locate.h"

/* MANIFEST_MAX_BYTES of src/lv2_locate.c: read_manifest reads no larger file */
#define MANIFEST_MAX ((size_t)4 << 20)

#define NAMES_ALLOC_BOUND(uri_len) ((size_t)(uri_len) + 3u)

static __attribute__((noinline)) void check_no_overread(const uint8_t *uri, size_t un, const uint8_t *text, size_t tn)
{
    char *ua = fuzz_copy(uri, un, 1, FUZZ_PAD, 'a'), *ta = fuzz_copy(text, tn, 1, FUZZ_PAD, 'a');
    char *ub = fuzz_copy(uri, un, 1, FUZZ_PAD, '\0'), *tb = fuzz_copy(text, tn, 1, FUZZ_PAD, '\0');

    if (lv2_locate_manifest_names(ta, ua) != lv2_locate_manifest_names(tb, ub))
        abort();
    free(ua);
    free(ta);
    free(ub);
    free(tb);
}

static __attribute__((noinline)) void check_bounded_alloc(size_t held, size_t uri_len)
{
    if (held > NAMES_ALLOC_BOUND(uri_len))
        abort();
}

static __attribute__((noinline)) void check_reject_clean(size_t live_before)
{
    if (fuzz_alloc_live() != live_before)
        abort();
}

/* asked again with the reader's allocation failing, as when memory runs out: whatever it answers, it holds nothing */
static __attribute__((noinline)) void check_reject_clean_starved(const char *text, const char *uri)
{
    const size_t live = fuzz_alloc_live();

    fuzz_alloc_fail(0);
    (void)lv2_locate_manifest_names(text, uri);
    fuzz_alloc_fail(SIZE_MAX);
    check_reject_clean(live);
}

/* what the reader's header says a prefixed name may spell unescaped: letters, digits, '_', '-', ':' and '.', not last */
static int plain(const char *s)
{
    size_t n = strlen(s), i;

    if (n == 0 || s[n - 1] == '.')
        return 0;
    for (i = 0; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && s[i] != '_' && s[i] != '-' && s[i] != '.' && s[i] != ':')
            return 0;
    return 1;
}

static void must_name(const char *text, const char *uri)
{
    if (lv2_locate_manifest_names(text, uri) != 1)
        abort();
}

static __attribute__((noinline)) void check_round_trip(const char *uri, int named)
{
    const size_t n = strlen(uri);
    char *text;
    size_t k;

    if (!named)
    {
        if (lv2_locate_manifest_names("", uri) != 0)
            abort();
        return;
    }
    if (!(text = malloc(3 * n + 64)))
        abort();
    snprintf(text, 3 * n + 64, "<%s>\n    a lv2:Plugin .\n", uri);
    must_name(text, uri);
    // a namespace is an IRI: no '>' inside it
    for (k = 1; k < n && uri[k - 1] != '>'; k++)
    {
        const char *local = uri + k;

        if (!plain(local))
            continue;
        snprintf(text, 3 * n + 64, "@prefix p: <%.*s> .\np:%s\n    a lv2:Plugin .\n", (int)k, uri, local);
        must_name(text, uri);
        snprintf(text, 3 * n + 64, "PREFIX p: <%.*s>\np:%s\n    a lv2:Plugin .\n", (int)k, uri, local);
        must_name(text, uri);
        snprintf(text, 3 * n + 64, "@prefix : <%.*s> .\n:%s\n    a lv2:Plugin .\n", (int)k, uri, local);
        must_name(text, uri);
    }
    free(text);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const size_t k = size ? (data[0] < size - 1 ? data[0] : size - 1) : 0;
    const uint8_t *uri = data + (size ? 1 : 0), *text = uri + k;
    const size_t un = fuzz_strnlen(uri, k), tn = fuzz_strnlen(text, size - (size ? 1 : 0) - k);
    char *u, *t;
    size_t live;
    int named;

    if (tn > MANIFEST_MAX)
        return 0;
    check_no_overread(uri, un, text, tn);

    u = fuzz_copy(uri, un, 1, 0, 0);
    t = fuzz_copy(text, tn, 1, 0, 0);
    live = fuzz_alloc_live();
    fuzz_alloc_peak_reset();
    named = lv2_locate_manifest_names(t, u);
    check_bounded_alloc(fuzz_alloc_peak() - live, un);
    check_reject_clean(live);
    check_reject_clean_starved(t, u);
    check_round_trip(u, named);
    free(u);
    free(t);
    return 0;
}
