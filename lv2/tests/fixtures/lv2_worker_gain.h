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
 * lv2_worker_gain.h — a TEST LV2 plugin whose gain reaches the audio ONLY through the LV2 worker
 * round-trip (docs/design/specs/2026-09-29-host-backend-one-contract.md §3, the worker's RT rule):
 * `run()` schedules the new dB value, `work()` converts it to linear and responds, and
 * `work_response()` is the one place the applied gain moves. It REQUIRES `urid:map`,
 * `worker:schedule` and `options` carrying `bufsz:maxBlockLength` — `instantiate` returns NULL
 * without any of them, the way a real plugin refuses — and records what the host handed it, so
 * the host's features are measured, not read off a reply. Mono: port 0 in, 1 out, 2 `gain_db`.
 */
#ifndef OMX_TEST_LV2_WORKER_GAIN_H
#define OMX_TEST_LV2_WORKER_GAIN_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#include <lv2/core/lv2.h>

#define LV2_WORKER_GAIN_URI "urn:openmixer:test:worker-gain"
enum { LV2_WORKER_GAIN_IN = 0, LV2_WORKER_GAIN_OUT = 1, LV2_WORKER_GAIN_DB = 2 };

/* What the plugin saw — written by the thread the host called it on, read by the test. */
struct lv2_worker_gain_probe {
  _Atomic unsigned instantiate_calls; /* every call, refused or not */
  _Atomic unsigned instantiated, activated, cleaned_up;
  _Atomic unsigned scheduled, works, responses;
  _Atomic unsigned over_block;     /* run() called with more frames than maxBlockLength */
  _Atomic unsigned mapped_in_work; /* urid:map answered non-zero from the worker thread */
  double rate_option;              /* param:sampleRate, as the options feature carried it */
  int32_t max_block_option;        /* bufsz:maxBlockLength */
  int32_t min_block_option;        /* bufsz:minBlockLength */
  int32_t nominal_block_option;    /* bufsz:nominalBlockLength */
  int saw_bounded;                 /* bufsz:boundedBlockLength was in the features */
  int unmap_round_trip;            /* urid:unmap gave back what urid:map was handed */
  int log_answered;                /* log:log was in the features and its printf answered */
  pthread_t work_thread;           /* the thread work() last ran on */
  pthread_t run_thread;            /* the thread run() last ran on */
};
extern struct lv2_worker_gain_probe lv2_worker_gain_probe;

const LV2_Descriptor *lv2_worker_gain_descriptor(void);

#endif /* OMX_TEST_LV2_WORKER_GAIN_H */
