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
 * lv2_host_fixture.c — the fixture bundle omx-host-fixture.lv2
 * the host half's oracle racks through lilv (lv2/tests/lv2_host_test.c).
 *
 * A mono gain built to be UNFORGIVING of a host that skips a step, the way third-party plugins are:
 *  - run() DEREFERENCES every port it declares — the audio pair, `lv2:enabled`, the gain row, the
 *    meter output and the latency output — so a port the host left unconnected is a SIGSEGV, not a
 *    silent default;
 *  - instantiate() REFUSES (returns NULL) without `urid:map`, and when `bundle_path` is not this
 *    bundle's directory;
 *  - the gain is the row (`gain_db`, TTL default −20 dB) PLUS a state property (`offset_db`) that
 *    only `state:loadDefaultState` delivers: the TTL's default state holds −6 dB, the plugin's own
 *    fallback is 0 dB, so −26 dB proves the state was restored and −20 dB proves it was not;
 *  - the latency port reports 7 frames — a DECLARED figure for the host to publish, not a delay the
 *    fixture applies.
 * Its refused twins in the same TTL (an atom port, three inputs, a plugin with no port at all) have NO descriptor
 * here: the host must refuse them from the TTL before it opens this binary at all.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "lv2_host_fixture.h"

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>

#define FIXTURE_URI "urn:openmixer:test:host-fixture"
#define FIXTURE_OFFSET_URI FIXTURE_URI "#offset_db"
#define FIXTURE_BUNDLE_DIR "omx-host-fixture.lv2/"
#define FIXTURE_LATENCY 7.0f

enum { P_IN, P_OUT, P_ENABLED, P_GAIN, P_METER, P_LATENCY, P_COUNT };

struct fixture {
  const float *in;
  float *out;
  const float *enabled, *gain_db;
  float *meter, *latency;
  float offset_db;
  LV2_URID offset_key, atom_float;
};

static LV2_Handle instantiate(const LV2_Descriptor *d, double rate, const char *bundle_path,
                              const LV2_Feature *const *features) {
  (void)d;
  (void)rate;
  const size_t bl = bundle_path ? strlen(bundle_path) : 0, dl = strlen(FIXTURE_BUNDLE_DIR);
  if (bl < dl || strcmp(bundle_path + bl - dl, FIXTURE_BUNDLE_DIR) != 0) return NULL;
  const LV2_URID_Map *map = NULL;
  for (int i = 0; features && features[i]; i++)
    if (strcmp(features[i]->URI, LV2_URID__map) == 0) map = features[i]->data;
  if (!map) return NULL;
  struct fixture *f = calloc(1, sizeof *f);
  if (!f) return NULL;
  f->offset_key = map->map(map->handle, FIXTURE_OFFSET_URI);
  f->atom_float = map->map(map->handle, LV2_ATOM__Float);
  return f;
}

static void connect_port(LV2_Handle h, uint32_t port, void *data) {
  struct fixture *f = h;
  switch (port) {
    case P_IN: f->in = data; break;
    case P_OUT: f->out = data; break;
    case P_ENABLED: f->enabled = data; break;
    case P_GAIN: f->gain_db = data; break;
    case P_METER: f->meter = data; break;
    case P_LATENCY: f->latency = data; break;
    default: break;
  }
}

static void run(LV2_Handle h, uint32_t n) {
  struct fixture *f = h;
  const int on = *f->enabled > 0.5f;
  const float g = powf(10.0f, (*f->gain_db + f->offset_db) / 20.0f);
  float peak = 0.0f;
  for (uint32_t i = 0; i < n; i++) {
    f->out[i] = on ? f->in[i] * g : f->in[i];
    peak = fmaxf(peak, fabsf(f->out[i]));
  }
  *f->meter = peak;
  *f->latency = FIXTURE_LATENCY;
}

static void cleanup(LV2_Handle h) { free(h); }

static LV2_State_Status restore(LV2_Handle h, LV2_State_Retrieve_Function retrieve, LV2_State_Handle sh,
                                uint32_t flags, const LV2_Feature *const *features) {
  (void)flags;
  (void)features;
  struct fixture *f = h;
  size_t size = 0;
  uint32_t type = 0, vflags = 0;
  const void *v = retrieve(sh, f->offset_key, &size, &type, &vflags);
  if (v && type == f->atom_float && size == sizeof(float)) f->offset_db = *(const float *)v;
  return LV2_STATE_SUCCESS;
}

