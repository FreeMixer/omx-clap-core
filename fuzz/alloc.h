/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */

/* Forced on every reader object a harness links (`-include fuzz/alloc.h`, never on the harness itself): the reader's
 * malloc, calloc, realloc, strdup and free go to fuzz/alloc.c, which counts the bytes the reader holds. The system
 * headers come first, so what they declare keeps its name. A block libc allocates on the reader's behalf (scandir's
 * names) would reach fuzz_free unknown; no harness drives a reader down such a path. */

#ifndef OMX_FUZZ_ALLOC_H
#define OMX_FUZZ_ALLOC_H

#include <stdlib.h>
#include <string.h>

void *fuzz_malloc(size_t n);
void *fuzz_calloc(size_t k, size_t n);
void *fuzz_realloc(void *p, size_t n);
char *fuzz_strdup(const char *s);
void fuzz_free(void *p);

#define malloc fuzz_malloc
#define calloc fuzz_calloc
#define realloc fuzz_realloc
#define strdup fuzz_strdup
#define free fuzz_free

#endif /* OMX_FUZZ_ALLOC_H */
