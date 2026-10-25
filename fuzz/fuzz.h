/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */

/* What every harness in fuzz/ shares: the exact-size and padded copies a reader is handed, and the count of the bytes
 * a reader holds (fuzz/alloc.c). Nothing here reads a clock, the environment, a file or the network. */

#ifndef OMX_FUZZ_H
#define OMX_FUZZ_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* the bytes a padded copy carries past its input: a reader that reads one of them reads past what it was given */
#define FUZZ_PAD 16

/* A heap copy of `n` bytes; with `terminated` a NUL after them, as a reader of C strings is handed its text; then `pad`
 * bytes of `fill`, and for a C string a NUL after the pad too, so a reader that runs past its NUL stops inside the copy.
 * With pad 0 the copy ends where the reader's bytes end, so a read past them is AddressSanitizer's. */
static inline char *fuzz_copy(const uint8_t *p, size_t n, int terminated, size_t pad, char fill)
{
    const size_t t = terminated ? 1 : 0, end = pad && terminated ? 1 : 0, total = n + t + pad + end;
    char *b = malloc(total ? total : 1);

    /* not a property: the harness's own allocation failed */
    if (!b)
        abort();
    if (n)
        memcpy(b, p, n);
    if (t)
        b[n] = '\0';
    memset(b + n + t, fill, pad);
    if (end)
        b[total - 1] = '\0';
    return b;
}

/* how long the C string at the start of `n` bytes is: up to its first NUL, or all of them */
static inline size_t fuzz_strnlen(const uint8_t *p, size_t n)
{
    const uint8_t *z = memchr(p, 0, n);

    return z ? (size_t)(z - p) : n;
}

/* fuzz/alloc.c: the reader's malloc, realloc, strdup and free (forced on its objects by fuzz/alloc.h) */
void *fuzz_malloc(size_t n);
void *fuzz_calloc(size_t k, size_t n);
void *fuzz_realloc(void *p, size_t n);
char *fuzz_strdup(const char *s);
void fuzz_free(void *p);

/* the bytes the reader holds now; the most it held since the last fuzz_alloc_peak_reset() */
size_t fuzz_alloc_live(void);
size_t fuzz_alloc_peak(void);
void fuzz_alloc_peak_reset(void);

/* how many allocations the reader has asked for; fuzz_alloc_fail(k): the k-th from now (0 the next) fails, once, as
 * when memory runs out; SIZE_MAX none */
size_t fuzz_alloc_count(void);
void fuzz_alloc_fail(size_t k);

/* which of `n` allocations an input fails: one its bytes choose, so the same input fails the same one */
static inline size_t fuzz_pick(const uint8_t *p, size_t len, size_t n)
{
    size_t h = 5381, i;

    for (i = 0; i < len; i++)
        h = h * 33 + p[i];
    return n ? h % n : 0;
}

#endif /* OMX_FUZZ_H */
