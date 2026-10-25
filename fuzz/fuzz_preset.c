/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */

/* fuzz_preset: the preset file reader, pj_parse of src/preset_file.c, over the bytes of a file as pj_read_file hands
 * them to it, and the writer, pj_write, over what it accepts. pj_parse is the one JSON reader of the tree.
 *
 * Input: the file, whole. A file larger than PRESET_FILE_MAX_BYTES is not read, as pj_read_file reads none.
 *
 * Properties, each in a check_ function of its own, `if (!cond) abort();`:
 *   no_overread    the tree is the one the file's own bytes give: the input parsed with a digit after it and with a NUL
 *                  after it is the same answer and the same tree. The parse that follows reads the input from a buffer
 *                  of exactly its size, so a read past it is AddressSanitizer's to report.
 *   reject_clean   a refused file leaves the tree as pj_parse found it, every byte zero, and gives back every byte the
 *                  reader took on the way to refusing it. So does the file read again with one of its allocations
 *                  failing (which one, the input's bytes choose), which is refused; and a write with one of the
 *                  writer's failing gives no text and gives its buffer back.
 *   round_trip     an accepted file written back, compact and pretty, parses to the same tree (types, member names in
 *                  order, every number's text and value, every string) and writes the same text again; a rewrite that
 *                  sets one member, as a save sets modifiedAt, keeps every other member and its place.
 *   bounded_alloc  for a file of n bytes the reader holds at most PJ_ALLOC_BOUND(n) bytes at once.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz.h"
#include "preset_file.h"

/* An item costs its container at most two slots (the slots double), and the first item eight; each slot is a struct pj
 * and, in an object, a key pointer. A container that is closed and followed by another item takes at least four bytes
 * of file; arrays nested deep around one digit, repeated, cost about 220 bytes of tree for each. The containers still
 * open take one byte each, and there are PRESET_JSON_DEPTH_MAX of them at most: their first eight slots are the
 * constant. */
#define PJ_SLOT (sizeof(struct pj) + sizeof(char *))
#define PJ_ALLOC_BOUND(n) (256u * (size_t)(n) + PRESET_JSON_DEPTH_MAX * 8u * PJ_SLOT + 4096u)

/* the member a save rewrites */
#define REWRITTEN "modifiedAt"

static int same(const struct pj *a, const struct pj *b)
{
    size_t i;

    if (a->type != b->type || a->n != b->n)
        return 0;
    switch (a->type)
    {
    case PJ_NUMBER:
        return strcmp(a->str, b->str) == 0 && memcmp(&a->num, &b->num, sizeof(a->num)) == 0;
    case PJ_STRING:
        return strcmp(a->str, b->str) == 0;
    case PJ_ARRAY:
    case PJ_OBJECT:
        for (i = 0; i < a->n; i++)
            if ((a->type == PJ_OBJECT && strcmp(a->keys[i], b->keys[i]) != 0) || !same(&a->kids[i], &b->kids[i]))
                return 0;
        return 1;
    default:
        return 1;
    }
}

static __attribute__((noinline)) void check_no_overread(const uint8_t *data, size_t size)
{
    char *digit = fuzz_copy(data, size, 0, FUZZ_PAD, '1'), *nul = fuzz_copy(data, size, 0, FUZZ_PAD, '\0');
    struct pj a, b;
    const int ra = pj_parse(digit, size, &a), rb = pj_parse(nul, size, &b);

    if (ra != rb)
        abort();
    if (ra == 0 && !same(&a, &b))
        abort();
    pj_free(&a);
    pj_free(&b);
    free(digit);
    free(nul);
}

static __attribute__((noinline)) void check_bounded_alloc(size_t held, size_t size)
{
    if (held > PJ_ALLOC_BOUND(size))
        abort();
}

static __attribute__((noinline)) void check_reject_clean(int rc, const struct pj *doc, size_t live_before)
{
    static const struct pj empty;

    if (rc == 0)
        abort();
    if (memcmp(doc, &empty, sizeof(empty)) != 0)
        abort();
    if (fuzz_alloc_live() != live_before)
        abort();
}

/* The file again with one of the allocations its reading took failing, as when memory runs out: refused like any
 * refusal. And a tree written with one of the writer's failing: no text, and its buffer given back. */
