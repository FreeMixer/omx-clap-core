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
 * Its twins in the same TTL (an atom port, a mono-in/stereo-out pair) have NO descriptor here: the
 * host must refuse them from the TTL before it opens this binary at all.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <lv2/atom/atom.h>
#include <lv2/core/lv2.h>
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

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index) { return index == 0 ? &DESCRIPTOR : NULL; }