static LV2_State_Status save(LV2_Handle h, LV2_State_Store_Function store, LV2_State_Handle sh, uint32_t flags,
                             const LV2_Feature *const *features) {
  (void)features;
  (void)flags;
  struct fixture *f = h;
  return store(sh, f->offset_key, &f->offset_db, sizeof f->offset_db, f->atom_float,
               LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
}

static const void *extension_data(const char *uri) {
  static const LV2_State_Interface state = {save, restore};
  return strcmp(uri, LV2_STATE__interface) == 0 ? &state : NULL;
}

static const LV2_Descriptor DESCRIPTOR = {FIXTURE_URI, instantiate, connect_port, NULL, run, NULL, cleanup, extension_data};

/* the fixed-block twin (#fixed, the ttl's lv2:requiredFeature bufsz:fixedBlockLength): in to out, nothing else, so a
 * host declaring a fixed block can instantiate and run it, and one that does not refuses it before this is reached */
struct fixed_twin { float *in, *out; };
static LV2_Handle fixed_instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *f)
{
    (void)d; (void)rate; (void)path; (void)f;
    return calloc(1, sizeof(struct fixed_twin));
}
static void fixed_connect_port(LV2_Handle h, uint32_t port, void *data)
{
    struct fixed_twin *t = h;
    if (port == 0) t->in = data; else if (port == 1) t->out = data;
}
static void fixed_run(LV2_Handle h, uint32_t n)
{
    struct fixed_twin *t = h;
    if (t->in && t->out) memmove(t->out, t->in, n * sizeof(float));
}
static void fixed_cleanup(LV2_Handle h) { free(h); }
static const LV2_Descriptor FIXED_TWIN = {FIXTURE_URI "#fixed", fixed_instantiate, fixed_connect_port, NULL, fixed_run, NULL, fixed_cleanup, NULL};

/* the in-place twin (#inplace, lv2:requiredFeature lv2:inPlaceBroken): its witness counts the runs and the runs whose
 * input and output were one buffer, which a host honouring the requirement never does */
struct lv2_host_fixture_inplace lv2_host_fixture_inplace;
static void inplace_run(LV2_Handle h, uint32_t n)
{
    struct fixed_twin *t = h;
    lv2_host_fixture_inplace.runs++;
    if (t->in && t->out && t->in == t->out) lv2_host_fixture_inplace.aliased++;
    if (t->in && t->out) memmove(t->out, t->in, n * sizeof(float));
}
static const LV2_Descriptor INPLACE_TWIN = {FIXTURE_URI "#inplace", fixed_instantiate, fixed_connect_port, NULL, inplace_run, NULL, fixed_cleanup, NULL};

/* the two-MIDI-inputs twin (#midi-two): audio in to out, MIDI A (2) the note path's, MIDI B (3) fed empty; admitted since
 * a MIDI port after the first is an input fed empty (#199), so it needs a descriptor to run through */
struct midi_two { float *in, *out; const void *a, *b; };
static LV2_Handle midi_two_instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *f)
{
    (void)d; (void)rate; (void)path; (void)f;
    return calloc(1, sizeof(struct midi_two));
}
static void midi_two_connect_port(LV2_Handle h, uint32_t port, void *data)
{
    struct midi_two *t = h;
    if (port == 0) t->in = data; else if (port == 1) t->out = data; else if (port == 2) t->a = data; else if (port == 3) t->b = data;
}
static const LV2_Descriptor MIDI_TWO_TWIN = {FIXTURE_URI "#midi-two", midi_two_instantiate, midi_two_connect_port, NULL, fixed_run, NULL, fixed_cleanup, NULL};

/* the analyser twin (#no-out): one audio input and a control output, the block's peak; no audio output at all */
struct analyser { const float *in; float *peak; };
static LV2_Handle analyser_instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *f)
{
    (void)d; (void)rate; (void)path; (void)f;
    return calloc(1, sizeof(struct analyser));
}
static void analyser_connect_port(LV2_Handle h, uint32_t port, void *data)
{
    struct analyser *a = h;
    if (port == 0) a->in = data; else if (port == 1) a->peak = data;
}
static void analyser_run(LV2_Handle h, uint32_t n)
{
    struct analyser *a = h;
    float peak = 0.0f;
    uint32_t i;
    if (!a->in) return;
    for (i = 0; i < n; i++) peak = fmaxf(peak, fabsf(a->in[i]));
    if (a->peak) *a->peak = peak;
}
static const LV2_Descriptor ANALYSER_TWIN = {FIXTURE_URI "#no-out", analyser_instantiate, analyser_connect_port, NULL, analyser_run, NULL, fixed_cleanup, NULL};

/* the generator twin (#gen): a stereo output and no input of any kind; 0.5 on the left and 0.25 on the right, every
 * frame, the levels of the e2e's meter source, so a graph that racks it hears a known signal */
