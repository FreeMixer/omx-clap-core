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

/* lv2_worker_gain.c — see lv2_worker_gain.h. A test fixture, never a shipped plugin. */
#include "lv2_worker_gain.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <lv2/atom/atom.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/log/log.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

struct lv2_worker_gain_probe lv2_worker_gain_probe;

struct fx {
  const float *in;
  float *out;
  const float *gain_db;
  const LV2_Worker_Schedule *schedule;
  const LV2_URID_Map *map;
  uint32_t max_block;
  float scheduled_db; /* the last value run() handed the worker */
  float applied;      /* linear; moved ONLY by work_response */
};

static LV2_Handle instantiate(const LV2_Descriptor *d, double rate, const char *path,
                              const LV2_Feature *const *features) {
  (void)d, (void)rate, (void)path;
  atomic_fetch_add(&lv2_worker_gain_probe.instantiate_calls, 1);
  const LV2_URID_Map *map = NULL;
  const LV2_Worker_Schedule *schedule = NULL;
  const LV2_Options_Option *options = NULL;
  const LV2_URID_Unmap *unmap = NULL;
  const LV2_Log_Log *log = NULL;
  for (int i = 0; features && features[i]; i++) {
    if (!strcmp(features[i]->URI, LV2_URID__unmap)) unmap = features[i]->data;
    else if (!strcmp(features[i]->URI, LV2_LOG__log)) log = features[i]->data;
    else if (!strcmp(features[i]->URI, LV2_BUF_SIZE__boundedBlockLength)) lv2_worker_gain_probe.saw_bounded = 1;
    if (!strcmp(features[i]->URI, LV2_URID__map)) map = features[i]->data;
    else if (!strcmp(features[i]->URI, LV2_WORKER__schedule)) schedule = features[i]->data;
    else if (!strcmp(features[i]->URI, LV2_OPTIONS__options)) options = features[i]->data;
  }
  if (!map || !schedule || !options) return NULL; /* all three are lv2:requiredFeature */
  const LV2_URID u_int = map->map(map->handle, LV2_ATOM__Int), u_float = map->map(map->handle, LV2_ATOM__Float);
  const LV2_URID u_max = map->map(map->handle, LV2_BUF_SIZE__maxBlockLength);
  const LV2_URID u_min = map->map(map->handle, LV2_BUF_SIZE__minBlockLength);
  const LV2_URID u_rate = map->map(map->handle, LV2_PARAMETERS__sampleRate);
  const LV2_URID u_nominal = map->map(map->handle, LV2_BUF_SIZE__nominalBlockLength);
  if (unmap) {
    const char *back = unmap->unmap(unmap->handle, u_rate);
    lv2_worker_gain_probe.unmap_round_trip = back && !strcmp(back, LV2_PARAMETERS__sampleRate);
  }
  if (log) lv2_worker_gain_probe.log_answered = log->printf(log->handle, map->map(map->handle, LV2_LOG__Note), "instantiate %d\n", 1) == 0;
  int32_t max_block = 0;
  for (const LV2_Options_Option *o = options; o->key; o++) {
    if (o->key == u_max && o->type == u_int) max_block = *(const int32_t *)o->value;
    else if (o->key == u_min && o->type == u_int) lv2_worker_gain_probe.min_block_option = *(const int32_t *)o->value;
    else if (o->key == u_rate && o->type == u_float) lv2_worker_gain_probe.rate_option = *(const float *)o->value;
    else if (o->key == u_nominal && o->type == u_int) lv2_worker_gain_probe.nominal_block_option = *(const int32_t *)o->value;
  }
  lv2_worker_gain_probe.max_block_option = max_block;
  if (max_block <= 0) return NULL;
  struct fx *f = calloc(1, sizeof *f);
  if (!f) return NULL;
  f->map = map;
  f->schedule = schedule;
  f->max_block = (uint32_t)max_block;
  f->scheduled_db = NAN;
  f->applied = 1.0f;
  atomic_fetch_add(&lv2_worker_gain_probe.instantiated, 1);
  return f;
}

static void connect_port(LV2_Handle h, uint32_t port, void *data) {
  struct fx *f = h;
  if (port == LV2_WORKER_GAIN_IN) f->in = data;
  else if (port == LV2_WORKER_GAIN_OUT) f->out = data;
  else if (port == LV2_WORKER_GAIN_DB) f->gain_db = data;
}

static void activate(LV2_Handle h) {
  (void)h;
  atomic_fetch_add(&lv2_worker_gain_probe.activated, 1);
}

static void run(LV2_Handle h, uint32_t n) {
  struct fx *f = h;
  lv2_worker_gain_probe.run_thread = pthread_self();
  if (n > f->max_block) atomic_fetch_add(&lv2_worker_gain_probe.over_block, 1);
  const float db = *f->gain_db;
  if (db != f->scheduled_db &&
      f->schedule->schedule_work(f->schedule->handle, sizeof db, &db) == LV2_WORKER_SUCCESS) {
    f->scheduled_db = db;
    atomic_fetch_add(&lv2_worker_gain_probe.scheduled, 1);
  }
  for (uint32_t i = 0; i < n; i++) f->out[i] = f->in[i] * f->applied;
}

static LV2_Worker_Status work(LV2_Handle h, LV2_Worker_Respond_Function respond, LV2_Worker_Respond_Handle rh,
                              uint32_t size, const void *data) {
  struct fx *f = h;
  if (size != sizeof(float)) return LV2_WORKER_ERR_UNKNOWN;
  lv2_worker_gain_probe.work_thread = pthread_self();
  if (f->map->map(f->map->handle, "urn:openmixer:test:worker-gain#mapped-in-work") != 0)
    atomic_fetch_add(&lv2_worker_gain_probe.mapped_in_work, 1);
  float db;
  memcpy(&db, data, sizeof db);
  const float lin = powf(10.0f, db / 20.0f);
  atomic_fetch_add(&lv2_worker_gain_probe.works, 1);
  return respond(rh, sizeof lin, &lin);
}

static LV2_Worker_Status work_response(LV2_Handle h, uint32_t size, const void *data) {
  struct fx *f = h;
  if (size != sizeof(float)) return LV2_WORKER_ERR_UNKNOWN;
  memcpy(&f->applied, data, sizeof f->applied);
  atomic_fetch_add(&lv2_worker_gain_probe.responses, 1);
  return LV2_WORKER_SUCCESS;
}

static const LV2_Worker_Interface WORKER = {work, work_response, NULL};

static const void *extension_data(const char *uri) {
  return strcmp(uri, LV2_WORKER__interface) == 0 ? &WORKER : NULL;
}

static void cleanup(LV2_Handle h) {
  free(h);
  atomic_fetch_add(&lv2_worker_gain_probe.cleaned_up, 1);
}

static const LV2_Descriptor DESCRIPTOR = {
  LV2_WORKER_GAIN_URI, instantiate, connect_port, activate, run, NULL, cleanup, extension_data,
};

const LV2_Descriptor *lv2_worker_gain_descriptor(void) { return &DESCRIPTOR; }

#ifdef LV2_WORKER_GAIN_BUNDLE
/* built as the bundle omx-worker-gain.lv2: the entry point a host walks */
LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index) { return index == 0 ? lv2_worker_gain_descriptor() : NULL; }
#endif
