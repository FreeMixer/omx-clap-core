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
 * transport_seqlock_test.c: the published transport (src/clap_stage.h, struct omx_clap_transport_src) with an audio
 * thread as its one writer and other audio threads reading it in the same cycle (#121).
 *
 *   make test-transport          the arm below, then the same under ThreadSanitizer
 *
 * 1. A writer thread publishes a new record every cycle and three reader threads each read a whole record per cycle,
 *    1,000,000 cycles each. Every field of a record is a function of the publish that wrote it, its bar_number: a
 *    reader never sees a record whose fields come from two publishes, and its position never goes backwards except at
 *    a loop wrap the writer published (every LOOP_CYCLES publishes the position starts again from 0).
 * 2. The publish and the read allocate nothing: the writer and the readers run under the allocation witness (malloc,
 *    calloc, realloc and free wrapped at link time; not in the ThreadSanitizer build, which brings its own).
 * 3. Sabotage, make test-transport-sabotage: the same arm with the sequence re-check taken out of
 *    omx_clap_transport_read must report torn records.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clap_stage.h"

#define READERS     3
#define CYCLES      1000000u
#define LOOP_CYCLES 4096u   /* a loop wrap every this many publishes */

static int g_failures;

#define CHECK(cond, ...) \
    do { \
        if (cond) { printf("ok   " __VA_ARGS__); printf("\n"); } \
        else { printf("FAIL " __VA_ARGS__); printf("\n"); g_failures++; } \
    } while (0)

#ifdef OMX_TEST_WITNESS
/* the allocation witness: every allocator call made while g_witness is set is counted */
static _Atomic int g_witness, g_allocs;
void *__real_malloc(size_t n);
void *__real_calloc(size_t a, size_t b);
void *__real_realloc(void *p, size_t n);
void __real_free(void *p);
void *__wrap_malloc(size_t n) { atomic_fetch_add(&g_allocs, atomic_load(&g_witness)); return __real_malloc(n); }
void *__wrap_calloc(size_t a, size_t b) { atomic_fetch_add(&g_allocs, atomic_load(&g_witness)); return __real_calloc(a, b); }
void *__wrap_realloc(void *p, size_t n) { atomic_fetch_add(&g_allocs, atomic_load(&g_witness)); return __real_realloc(p, n); }
void __wrap_free(void *p) { atomic_fetch_add(&g_allocs, atomic_load(&g_witness)); __real_free(p); }
#endif

static struct omx_clap_transport_src g_src;
static _Atomic int g_go, g_readers_left;

/* the record of publish `k`: each field a function of k, the position looping every LOOP_CYCLES publishes */
static struct omx_clap_transport record_of(uint32_t k)
{
    struct omx_clap_transport t;
    const uint32_t at = k % LOOP_CYCLES;

    memset(&t, 0, sizeof t);
    t.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE |
              CLAP_TRANSPORT_HAS_TIME_SIGNATURE | (k & 1u ? CLAP_TRANSPORT_IS_PLAYING : 0u);
    t.tempo = 60.0 + (double)(k % 997u) / 8.0;
    t.song_pos_beats = (clap_beattime)at * (CLAP_BEATTIME_FACTOR / 4);
    t.song_pos_seconds = (clap_sectime)at * (CLAP_SECTIME_FACTOR / 8);
    t.bar_start = (clap_beattime)(at / 16u) * 4 * CLAP_BEATTIME_FACTOR;
    t.bar_number = (int32_t)k;
    t.tsig_num = (uint16_t)(1u + k % 15u);
    t.tsig_denom = (uint16_t)(1u << (k % 5u));
    return t;
}

static int whole(const struct omx_clap_transport *t)
{
    const struct omx_clap_transport want = record_of((uint32_t)t->bar_number);

    return t->flags == want.flags && t->tempo == want.tempo && t->song_pos_beats == want.song_pos_beats &&
           t->song_pos_seconds == want.song_pos_seconds && t->bar_start == want.bar_start &&
           t->tsig_num == want.tsig_num && t->tsig_denom == want.tsig_denom;
}

static void *writer(void *arg)
{
    uint32_t k = 1;
    struct omx_clap_transport t;

    (void)arg;
    while (!atomic_load(&g_go))
        ;
    while (atomic_load_explicit(&g_readers_left, memory_order_relaxed) > 0)
    {
        t = record_of(k++);
        omx_clap_transport_publish(&g_src, &t);
        // the rest of the writer's cycle: a short, varying stretch, so reads land both between publishes and over them
        for (uint32_t spin = 512u + k % 61u; spin > 0; spin--)
            (void)atomic_load_explicit(&g_readers_left, memory_order_relaxed);
    }
    return (void *)(uintptr_t)k;
}

struct reader_result
{
    uint32_t torn, backwards, missed, whole_reads;
};

static void *reader(void *arg)
{
    struct reader_result *r = arg;
    struct omx_clap_transport t, last;
    uint32_t c;
    int have = 0;

    memset(&last, 0, sizeof last);
    while (!atomic_load(&g_go))
        ;
    for (c = 0; c < CYCLES; c++)
    {
        // one whole record per cycle: a read a publish ran over every try of is counted and made again
        while (!omx_clap_transport_read(&g_src, &t))
            r->missed++;
        r->whole_reads++;
        if (!whole(&t))
            r->torn++;
        else if (have)
        {
            const uint32_t k = (uint32_t)t.bar_number, k0 = (uint32_t)last.bar_number;

            if (k < k0 || (t.song_pos_beats < last.song_pos_beats && k / LOOP_CYCLES == k0 / LOOP_CYCLES))
                r->backwards++;
        }
        if (whole(&t))
        {
            last = t;
            have = 1;
        }
    }
    atomic_fetch_sub(&g_readers_left, 1);
    return NULL;
}

int main(void)
{
    pthread_t w, rd[READERS];
    struct reader_result res[READERS];
    struct omx_clap_transport first = record_of(0);
    uint32_t i, torn = 0, backwards = 0, missed = 0, reads = 0;
    void *published;

    memset(res, 0, sizeof res);
    omx_clap_transport_publish(&g_src, &first);
    atomic_store(&g_readers_left, READERS);
    pthread_create(&w, NULL, writer, NULL);
    for (i = 0; i < READERS; i++)
        pthread_create(&rd[i], NULL, reader, &res[i]);
#ifdef OMX_TEST_WITNESS
    atomic_store(&g_witness, 1);
#endif
    atomic_store(&g_go, 1);
    for (i = 0; i < READERS; i++)
        pthread_join(rd[i], NULL);
    pthread_join(w, &published);
#ifdef OMX_TEST_WITNESS
    atomic_store(&g_witness, 0);
#endif
    for (i = 0; i < READERS; i++)
    {
        torn += res[i].torn;
        backwards += res[i].backwards;
        missed += res[i].missed;
        reads += res[i].whole_reads;
    }
    printf("     %u publishes; %u readers x %u cycles: %u whole reads, %u reads a publish ran over\n",
           (uint32_t)(uintptr_t)published - 1u, READERS, CYCLES, reads, missed);
    CHECK((uint32_t)(uintptr_t)published > LOOP_CYCLES, "the writer published every cycle and looped (%u publishes)",
          (uint32_t)(uintptr_t)published - 1u);
    CHECK(reads == READERS * CYCLES, "each reader read a whole record every cycle (%u)", reads);
    CHECK(torn == 0, "no reader saw a record whose fields come from two publishes (%u torn)", torn);
    CHECK(backwards == 0, "no reader's position went backwards but at a published loop wrap (%u)", backwards);
#ifdef OMX_TEST_WITNESS
    CHECK(atomic_load(&g_allocs) == 0, "the publish and the reads allocate nothing (%d allocator calls)", atomic_load(&g_allocs));
#endif
    if (g_failures)
    {
        printf("transport seqlock: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("transport seqlock: all passed\n");
    return 0;
}
