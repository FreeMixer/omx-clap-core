/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */

/* fuzz_state: the plugin state body of a preset file, preset_base64_decode of src/preset_file.c over the text of
 * state.bytes, and the body on its way back to a file: preset_base64_encode, and the body through a whole preset
 * document (pj_write, pj_parse, pj_get_str). The host parses no other state body: one saved to a state file is handed to
 * the plugin as its bytes (clap_state_load in src/effects.c, through the core's bounded istream), and the LV2 adapter's
 * body is Turtle that lilv reads; the plugin and lilv are not the target here.
 *
 * Input: the text, up to its first NUL; a string the preset reader accepts holds none. A text larger than
 * PRESET_FILE_MAX_BYTES is no member of a file the reader reads.
 *
 * Properties, each in a check_ function of its own, `if (!cond) abort();`:
 *   no_overread    the body is the one the text gives: the text with a character no base64 has after its NUL ('*',
 *                  which a decoder that reads it refuses) and with a NUL there decodes the same, or is refused both
 *                  times. The decode that follows reads a copy of exactly the text and its NUL, so a read past it is
 *                  AddressSanitizer's to report.
 *   reject_clean   a refused text gives back every byte the decoder took and leaves the length it was handed unwritten;
 *                  so does any text decoded with its allocation failing, which is refused, and a body encoded with the
 *                  encoder's failing gives no text.
 *   round_trip     a body encoded is (len + 2) / 3 * 4 characters and decodes to the same bytes, and encodes to the same
 *                  text again; set as state.bytes of a preset written and read back, it decodes to the same bytes.
 *   bounded_alloc  for a text of n characters the decoder holds at most n / 4 * 3 + 3 bytes.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz.h"
#include "preset_file.h"

#define B64_ALLOC_BOUND(n) ((size_t)(n) / 4u * 3u + 3u)

static int same_body(const uint8_t *a, size_t an, const uint8_t *b, size_t bn)
{
    return an == bn && (an == 0 || memcmp(a, b, an) == 0);
}

static __attribute__((noinline)) void check_no_overread(const uint8_t *data, size_t n)
{
    char *star = fuzz_copy(data, n, 1, FUZZ_PAD, '*'), *nul = fuzz_copy(data, n, 1, FUZZ_PAD, '\0');
    size_t la = 0, lb = 0;
    uint8_t *a = preset_base64_decode(star, &la), *b = preset_base64_decode(nul, &lb);

    if (!a != !b)
        abort();
    if (a && !same_body(a, la, b, lb))
        abort();
    fuzz_free(a);
    fuzz_free(b);
    free(star);
    free(nul);
}

static __attribute__((noinline)) void check_bounded_alloc(size_t held, size_t n)
{
    if (held > B64_ALLOC_BOUND(n))
        abort();
}

static __attribute__((noinline)) void check_reject_clean(const uint8_t *body, size_t len, size_t live_before)
{
    if (body)
        abort();
    if (len != SIZE_MAX)
        abort();
    if (fuzz_alloc_live() != live_before)
        abort();
}

/* the text decoded again with its allocation failing, as when memory runs out: refused like any refusal; and the body
 * encoded with the encoder's failing: no text, nothing held */
static __attribute__((noinline)) void check_reject_clean_starved(const char *text, const uint8_t *body, size_t len)
{
    const size_t live = fuzz_alloc_live();
    size_t n = SIZE_MAX;
    uint8_t *starved;
    char *encoded;

    fuzz_alloc_fail(0);
    starved = preset_base64_decode(text, &n);
    fuzz_alloc_fail(SIZE_MAX);
    check_reject_clean(starved, n, live);
    if (!body)
        return;
    fuzz_alloc_fail(0);
    encoded = preset_base64_encode(body, len);
    fuzz_alloc_fail(SIZE_MAX);
    if (encoded || fuzz_alloc_live() != live)
        abort();
}

/* the body as state.bytes of a preset document, written and read back */
static void round_trip_document(const char *text, const uint8_t *body, size_t len)
{
    struct pj doc, state, v, back;
    const char *bytes;
    char *file;
    uint8_t *got;
    size_t n = 0;

    pj_object(&doc);
    pj_object(&state);
    if (pj_string(&v, PRESET_FORMAT_STATE) != 0 || pj_set(&doc, "format", &v) != 0
        || pj_string(&v, PRESET_CARRIES_STATE) != 0 || pj_set(&doc, "carries", &v) != 0
        || pj_string(&v, PRESET_ENCODING_BASE64) != 0 || pj_set(&state, "encoding", &v) != 0
        || pj_string(&v, PRESET_CONTEXT_PRESET) != 0 || pj_set(&state, "context", &v) != 0
        || pj_string(&v, text) != 0 || pj_set(&state, "bytes", &v) != 0 || pj_set(&doc, "state", &state) != 0)
        abort();
    file = pj_write(&doc, 1);
    if (!file || pj_parse(file, strlen(file), &back) != 0)
        abort();
    bytes = pj_get_str(pj_get(&back, "state"), "bytes");
    if (!bytes || !(got = preset_base64_decode(bytes, &n)) || !same_body(got, n, body, len))
        abort();
    fuzz_free(got);
    fuzz_free(file);
    pj_free(&back);
    pj_free(&doc);
}

static __attribute__((noinline)) void check_round_trip(const uint8_t *body, size_t len)
{
    char *text = preset_base64_encode(body, len), *again;
    uint8_t *back;
    size_t n = 0;

    if (!text || strlen(text) != (len + 2) / 3 * 4)
        abort();
    back = preset_base64_decode(text, &n);
    if (!back || !same_body(back, n, body, len))
        abort();
    again = preset_base64_encode(back, n);
    if (!again || strcmp(again, text) != 0)
        abort();
    round_trip_document(text, body, len);
    fuzz_free(again);
    fuzz_free(back);
    fuzz_free(text);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const size_t n = fuzz_strnlen(data, size);
    char *text;
    uint8_t *body;
    size_t len = SIZE_MAX, live;

    if (n > PRESET_FILE_MAX_BYTES)
        return 0;
    check_no_overread(data, n);

    text = fuzz_copy(data, n, 1, 0, 0);
    live = fuzz_alloc_live();
    fuzz_alloc_peak_reset();
    body = preset_base64_decode(text, &len);
    check_bounded_alloc(fuzz_alloc_peak() - live, n);
    if (!body)
        check_reject_clean(body, len, live);
    else
        check_round_trip(body, len);
    check_reject_clean_starved(text, body, len);
    fuzz_free(body);
    free(text);
    return 0;
}