static __attribute__((noinline)) void check_reject_clean_starved(const uint8_t *data, size_t size, size_t took,
                                                                 const struct pj *doc)
{
    struct pj starved;
    size_t live = fuzz_alloc_live(), before;
    char *text;
    int rc;

    if (took)
    {
        fuzz_alloc_fail(fuzz_pick(data, size, took));
        rc = pj_parse((const char *)data, size, &starved);
        fuzz_alloc_fail(SIZE_MAX);
        check_reject_clean(rc, &starved, live);
    }
    if (!doc)
        return;
    before = fuzz_alloc_count();
    fuzz_free(pj_write(doc, 1));
    if (fuzz_alloc_count() == before)
        return;
    fuzz_alloc_fail(fuzz_pick(data, size, fuzz_alloc_count() - before));
    text = pj_write(doc, 1);
    fuzz_alloc_fail(SIZE_MAX);
    if (text || fuzz_alloc_live() != live)
        abort();
}

/* written, read back, written again */
static void round_trip_text(const struct pj *doc, int pretty)
{
    char *text = pj_write(doc, pretty), *again;
    struct pj back;

    if (!text)
        abort();
    if (pj_parse(text, strlen(text), &back) != 0)
        abort();
    if (!same(doc, &back))
        abort();
    again = pj_write(&back, pretty);
    if (!again || strcmp(text, again) != 0)
        abort();
    fuzz_free(again);
    fuzz_free(text);
    pj_free(&back);
}

/* a save's rewrite of an object: REWRITTEN set, in place when the file has it (its first), appended when not */
static void round_trip_rewrite(const struct pj *doc)
{
    char *text = pj_write(doc, 0), *saved;
    struct pj copy, back, v;
    size_t i, at = doc->n;

    if (!text || pj_parse(text, strlen(text), &copy) != 0)
        abort();
    if (pj_string(&v, "2026-10-25T00:00:00Z") != 0 || pj_set(&copy, REWRITTEN, &v) != 0)
        abort();
    saved = pj_write(&copy, 1);
    if (!saved || pj_parse(saved, strlen(saved), &back) != 0)
        abort();
    for (i = 0; i < doc->n && at == doc->n; i++)
        if (strcmp(doc->keys[i], REWRITTEN) == 0)
            at = i;
    if (back.n != doc->n + (at == doc->n ? 1u : 0u))
        abort();
    for (i = 0; i < doc->n; i++)
        if (strcmp(back.keys[i], doc->keys[i]) != 0 || (i != at && !same(&back.kids[i], &doc->kids[i])))
            abort();
    if (strcmp(back.keys[at], REWRITTEN) != 0 || back.kids[at].type != PJ_STRING)
        abort();
    fuzz_free(saved);
    fuzz_free(text);
    pj_free(&copy);
    pj_free(&back);
}

static __attribute__((noinline)) void check_round_trip(const struct pj *doc)
{
    round_trip_text(doc, 0);
    round_trip_text(doc, 1);
    if (doc->type == PJ_OBJECT)
        round_trip_rewrite(doc);
}

/* the members docs/presets.md names, looked up as a consumer looks them up; what they say is not this reader's */
static void read_members(const struct pj *doc)
{
    static const char *const top[] = { "format", "id", "name", "carries", "createdAt", REWRITTEN };
    const struct pj *params = pj_get(doc, "params"), *state = pj_get(doc, "state");
    volatile size_t seen = 0;
    size_t i;

    for (i = 0; i < sizeof(top) / sizeof(top[0]); i++)
        seen += pj_get_str(doc, top[i]) != NULL;
    seen += pj_get_str(pj_get(doc, "plugin"), "id") != NULL;
    for (i = 0; params && params->type == PJ_ARRAY && i < params->n; i++)
        seen += pj_get_str(&params->kids[i], "symbol") != NULL && pj_get(&params->kids[i], "value") != NULL;
    seen += pj_get_str(state, "encoding") != NULL && pj_get_str(state, "bytes") != NULL;
    (void)seen;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct pj doc;
    size_t live, count;
    int rc;

    if (size > PRESET_FILE_MAX_BYTES)
        return 0;
    check_no_overread(data, size);

    live = fuzz_alloc_live();
    count = fuzz_alloc_count();
    fuzz_alloc_peak_reset();
    rc = pj_parse((const char *)data, size, &doc);
    check_bounded_alloc(fuzz_alloc_peak() - live, size);
    count = fuzz_alloc_count() - count;
    if (rc != 0)
    {
        check_reject_clean(rc, &doc, live);
        check_reject_clean_starved(data, size, count, NULL);
        return 0;
    }
    read_members(&doc);
    check_round_trip(&doc);
    check_reject_clean_starved(data, size, count, &doc);
    pj_free(&doc);
    return 0;
}
