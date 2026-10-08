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
 * lv2_fakes.h — the fake plugins of the console's mix_lv2.test.c, moved with it and built as the bundle
 * omx-lv2-fakes.lv2, so the LV2 host and the CLAP shim rack them through lilv like any plugin. One instance shape,
 * behaviour chosen by the URI's last digit. A test reaches the instance it racked through lv2_fakes_last, taken with
 * dlsym from the binary the host loaded (dlopen RTLD_NOLOAD on the same path).
 *
 * Ports: mono 0 in, 1 out, 2 latency; stereo 0 in L, 1 in R, 2 out L, 3 out R, 4 latency.
 */
#ifndef OMX_TEST_LV2_FAKES_H
#define OMX_TEST_LV2_FAKES_H

#include <stdint.h>

#include <lv2/core/lv2.h>
#include <lv2/worker/worker.h>

enum fake_kind
{
    K_PAD, K_BROKEN, K_CLOBBER, K_NAN, K_HOT, K_WORKER, K_NAN_ALWAYS, K_PROPS
};

#define FAKE_PAD            "urn:omx:test:pad:mono#0"
#define FAKE_PAD2           "urn:omx:test:pad:stereo#0"
#define FAKE_BROKEN         "urn:omx:test:broken:mono#1"
#define FAKE_CLOBBER        "urn:omx:test:clobber:mono#2"
#define FAKE_NAN            "urn:omx:test:nan:mono#3"
#define FAKE_HOT            "urn:omx:test:hot:mono#4"
#define FAKE_WORKER         "urn:omx:test:worker:mono#5"
#define FAKE_PROPS          "urn:omx:test:props:mono#7"           // a pad with one control port per port property
#define FAKE_NAN_ALWAYS     "urn:omx:test:nan-always:mono#6"     // non-finite from its first run: refuses the warm-up

#define PAD_GAIN            0.1f        // -20 dB, the fake's own definition
/* the fake's latency is a DURATION (1/6000 s) it reports in frames at its own rate, floored */
#define PAD_LATENCY_DIV     6000.0

struct fake
{
    enum fake_kind kind;
    const float *in[2];
    float *out[2];
    float *latency;
    uint32_t legs, runs;
    double rate;                        // what the host passed to instantiate
    float saw_in_r0;                    // the first sample the R input carried on the last run
    int nan_on;                         // K_NAN: emit NaN while set
    const LV2_Worker_Schedule *sched;
    uint32_t seq;
    LV2_Worker_Respond_Function respond;    // K_WORKER: the respond work() was last handed, and its handle
    LV2_Worker_Respond_Handle respond_handle;
    int latency_bias;                   // added to the latency the port reports (a test moves it)
    uint32_t activations, deactivations;    // LV2 activate and deactivate calls on this instance
};

/* the instance instantiated last, the trace of the worker fake, and clearing it */
typedef struct fake *(*lv2_fakes_last_fn)(void);
typedef const char *(*lv2_fakes_trace_fn)(void);
typedef void (*lv2_fakes_trace_clear_fn)(void);

static inline uint32_t pad_latency_frames(double rate)
{
    return (uint32_t)(rate / PAD_LATENCY_DIV);
}

#endif
