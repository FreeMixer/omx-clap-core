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


/*
 * The resource hints a plugin declares about itself (org.openmixer.resource-hints.draft/0, omx-contract's
 * RESOURCE_HINTS), through libomx-clap-core against tests/fake_ext.clap, which states two of them and leaves five clear
 * with garbage in their members: the two read back, the five read as unknown and never as zero; one bit flipped and
 * restored; a bit RESOURCE_HINTS has no hint for, and a flag that is neither 0 nor 1, refused with their codes; no
 * extension, and a get that answers false, read as none. The words are omx-contract's, in its order.
 *   resource_hints_test <fake_ext.clap>
 */

#include <dlfcn.h>
#include <stdlib.h>

#include "../src/clap_host.h"
#include <omxcontract/omx_vocabulary.h>
#include "test_util.h"

#define EXT_ID          "org.omx-clap-host.test.ext"
#define AT_REST         3145728u    // the fixture's footprint-at-rest, bytes
#define THREADS         2u          // its threads-spawned
#define TWO             (OMX_RESOURCE_HINT_FOOTPRINT_AT_REST | OMX_RESOURCE_HINT_THREADS_SPAWNED)

typedef struct
{
    uint32_t offer, answer, streams_byte;
    omx_resource_hints_t hints;
} fixture_hints_t;

/* the hints of an instance opened afresh, the plugin's get asked again: rc of omx_clap_host_resource_hints */
static int reopen(const char *path, omx_resource_hints_t *hints, const char **why)
{
    struct omx_clap_instance *in = NULL;
    char reason[OMX_CLAP_WHY_MAX];
    int rc;

    if (omx_clap_host_open(path, EXT_ID, &in, reason) != 0 || !in)
    {
        printf("FAIL fake_ext opens (%s)\n", reason);
        g_failures++;
        return -2;
    }
    memset(hints, 0x5a, sizeof(*hints));
    rc = omx_clap_host_resource_hints(in, hints, why);
    omx_clap_host_close(in);
    return rc;
}

/* how many of the seven read as unknown: figure() answers 0 and leaves the figure 0 */
static uint32_t unknowns(const omx_resource_hints_t *hints)
{
    uint32_t i, n = 0;

    for (i = 0; i < OMX_RESOURCE_HINT_COUNT; i++)
    {
        uint64_t f = 1;

        n += omx_clap_resource_hint_figure(hints, i, &f) == 0 && f == 0;
    }
    return n;
}

