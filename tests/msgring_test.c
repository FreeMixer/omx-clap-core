// SPDX-License-Identifier: GPL-3.0-or-later
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
 * msgring_test.c: closed-form test of the worker message ring (src/omx_msgring.h).
 *
 *   make test-msgring
 *
 * 1. ACCOUNTING by value: a 64-byte ring holds exactly five 8-byte records (12 bytes each), the
 *    sixth is refused whole; after one pop, the record that must wrap costs its 4-byte skip plus
 *    its 12 and fits EXACTLY (16 free), and reads back intact from offset 0.
 * 2. Refusals: a record larger than the ring, a cap that is not a power of two, a NULL buffer.
 * 3. FIFO across many wraps, single thread, sizes 0..37 — every payload byte checked.
 * 4. Two threads, 1,000,000 records of varying length through a 256-byte ring: every record
 *    arrives, in order, with its bytes intact; a refused push is retried, never lost.
 */
#include "../src/omx_msgring.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static int failures = 0;
#define CHECK(cond, ...)                                  \
  do {                                                    \
    if (!(cond)) {                                        \
      failures++;                                         \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
      fprintf(stderr, __VA_ARGS__);                       \
      fprintf(stderr, "\n");                              \
    }                                                     \
  } while (0)

static void fill(uint8_t *p, uint32_t n, uint32_t seq) {
  for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)(seq * 31u + i * 7u + 1u);
}
static int matches(const uint8_t *p, uint32_t n, uint32_t seq) {
  for (uint32_t i = 0; i < n; i++)
    if (p[i] != (uint8_t)(seq * 31u + i * 7u + 1u)) return 0;
  return 1;
}

static void test_accounting(void) {
  static uint8_t buf[64];
  struct omx_msgring r;
  CHECK(omx_msgring_init(&r, buf, 64) == 0, "init 64");
  uint8_t msg[8];
  for (uint32_t k = 0; k < 5; k++) {
    fill(msg, 8, k);
    CHECK(omx_msgring_push(&r, msg, 8) == 0, "push %u of 5 must fit", k);
  }
  CHECK(omx_msgring_used(&r) == 60, "5 x 12 bytes = 60 used, got %u", omx_msgring_used(&r));
  fill(msg, 8, 5);
  CHECK(omx_msgring_push(&r, msg, 8) == -1, "the 6th 8-byte record needs 12 of 4 free: refused");
  CHECK(omx_msgring_used(&r) == 60, "a refused push writes nothing");

  uint32_t size = 99;
  const uint8_t *p = omx_msgring_peek(&r, &size);
  CHECK(p != NULL && size == 8 && matches(p, 8, 0), "first record reads back first");
  omx_msgring_pop(&r);
  CHECK(omx_msgring_used(&r) == 48, "one pop frees 12, got %u used", omx_msgring_used(&r));
  /* head at 60: 4 contiguous bytes left, the record needs 12 -> 4 skip + 12 = 16 = free. */
  fill(msg, 8, 5);
  CHECK(omx_msgring_push(&r, msg, 8) == 0, "the wrapping record fits exactly (16 of 16)");
  CHECK(omx_msgring_used(&r) == 64, "ring exactly full after the wrap, got %u", omx_msgring_used(&r));
  CHECK(omx_msgring_push(&r, msg, 0) == -1, "even an empty record is refused on a full ring");
  for (uint32_t k = 1; k <= 5; k++) {
    p = omx_msgring_peek(&r, &size);
    CHECK(p != NULL && size == 8 && matches(p, 8, k), "record %u in order after the wrap", k);
    if (k == 5) CHECK(p == buf + OMX_MSGRING_HDR, "the wrapped record is contiguous at offset 0");
    omx_msgring_pop(&r);
  }
  CHECK(omx_msgring_used(&r) == 0 && omx_msgring_peek(&r, &size) == NULL, "drained to empty");
  omx_msgring_pop(&r); /* no-op on empty */
  CHECK(omx_msgring_used(&r) == 0, "pop on empty is a no-op");
}

