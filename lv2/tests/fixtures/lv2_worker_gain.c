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
#include <lv2/state/state.h>
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

/* ---- the live twin ---- */

struct live_msg {
  int32_t kind; /* 0: the gain port's dB, 1: the state's trim in dB; a response carries the linear value */
  float value;
};

struct live {
  struct fx fx;
  LV2_URID trim_key, float_t, max_key, int_t;
  float trim; /* linear; moved ONLY by work_response */
};

static LV2_Handle live_instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *features) {
  struct fx *base = instantiate(d, rate, path, features);
  if (!base) return NULL;
  struct live *l = calloc(1, sizeof *l);
  if (!l) {
    free(base);
    return NULL;
  }
  l->fx = *base;
  free(base);
  l->trim = 1.0f;
  l->trim_key = l->fx.map->map(l->fx.map->handle, LV2_WORKER_GAIN_LIVE_URI "-trim");
  l->float_t = l->fx.map->map(l->fx.map->handle, LV2_ATOM__Float);
  l->max_key = l->fx.map->map(l->fx.map->handle, LV2_BUF_SIZE__maxBlockLength);
  l->int_t = l->fx.map->map(l->fx.map->handle, LV2_ATOM__Int);
  return l;
}

static void live_run(LV2_Handle h, uint32_t n) {
  struct live *l = h;
  struct fx *f = &l->fx;
  if (n > f->max_block) atomic_fetch_add(&lv2_worker_gain_probe.over_block, 1);
  const struct live_msg m = {0, *f->gain_db};
  if (m.value != f->scheduled_db && f->schedule->schedule_work(f->schedule->handle, sizeof m, &m) == LV2_WORKER_SUCCESS) {
    f->scheduled_db = m.value;
    atomic_fetch_add(&lv2_worker_gain_probe.scheduled, 1);
  }
  for (uint32_t i = 0; i < n; i++) f->out[i] = f->in[i] * f->applied * l->trim;
}

static LV2_Worker_Status live_work(LV2_Handle h, LV2_Worker_Respond_Function respond, LV2_Worker_Respond_Handle rh,
                                   uint32_t size, const void *data) {
  (void)h;
  struct live_msg m;
  if (size != sizeof m) return LV2_WORKER_ERR_UNKNOWN;
  memcpy(&m, data, sizeof m);
  m.value = powf(10.0f, m.value / 20.0f);
  atomic_fetch_add(&lv2_worker_gain_probe.works, 1);
  return respond(rh, sizeof m, &m);
}

static LV2_Worker_Status live_response(LV2_Handle h, uint32_t size, const void *data) {
  struct live *l = h;
  struct live_msg m;
  if (size != sizeof m) return LV2_WORKER_ERR_UNKNOWN;
  memcpy(&m, data, sizeof m);
  if (m.kind == 1) {
    l->trim = m.value;
    lv2_worker_gain_probe.trim_applied = m.value;
  } else {
    l->fx.applied = m.value;
  }
  atomic_fetch_add(&lv2_worker_gain_probe.responses, 1);
  return LV2_WORKER_SUCCESS;
}

static uint32_t live_options_get(LV2_Handle h, LV2_Options_Option *options) {
  (void)h;
  (void)options;
  return LV2_OPTIONS_ERR_UNKNOWN;
}

/* a block length told while the plugin lives: run() checks against it from now on */
static uint32_t live_options_set(LV2_Handle h, const LV2_Options_Option *options) {
  struct live *l = h;
  for (const LV2_Options_Option *o = options; o->key; o++)
    if (o->key == l->max_key && o->type == l->int_t) {
      l->fx.max_block = (uint32_t)*(const int32_t *)o->value;
      lv2_worker_gain_probe.max_block_set = *(const int32_t *)o->value;
    }
  atomic_fetch_add(&lv2_worker_gain_probe.options_set, 1);
  return LV2_OPTIONS_SUCCESS;
}

static LV2_State_Status live_save(LV2_Handle h, LV2_State_Store_Function store, LV2_State_Handle sh, uint32_t flags,
                                  const LV2_Feature *const *features) {
  struct live *l = h;
  (void)flags, (void)features;
  return store(sh, l->trim_key, &lv2_worker_gain_probe.save_trim_db, sizeof(float), l->float_t, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
}

/* thread-safe: nothing here touches what run() reads; the trim goes to the worker, from the schedule restore() is given */
static LV2_State_Status live_restore(LV2_Handle h, LV2_State_Retrieve_Function retrieve, LV2_State_Handle sh, uint32_t flags,
                                     const LV2_Feature *const *features) {
  struct live *l = h;
  const LV2_Worker_Schedule *schedule = NULL;
  size_t size = 0;
  uint32_t type = 0, vflags = 0;
  (void)flags;
  atomic_fetch_add(&lv2_worker_gain_probe.restores, 1);
  for (int i = 0; features && features[i]; i++)
    if (!strcmp(features[i]->URI, LV2_WORKER__schedule)) schedule = features[i]->data;
  lv2_worker_gain_probe.restore_had_schedule = schedule != NULL;
  const float *db = retrieve(sh, l->trim_key, &size, &type, &vflags);
  if (!db || size != sizeof(float) || type != l->float_t) return LV2_STATE_ERR_NO_PROPERTY;
  const struct live_msg m = {1, *db};
  if (!schedule || schedule->schedule_work(schedule->handle, sizeof m, &m) != LV2_WORKER_SUCCESS) return LV2_STATE_ERR_UNKNOWN;
  return LV2_STATE_SUCCESS;
}

static const LV2_Worker_Interface LIVE_WORKER = {live_work, live_response, NULL};
static const LV2_Options_Interface LIVE_OPTIONS = {live_options_get, live_options_set};
static const LV2_State_Interface LIVE_STATE = {live_save, live_restore};

static const void *live_extension_data(const char *uri) {
  if (!strcmp(uri, LV2_WORKER__interface)) return &LIVE_WORKER;
  if (!strcmp(uri, LV2_OPTIONS__interface)) return &LIVE_OPTIONS;
  if (!strcmp(uri, LV2_STATE__interface)) return &LIVE_STATE;
  return NULL;
}

static const LV2_Descriptor LIVE_DESCRIPTOR = {
  LV2_WORKER_GAIN_LIVE_URI, live_instantiate, connect_port, activate, live_run, NULL, cleanup, live_extension_data,
};

const LV2_Descriptor *lv2_worker_gain_live_descriptor(void) { return &LIVE_DESCRIPTOR; }

#ifdef LV2_WORKER_GAIN_BUNDLE
/* built as the bundle omx-worker-gain.lv2: the entry point a host walks */
LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index) {
  return index == 0 ? lv2_worker_gain_descriptor() : index == 1 ? lv2_worker_gain_live_descriptor() : NULL;
}
#endif