int main(int argc, char **argv)
{
    static const char *const words[] = OMX_RESOURCE_HINTS_INIT;
    fixture_hints_t *fx, saved;
    omx_resource_hints_t h;
    const char *why = "unset";
    uint64_t f;
    uint32_t i;
    void *file;
    int rc;

    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <fake_ext.clap>\n", argv[0]);
        return 2;
    }
    file = dlopen(argv[1], RTLD_NOW);
    fx = file ? ((fixture_hints_t * (*)(void))dlsym(file, "fake_ext_hints"))() : NULL;
    CHECK(fx != NULL, "the fixture's hints are reachable (%s)", file ? "fake_ext_hints" : dlerror());
    if (!fx)
        return report("resource hints test");
    saved = *fx;

    /* the words and units: omx-contract's, one per bit, in the order of RESOURCE_HINTS */
    CHECK(OMX_RESOURCE_HINT_COUNT == 7u && sizeof(words) / sizeof(words[0]) == OMX_RESOURCE_HINT_COUNT, "seven hints");
    for (i = 0; i < OMX_RESOURCE_HINT_COUNT; i++)
        CHECK(omx_clap_resource_hint_word(i) && !strcmp(omx_clap_resource_hint_word(i), words[i]),
              "hint %u is %s (%s)", i, words[i], omx_clap_resource_hint_word(i));
    CHECK(!strcmp(omx_clap_resource_hint_unit(0), "bytes") && !strcmp(omx_clap_resource_hint_unit(1), "bytes") &&
              !strcmp(omx_clap_resource_hint_unit(2), "bytes") && !strcmp(omx_clap_resource_hint_unit(3), "count") &&
              !strcmp(omx_clap_resource_hint_unit(4), "flag") && !strcmp(omx_clap_resource_hint_unit(5), "flag") &&
              !strcmp(omx_clap_resource_hint_unit(6), "ms"),
          "the units: bytes, bytes, bytes, count, flag, flag, ms");
    CHECK(omx_clap_resource_hint_word(OMX_RESOURCE_HINT_COUNT) == NULL && omx_clap_resource_hint_unit(OMX_RESOURCE_HINT_COUNT) == NULL,
          "no word and no unit past the seventh");

    /* the declaration: two hints read back, the five others unknown, whatever their members held */
    rc = reopen(argv[1], &h, &why);
    CHECK(rc == 1 && why == NULL && h.declared == TWO, "two hints declared (%d, %#x, %s)", rc, h.declared, why ? why : "-");
    CHECK(omx_clap_resource_hint_figure(&h, 0, &f) == 1 && f == AT_REST && h.footprint_at_rest == AT_REST,
          "footprint-at-rest reads %u bytes (%llu)", AT_REST, (unsigned long long)f);
    CHECK(omx_clap_resource_hint_figure(&h, 3, &f) == 1 && f == THREADS && h.threads_spawned == THREADS,
          "threads-spawned reads %u (%llu)", THREADS, (unsigned long long)f);
    CHECK(unknowns(&h) == 5u, "the five others read as unknown (%u)", unknowns(&h));
    CHECK(h.footprint_per_voice == 0 && h.shared_load == 0 && !h.streams_from_disk && !h.opens_sockets && h.activate_time == 0,
          "an unknown hint's member is 0, never the plugin's garbage (%llu, %llu, %d, %d, %u)",
          (unsigned long long)h.footprint_per_voice, (unsigned long long)h.shared_load, h.streams_from_disk, h.opens_sockets,
          h.activate_time);

    /* sabotage: one bit flipped, opens-sockets declared, and the garbage it held becomes its figure; restored, unknown */
    fx->hints.declared ^= OMX_RESOURCE_HINT_OPENS_SOCKETS;
    rc = reopen(argv[1], &h, &why);
    CHECK(rc == 1 && h.declared == (TWO | OMX_RESOURCE_HINT_OPENS_SOCKETS) && omx_clap_resource_hint_figure(&h, 5, &f) == 1 && f == 1 &&
              unknowns(&h) == 4u,
          "the bit flipped: opens-sockets reads declared, 1, and four stay unknown (%#x, %u)", h.declared, unknowns(&h));
    *fx = saved;
    rc = reopen(argv[1], &h, &why);
    CHECK(rc == 1 && h.declared == TWO && omx_clap_resource_hint_figure(&h, 5, &f) == 0 && unknowns(&h) == 5u,
          "restored: two declared, opens-sockets unknown again (%#x)", h.declared);

    /* refused: a bit with no hint, and a declared flag whose byte is 2 */
    fx->hints.declared |= 1u << OMX_RESOURCE_HINT_COUNT;
    rc = reopen(argv[1], &h, &why);
    CHECK(rc == -1 && why && !strcmp(why, "hosting.resource-hints.unknown-hint") && h.declared == 0 && unknowns(&h) == 7u,
          "a bit past the seventh: refused, hosting.resource-hints.unknown-hint, every hint unknown (%d, %s)", rc, why ? why : "-");
    *fx = saved;
    fx->hints.declared |= OMX_RESOURCE_HINT_STREAMS_FROM_DISK;
    fx->streams_byte = 2u;
    rc = reopen(argv[1], &h, &why);
    CHECK(rc == -1 && why && !strcmp(why, "hosting.resource-hints.wrong-unit") && h.declared == 0 && unknowns(&h) == 7u,
          "a flag of 2: refused, hosting.resource-hints.wrong-unit, every hint unknown (%d, %s)", rc, why ? why : "-");
    fx->hints.declared = saved.hints.declared;
    rc = reopen(argv[1], &h, &why);
    CHECK(rc == 1 && why == NULL && h.declared == TWO && !h.streams_from_disk,
          "the same byte under a clear bit is unknown, not read: two declared (%d, %#x)", rc, h.declared);
    *fx = saved;

    /* none: a get that answers false, and no extension */
    fx->answer = 0;
    rc = reopen(argv[1], &h, &why);
    CHECK(rc == 0 && why == NULL && h.declared == 0 && unknowns(&h) == 7u, "a get that answers false states none (%d)", rc);
    *fx = saved;
    fx->offer = 0;
    rc = reopen(argv[1], &h, &why);
    CHECK(rc == 0 && why == NULL && h.declared == 0 && unknowns(&h) == 7u, "no extension states none: the mask all clear (%d)", rc);
    *fx = saved;

    CHECK(omx_clap_host_resource_hints(NULL, &h, &why) == 0 && h.declared == 0 && why == NULL, "no instance: none");
    CHECK(omx_clap_resource_hint_figure(NULL, 0, &f) == -1 && omx_clap_resource_hint_figure(&h, OMX_RESOURCE_HINT_COUNT, &f) == -1,
          "no hints, or an index past the seventh: -1");
    dlclose(file);
    return report("resource hints test");
}