static void test_refusals(void) {
  static uint8_t buf[64];
  struct omx_msgring r;
  CHECK(omx_msgring_init(&r, buf, 48) == -1, "cap 48 is not a power of two");
  CHECK(omx_msgring_init(&r, buf, 8) == -1, "cap 8 is below 16");
  CHECK(omx_msgring_init(&r, NULL, 64) == -1, "NULL buffer");
  CHECK(omx_msgring_init(&r, buf, 64) == 0, "init");
  static uint8_t big[64];
  CHECK(omx_msgring_push(&r, big, 61) == -1, "61 + 4 header > 64: refused");
  CHECK(omx_msgring_push(&r, big, 60) == 0, "60 + 4 = 64: fits an empty ring exactly");
  volatile uint32_t huge = UINT32_MAX - 2; /* opaque: the refusal is decided at run time */
  CHECK(omx_msgring_push(&r, big, huge) == -1, "a size that would overflow the arithmetic");
}

static void test_fifo_wraps(void) {
  static uint8_t buf[128];
  struct omx_msgring r;
  omx_msgring_init(&r, buf, 128);
  uint8_t msg[40];
  uint32_t wseq = 0, rseq = 0;
  for (int round = 0; round < 20000; round++) {
    uint32_t n = wseq % 38u;
    fill(msg, n, wseq);
    if (omx_msgring_push(&r, msg, n) == 0) wseq++;
    if (round % 3 != 0) {
      uint32_t size;
      const uint8_t *p = omx_msgring_peek(&r, &size);
      if (p) {
        CHECK(size == rseq % 38u && matches(p, size, rseq), "fifo record %u", rseq);
        omx_msgring_pop(&r);
        rseq++;
      }
    }
  }
  uint32_t size;
  const uint8_t *p;
  while ((p = omx_msgring_peek(&r, &size)) != NULL) {
    CHECK(size == rseq % 38u && matches(p, size, rseq), "fifo tail record %u", rseq);
    omx_msgring_pop(&r);
    rseq++;
  }
  CHECK(wseq == rseq && wseq > 10000, "every pushed record read once: wrote %u read %u", wseq, rseq);
}

#define THREADED_N 1000000u
static struct omx_msgring tr;
static uint8_t tbuf[256];

static void *producer(void *arg) {
  (void)arg;
  uint8_t msg[64];
  for (uint32_t seq = 0; seq < THREADED_N; seq++) {
    uint32_t n = 4u + seq % 53u;
    memcpy(msg, &seq, 4);
    fill(msg + 4, n - 4, seq);
    while (omx_msgring_push(&tr, msg, n) != 0) {
    }
  }
  return NULL;
}

static void test_threaded(void) {
  omx_msgring_init(&tr, tbuf, 256);
  pthread_t th;
  pthread_create(&th, NULL, producer, NULL);
  uint32_t want = 0, bad = 0;
  while (want < THREADED_N) {
    uint32_t size;
    const uint8_t *p = omx_msgring_peek(&tr, &size);
    if (!p) continue;
    uint32_t seq;
    memcpy(&seq, p, 4);
    if (seq != want || size != 4u + want % 53u || !matches(p + 4, size - 4, want)) bad++;
    omx_msgring_pop(&tr);
    want++;
  }
  pthread_join(th, NULL);
  CHECK(bad == 0, "threaded: %u of %u records out of order or torn", bad, THREADED_N);
  CHECK(omx_msgring_used(&tr) == 0, "threaded: ring empty at the end");
  printf("threaded: %u records through a 256-byte ring, %u bad\n", THREADED_N, bad);
}

int main(void) {
  test_accounting();
  test_refusals();
  test_fifo_wraps();
  test_threaded();
  if (failures) {
    fprintf(stderr, "msgring: %d failure(s)\n", failures);
    return 1;
  }
  printf("msgring: all passed\n");
  return 0;
}