#define GEN_LEFT 0.5f
#define GEN_RIGHT 0.25f
struct generator { float *out[2]; };
static LV2_Handle generator_instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *f)
{
    (void)d; (void)rate; (void)path; (void)f;
    return calloc(1, sizeof(struct generator));
}
static void generator_connect_port(LV2_Handle h, uint32_t port, void *data)
{
    struct generator *g = h;
    if (port < 2) g->out[port] = data;
}
static void generator_run(LV2_Handle h, uint32_t n)
{
    struct generator *g = h;
    uint32_t i;
    for (i = 0; i < n; i++)
    {
        if (g->out[0]) g->out[0][i] = GEN_LEFT;
        if (g->out[1]) g->out[1][i] = GEN_RIGHT;
    }
}
static const LV2_Descriptor GENERATOR_TWIN = {FIXTURE_URI "#gen", generator_instantiate, generator_connect_port, NULL, generator_run, NULL, fixed_cleanup, NULL};

/* the mono-in stereo-out twin (#wide): two main ports of different widths, out_l = in and out_r = in / 2 */
struct wide { const float *in; float *out[2]; };
static LV2_Handle wide_instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *f)
{
    (void)d; (void)rate; (void)path; (void)f;
    return calloc(1, sizeof(struct wide));
}
static void wide_connect_port(LV2_Handle h, uint32_t port, void *data)
{
    struct wide *w = h;
    if (port == 0) w->in = data; else if (port < 3) w->out[port - 1] = data;
}
static void wide_run(LV2_Handle h, uint32_t n)
{
    struct wide *w = h;
    uint32_t i;
    for (i = 0; i < n; i++)
    {
        const float x = w->in[i];
        w->out[0][i] = x;
        w->out[1][i] = 0.5f * x;
    }
}
static const LV2_Descriptor WIDE_TWIN = {FIXTURE_URI "#wide", wide_instantiate, wide_connect_port, NULL, wide_run, NULL, fixed_cleanup, NULL};

/* the control-only twin (#ctl-alone): a control input `amount` and a control output `twice`, amount * 2, written every
 * run(); no audio port and no MIDI port. Both ports are dereferenced, so one the host left unconnected is a SIGSEGV */
struct ctl_alone { const float *amount; float *twice; };
static LV2_Handle ctl_alone_instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *f)
{
    (void)d; (void)rate; (void)path; (void)f;
    return calloc(1, sizeof(struct ctl_alone));
}
static void ctl_alone_connect_port(LV2_Handle h, uint32_t port, void *data)
{
    struct ctl_alone *c = h;
    if (port == 0) c->amount = data; else if (port == 1) c->twice = data;
}
static void ctl_alone_run(LV2_Handle h, uint32_t n)
{
    struct ctl_alone *c = h;
    (void)n;
    *c->twice = *c->amount * 2.0f;
}
static const LV2_Descriptor CTL_ALONE_TWIN = {FIXTURE_URI "#ctl-alone", ctl_alone_instantiate, ctl_alone_connect_port, NULL, ctl_alone_run, NULL, fixed_cleanup, NULL};

/* the MIDI sink twin (#ctl-only): a MIDI input, a control input `amount` and a control output `notes`, the count of MIDI
 * events read in the block; no audio port and no output but the control one. Every port is dereferenced */
struct ctl_only { const LV2_Atom_Sequence *midi; const float *amount; float *notes; LV2_URID midi_event; };
static LV2_Handle ctl_only_instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *f)
{
    const LV2_URID_Map *map = NULL;
    struct ctl_only *c;
    (void)d; (void)rate; (void)path;
    for (int i = 0; f && f[i]; i++)
        if (strcmp(f[i]->URI, LV2_URID__map) == 0) map = f[i]->data;
    if (!map) return NULL;
    c = calloc(1, sizeof(*c));
    if (c) c->midi_event = map->map(map->handle, LV2_MIDI__MidiEvent);
    return c;
}
static void ctl_only_connect_port(LV2_Handle h, uint32_t port, void *data)
{
    struct ctl_only *c = h;
    if (port == 0) c->midi = data; else if (port == 1) c->amount = data; else if (port == 2) c->notes = data;
}
static void ctl_only_run(LV2_Handle h, uint32_t n)
{
    struct ctl_only *c = h;
    unsigned count = 0;
    (void)n;
    (void)*c->amount;
    LV2_ATOM_SEQUENCE_FOREACH(c->midi, ev)
        if (ev->body.type == c->midi_event) count++;
    *c->notes = (float)count;
}
static const LV2_Descriptor CTL_ONLY_TWIN = {FIXTURE_URI "#ctl-only", ctl_only_instantiate, ctl_only_connect_port, NULL, ctl_only_run, NULL, fixed_cleanup, NULL};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    return index == 0 ? &DESCRIPTOR : index == 1 ? &FIXED_TWIN : index == 2 ? &INPLACE_TWIN : index == 3 ? &MIDI_TWO_TWIN
         : index == 4 ? &ANALYSER_TWIN : index == 5 ? &GENERATOR_TWIN : index == 6 ? &WIDE_TWIN : index == 7 ? &CTL_ALONE_TWIN
         : index == 8 ? &CTL_ONLY_TWIN : NULL;
}
