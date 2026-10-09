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
 * clap_stage_test.c: the CLAP stage (clap_stage.h) against closed forms, offline, with fake clap_plugin_t instances this
 * file owns. Every arm runs at each rate in RATES: the fake proves it received the rate, and its latency is a duration
 * it reports in frames at that rate.
 *
 *   tests/clap_stage_test     (built with the malloc family wrapped: make test-fake)
 *
 *  1. PAD: a -20 dB 2x2 fake reads exactly 0.1 x the input once steady, zero sample error; the
 *     control thread publishes what `latency.get()` says (rate / PAD_LATENCY_DIV frames) and the
 *     RT body only ever reads it.
 *  2. MONO MIRROR: a 2x2 pad on a mono lane sees the lane on BOTH inputs and only L comes back;
 *     a 1x1 pad on a stereo lane returns its L on both legs.
 *  3. BYPASS BIT-IDENTICAL: steady bypass leaves the lane's bytes unchanged and never calls
 *     process(); the transition block is the closed-form crossfade.
 *  4. RESET: the body never calls reset(): a steady bypass marks the stage need_reset and the
 *     re-engage is the host's (clap_core_test's reset arm).
 *  5. EVENT DRAIN: the order is drain, process, MXCSR (the fake sees its events INSIDE process()
 *     and clears FTZ there; after omx_clap_run FTZ/DAZ are set again); a ring of eventsPerBlock
 *     + 3 records is delivered over TWO blocks, in order, none lost, every field as §4.2 says;
 *     a full ring refuses the producer, never drops on the RT.
 *  6. CONSTANT CHANNEL: an output the plugin reports constant is expanded from sample 0 BEFORE
 *     the clamp — a +40 dB constant reads the ceiling on every sample, every sample counted.
 *  7. PROCESS ERROR: a CLAP_PROCESS_ERROR block leaves the lane bit-identical dry and counts; at
 *     the strike count the stage faults and stays dry after the plugin recovers.
 *  8. NON-FINITE: the same shape for a NaN block; a recovery BEFORE the strike count fades in.
 *  9. CLAMP: a +40 dB plugin at 0.5 in is held at the +24 dBFS ceiling, counted, never a strike.
 * 10. WARM-UP: the declared block count runs through start/stop_processing on the control thread
 *     holding the audio role and ends in one reset(); a non-finite plugin refuses publish; a
 *     published stage refuses.
 * 11. STATE: the RT's first block after arm calls start_processing once; a stop request is
 *     honoured by the RT (stop_processing, STOPPED) and the lane carries dry from then on.
 * 12. OUT EVENTS: the counting sink accepts everything; a PARAM_VALUE from the plugin raises
 *     plugin_changed and nothing else is stored.
 * 13. RT: no allocation inside omx_clap_run across every arm above (malloc family wrapped), and
 *     the WITNESS: a fake that allocates inside process() is seen by the same counter.
 * 13. RT: no allocation inside omx_clap_run across every arm above, and the witness: a fake that allocates inside
 *     process() is seen by the same counter.
 * 14. XFADE: omx_hosted_xfade against (1 - i/n)·from + (i/n)·to to < 1e-7; identical bodies bit-exact.
 * The tempo a run hands the plugin is clap_engine_test.c's (omx_clap_host_run).
 */
#include <clap/clap.h>
#include <clap/ext/latency.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

#include "clap_stage.h"

static const float RATES[] = {44100.0f, 48000.0f, 88200.0f, 96000.0f, 176400.0f, 192000.0f};
static float omx_db_to_lin(float db) { return omx_hosted_db_to_lin(db); }
/** The rate the arm now running activates its plugin at — set only by main's loop. */
static float g_rate;

/* ---- RT allocation witness ---- */
static volatile int in_rt = 0, rt_allocs = 0;
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);
void *__wrap_malloc(size_t n) { rt_allocs += in_rt; return __real_malloc(n); }
void *__wrap_calloc(size_t a, size_t b) { rt_allocs += in_rt; return __real_calloc(a, b); }
void *__wrap_realloc(void *p, size_t n) { rt_allocs += in_rt; return __real_realloc(p, n); }
void __wrap_free(void *p) { rt_allocs += in_rt; __real_free(p); }
void *g_sink; /* keeps a witness allocation alive past the optimiser */

static int failures = 0;
#define CHECK(cond, ...)                                   \
  do {                                                     \
    if (!(cond)) {                                         \
      failures++;                                          \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
      fprintf(stderr, __VA_ARGS__);                        \
      fprintf(stderr, "\n");                               \
    }                                                      \
  } while (0)

/* ---- the fake plugins: one instance shape, behaviour chosen by kind ---- */
enum kind { K_PAD, K_CLOBBER, K_NAN, K_HOT, K_ERROR, K_CONSTANT, K_ALLOC, K_TALK };
struct fake {
  clap_plugin_t plugin; /* first, so plugin_data may simply be the fake */
  enum kind kind;
  uint32_t channels;
  double rate;
  uint32_t runs, starts, stops, resets;
  float saw_in_r0;      /* the first sample the R input carried on the last process */
  int nan_on, error_on; /* K_NAN / K_ERROR: misbehave while set */
  int ftz_inside;       /* K_CLOBBER: whether FTZ was set when process() began */
  /* K_TALK: every param event seen, in order, with the fields §4.2 fixes */
  uint32_t ev_count;
  clap_id ev_id[256];
  double ev_value[256];
  void *ev_cookie[256];
  int ev_fields_ok;
  uint32_t ev_per_block[8];
  uint32_t blocks;
  const clap_output_events_t *out; /* K_TALK pushes one PARAM_VALUE back */
  /* the transport the last process() was given, as the plugin reads it */
  int saw_transport;     /* 1: a transport was passed, 0: NULL */
  double saw_tempo;      /* its tempo, when it carried CLAP_TRANSPORT_HAS_TEMPO; else 0 */
  uint32_t saw_tflags;   /* its flags */
  int saw_theader_ok;    /* size, space, type and time 0 as CLAP fixes them */
};

#define PAD_GAIN 0.1f /* -20 dB, the fake's own definition */
/* The fake's latency is a DURATION (1/6000 s) it reports in frames at its own rate, floored. */
#define PAD_LATENCY_DIV 6000.0
static uint32_t pad_latency_frames(double rate) { return (uint32_t)(rate / PAD_LATENCY_DIV); }

static bool f_init(const clap_plugin_t *p) { (void)p; return true; }
static void f_destroy(const clap_plugin_t *p) { (void)p; }
static bool f_activate(const clap_plugin_t *p, double rate, uint32_t min_f, uint32_t max_f) {
  (void)min_f, (void)max_f;
  ((struct fake *)p->plugin_data)->rate = rate;
  return true;
}
static void f_deactivate(const clap_plugin_t *p) { (void)p; }
static bool f_start(const clap_plugin_t *p) { ((struct fake *)p->plugin_data)->starts++; return true; }
static void f_stop(const clap_plugin_t *p) { ((struct fake *)p->plugin_data)->stops++; }
static void f_reset(const clap_plugin_t *p) { ((struct fake *)p->plugin_data)->resets++; }

static clap_process_status f_process(const clap_plugin_t *p, const clap_process_t *pr) {
  struct fake *f = (struct fake *)p->plugin_data;
  f->runs++;
#if defined(__x86_64__) || defined(__i386__)
  f->ftz_inside = (_mm_getcsr() & 0x8040u) == 0x8040u;
  if (f->kind == K_CLOBBER) _mm_setcsr(_mm_getcsr() & ~0x8040u);
#endif
  const clap_audio_buffer_t *ai = &pr->audio_inputs[0];
  clap_audio_buffer_t *ao = &pr->audio_outputs[0];
  const uint32_t n = pr->frames_count;
  if (f->channels == 2) f->saw_in_r0 = ai->data32[1][0];
  /* the transport, as delivered: what a tempo-synced plugin reads */
  f->saw_transport = pr->transport != NULL;
  f->saw_tempo = pr->transport && (pr->transport->flags & CLAP_TRANSPORT_HAS_TEMPO) ? pr->transport->tempo : 0.0;
  f->saw_tflags = pr->transport ? pr->transport->flags : 0u;
  f->saw_theader_ok = pr->transport && pr->transport->header.size == sizeof *pr->transport &&
                      pr->transport->header.space_id == CLAP_CORE_EVENT_SPACE_ID &&
                      pr->transport->header.type == CLAP_EVENT_TRANSPORT && pr->transport->header.time == 0;
  /* the events, as delivered */
  const uint32_t ne = pr->in_events->size(pr->in_events);
  if (f->blocks < 8) f->ev_per_block[f->blocks] = ne;
  f->blocks++;
  for (uint32_t k = 0; k < ne; k++) {
    const clap_event_header_t *h = pr->in_events->get(pr->in_events, k);
    if (h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE) continue;
    const clap_event_param_value_t *e = (const clap_event_param_value_t *)h;
    if (f->ev_count < 256) {
      f->ev_id[f->ev_count] = e->param_id;
      f->ev_value[f->ev_count] = e->value;
      f->ev_cookie[f->ev_count] = e->cookie;
    }
    f->ev_count++;
    if (h->size != sizeof *e || h->time != 0 || e->note_id != -1 || e->port_index != -1 ||
        e->channel != -1 || e->key != -1)
      f->ev_fields_ok = 0;
  }
  if (f->kind == K_ERROR && f->error_on) return CLAP_PROCESS_ERROR;
  if (f->kind == K_ALLOC) g_sink = malloc(8);
  for (uint32_t c = 0; c < f->channels; c++) {
    const float *in = ai->data32[c];
    float *out = ao->data32[c];
    switch (f->kind) {
      case K_NAN:
        for (uint32_t i = 0; i < n; i++) out[i] = f->nan_on ? NAN : in[i] * PAD_GAIN;
        break;
      case K_HOT:
        for (uint32_t i = 0; i < n; i++) out[i] = in[i] * 100.0f; /* +40 dB */
        break;
      case K_CONSTANT:
        /* only sample 0 is valid — the rest is deliberately garbage the host must never read */
        out[0] = 100.0f;
        for (uint32_t i = 1; i < n; i++) out[i] = NAN;
        ao->constant_mask |= (uint64_t)1 << c;
        break;
      default:
        for (uint32_t i = 0; i < n; i++) out[i] = in[i] * PAD_GAIN;
    }
  }
  if (f->kind == K_TALK && pr->out_events) {
    clap_event_param_value_t e;
    memset(&e, 0, sizeof e);
    e.header.size = sizeof e;
    e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e.header.type = CLAP_EVENT_PARAM_VALUE;
    e.param_id = 7;
    e.value = 0.5;
    pr->out_events->try_push(pr->out_events, &e.header);
    clap_event_header_t other = {sizeof other, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
    pr->out_events->try_push(pr->out_events, &other);
  }
  return f->kind == K_TALK ? CLAP_PROCESS_TAIL : CLAP_PROCESS_CONTINUE;
}

static uint32_t f_latency(const clap_plugin_t *p) {
  return pad_latency_frames(((struct fake *)p->plugin_data)->rate);
}
static const clap_plugin_latency_t LATENCY_EXT = {f_latency};
static const void *f_get_extension(const clap_plugin_t *p, const char *id) {
  (void)p;
  return strcmp(id, CLAP_EXT_LATENCY) == 0 ? &LATENCY_EXT : NULL;
}
static void f_on_main(const clap_plugin_t *p) { (void)p; }

static const clap_plugin_descriptor_t DESC = {
    CLAP_VERSION_INIT, "urn:omx:test:pad", "Test Pad", "omx-clap-host", "", "", "", "0", "a fake", NULL};

static void fake_up(struct fake *f, enum kind kind, uint32_t channels) {
  memset(f, 0, sizeof *f);
  f->kind = kind;
  f->channels = channels;
  f->ev_fields_ok = 1;
  f->plugin.desc = &DESC;
  f->plugin.plugin_data = f;
  f->plugin.init = f_init;
  f->plugin.destroy = f_destroy;
  f->plugin.activate = f_activate;
  f->plugin.deactivate = f_deactivate;
  f->plugin.start_processing = f_start;
  f->plugin.stop_processing = f_stop;
  f->plugin.reset = f_reset;
  f->plugin.process = f_process;
  f->plugin.get_extension = f_get_extension;
  f->plugin.on_main_thread = f_on_main;
}

/* ---- the host's half, reduced to what the test needs ---- */
#define MAXB 256u
#define QCAP 8u /* a small ring so "full" is reachable; the declared depth is the host's */
struct rig {
  struct omx_clap_stage st;
  float in_l[MAXB], in_r[MAXB], out_l[MAXB], out_r[MAXB];
  struct omx_clap_param_record recs[CLAP_HOST_PARAM_QUEUE_DEPTH];
  struct fake f;
};

/** Init, activate at g_rate, bind, publish the latency the ext reports — the control thread's
 *  part of §5 — and ARM; the first omx_clap_run then takes the audio role. */
static void rig_up(struct rig *g, enum kind kind, uint32_t channels, uint32_t qcap) {
  memset(g, 0, sizeof *g);
  fake_up(&g->f, kind, channels);
  int ok = omx_clap_stage_init(&g->st, &(struct omx_hosted_bounce){g->in_l, g->in_r, g->out_l, g->out_r, MAXB}, g->recs, qcap);
  CHECK(ok == 0, "stage_init");
  CHECK(g->f.plugin.init(&g->f.plugin), "init");
  CHECK(g->f.plugin.activate(&g->f.plugin, (double)g_rate, 1, MAXB), "activate");
  CHECK(g->f.rate == (double)g_rate, "the plugin was activated at %.0f, not %.0f", g->f.rate, (double)g_rate);
  ok = omx_clap_bind(&g->st, &g->f.plugin, channels);
  CHECK(ok == 0, "bind");
  const clap_plugin_latency_t *lat = g->f.plugin.get_extension(&g->f.plugin, CLAP_EXT_LATENCY);
  omx_clap_publish_latency(&g->st, lat ? lat->get(&g->f.plugin) : 0);
  omx_clap_arm(&g->st);
}

static void fill(float *b, uint32_t n, float v) { for (uint32_t i = 0; i < n; i++) b[i] = v; }
static void ramp(float *b, uint32_t n) { for (uint32_t i = 0; i < n; i++) b[i] = 0.5f * (float)i / (float)n; }

/** The RT call, under the allocation witness. */
static void run(struct rig *g, float *l, float *r, uint32_t n) {
  in_rt = 1;
  omx_clap_run(&g->st, l, r, n);
  in_rt = 0;
}


static int bytes_equal(const float *a, const float *b, uint32_t n) { return memcmp(a, b, n * sizeof(float)) == 0; }

/* ---- 1. PAD, and the published latency ---- */
static void t_pad(void) {
  struct rig g;
  rig_up(&g, K_PAD, 2, QCAP);
  float l[MAXB], r[MAXB], dl[MAXB], dr[MAXB];
  ramp(l, 128), ramp(r, 128);
  memcpy(dl, l, sizeof l), memcpy(dr, r, sizeof r);
  run(&g, l, r, 128); /* the first block fades in from dry */
  ramp(l, 128), ramp(r, 128);
  run(&g, l, r, 128); /* steady: exactly the pad */
  int exact = 1;
  for (uint32_t i = 0; i < 128; i++) exact &= l[i] == dl[i] * PAD_GAIN && r[i] == dr[i] * PAD_GAIN;
  CHECK(exact, "steady pad is exactly 0.1 x in, zero sample error");
  const uint32_t want = pad_latency_frames((double)g_rate);
  CHECK(atomic_load(&g.st.latency_frames) == want, "latency published: %u frames at %.0f (%u)",
        atomic_load(&g.st.latency_frames), (double)g_rate, want);
  CHECK(g.f.starts == 1, "start_processing called once by the RT's first block (%u)", g.f.starts);
  CHECK(atomic_load(&g.st.state) == OMX_CLAP_PROCESSING, "state PROCESSING");
  CHECK(atomic_load(&g.st.h.runs) == 2, "two live runs");
  printf("pad @ %.0f: -%.5f dB steady, latency %u frames\n", (double)g_rate,
         -20.0 * log10((double)l[64] / (double)dl[64]), want);
}

/* ---- 2. MONO MIRROR ---- */
static void t_mono(void) {
  struct rig g;
  rig_up(&g, K_PAD, 2, QCAP);
  float l[MAXB], keep[MAXB];
  fill(l, 64, 0.25f);
  run(&g, l, NULL, 64);
  CHECK(g.f.saw_in_r0 == 0.25f, "a stereo plugin's R input carries the mono lane (%g)", g.f.saw_in_r0);
  fill(l, 64, 0.25f);
  run(&g, l, NULL, 64);
  CHECK(l[10] == 0.025f, "only L comes back on a mono lane (%g)", l[10]);
  memcpy(keep, g.out_r, sizeof keep);
  (void)keep;

  struct rig m;
  rig_up(&m, K_PAD, 1, QCAP);
  float sl[MAXB], sr[MAXB];
  fill(sl, 64, 0.5f), fill(sr, 64, -0.5f);
  run(&m, sl, sr, 64);
  fill(sl, 64, 0.5f), fill(sr, 64, -0.5f);
  run(&m, sl, sr, 64);
  CHECK(sl[5] == 0.05f && sr[5] == 0.05f, "a mono plugin on a stereo lane returns L on both legs (%g, %g)", sl[5], sr[5]);
}

/* ---- 3. BYPASS bit-identical, 4. RESET on re-engage ---- */
static void t_bypass_reset(void) {
  struct rig g;
  rig_up(&g, K_PAD, 2, QCAP);
  float l[MAXB], r[MAXB], dl[MAXB], dr[MAXB];
  ramp(l, 100), ramp(r, 100);
  run(&g, l, r, 100);
  CHECK(g.f.resets == 0, "no reset on the first block after publish (%u)", g.f.resets);
  ramp(l, 100), ramp(r, 100);
  run(&g, l, r, 100); /* steady wet */
  omx_clap_set_bypass(&g.st, 1);
  ramp(l, 100), ramp(r, 100);
  memcpy(dl, l, sizeof dl), memcpy(dr, r, sizeof dr);
  run(&g, l, r, 100); /* the transition: wet -> dry crossfade */
  float want[MAXB];
  float wet[MAXB];
  for (uint32_t i = 0; i < 100; i++) wet[i] = dl[i] * PAD_GAIN;
  omx_hosted_xfade(want, wet, dl, 100);
  CHECK(bytes_equal(l, want, 100), "the bypass transition is the closed-form crossfade wet->dry");
  const uint32_t runs_before = atomic_load(&g.st.h.runs);
  ramp(l, 100), ramp(r, 100);
  run(&g, l, r, 100); /* steady bypass */
  CHECK(bytes_equal(l, dl, 100) && bytes_equal(r, dr, 100), "steady bypass is bit-identical dry");
  CHECK(atomic_load(&g.st.h.runs) == runs_before, "steady bypass never calls process()");
  CHECK(g.f.resets == 0, "no reset while bypassed");
  CHECK(g.st.need_reset == 1, "the steady bypass marked the stage need_reset");
  omx_clap_set_bypass(&g.st, 0);
  ramp(l, 100), ramp(r, 100);
  run(&g, l, r, 100); /* re-engage at the stage: process(), fade in; the reset is the host's (clap_core_test) */
  ramp(l, 100), ramp(r, 100);
  run(&g, l, r, 100);
  CHECK(g.f.resets == 0 && atomic_load(&g.st.resets) == 0, "the body never calls reset() (%u)", g.f.resets);
  CHECK(l[50] == dl[50] * PAD_GAIN, "wet again after re-engage");
}

/* ---- 5. EVENT DRAIN: order, fields, two blocks, a full ring ---- */
static void t_events(void) {
  struct rig g;
  rig_up(&g, K_CLOBBER, 2, CLAP_HOST_PARAM_QUEUE_DEPTH);
  int cookie_a, cookie_b;
  const uint32_t total = CLAP_HOST_EVENTS_PER_BLOCK + 3u;
  for (uint32_t k = 0; k < total; k++)
    CHECK(omx_clap_param_push(&g.st, (clap_id)(100 + k), (double)k * 0.25, (k & 1) ? &cookie_a : &cookie_b) == 0,
          "push %u accepted", k);
  CHECK(omx_clap_queue_pending(&g.st.queue) == total, "%u records pending", total);
  float l[MAXB], r[MAXB];
  fill(l, 64, 0.1f), fill(r, 64, 0.1f);
#if defined(__x86_64__) || defined(__i386__)
  omx_hosted_denormals_off();
#endif
  run(&g, l, r, 64);
  CHECK(g.f.ev_per_block[0] == CLAP_HOST_EVENTS_PER_BLOCK, "block 1 carried eventsPerBlock (%u)", g.f.ev_per_block[0]);
  CHECK(omx_clap_queue_pending(&g.st.queue) == 3, "the surplus stays in the ring (%u)", omx_clap_queue_pending(&g.st.queue));
#if defined(__x86_64__) || defined(__i386__)
  CHECK(g.f.ftz_inside, "FTZ/DAZ were set when process() began");
  CHECK((_mm_getcsr() & 0x8040u) == 0x8040u, "FTZ/DAZ re-asserted after a plugin that cleared them");
#endif
  fill(l, 64, 0.1f), fill(r, 64, 0.1f);
  run(&g, l, r, 64);
  CHECK(g.f.ev_per_block[1] == 3, "block 2 carried the 3 left over (%u)", g.f.ev_per_block[1]);
  CHECK(g.f.ev_count == total, "none lost: %u delivered (%u)", g.f.ev_count, total);
  int in_order = 1;
  for (uint32_t k = 0; k < total; k++)
    in_order &= g.f.ev_id[k] == 100 + k && g.f.ev_value[k] == (double)k * 0.25 &&
                g.f.ev_cookie[k] == ((k & 1) ? (void *)&cookie_a : (void *)&cookie_b);
  CHECK(in_order, "delivered in production order with id, value and cookie intact");
  CHECK(g.f.ev_fields_ok, "time 0, note_id/port_index/channel/key -1, size right on every event");
  CHECK(atomic_load(&g.st.events_delivered) == total, "the stage counted %u", total);
  fill(l, 64, 0.1f), fill(r, 64, 0.1f);
  run(&g, l, r, 64);
  CHECK(g.f.ev_per_block[2] == 0, "an empty ring delivers an empty list");

  /* a small ring is FULL at its capacity and refuses the producer */
  struct rig s;
  rig_up(&s, K_PAD, 2, QCAP);
  for (uint32_t k = 0; k < QCAP; k++) CHECK(omx_clap_param_push(&s.st, k, 1.0, NULL) == 0, "push into a free slot");
  CHECK(omx_clap_param_push(&s.st, 99, 1.0, NULL) == -1, "a full ring refuses the producer");
  run(&s, l, r, 64);
  CHECK(omx_clap_param_push(&s.st, 99, 1.0, NULL) == 0, "and takes it once the RT drained");
}

/* ---- 6. CONSTANT channel expanded before the clamp ---- */
static void t_constant(void) {
  struct rig g;
  rig_up(&g, K_CONSTANT, 2, QCAP);
  float l[MAXB], r[MAXB];
  fill(l, 64, 0.5f), fill(r, 64, 0.5f);
  run(&g, l, r, 64); /* fade in */
  fill(l, 64, 0.5f), fill(r, 64, 0.5f);
  run(&g, l, r, 64);
  int all = 1;
  for (uint32_t i = 0; i < 64; i++) all &= l[i] == omx_db_to_lin(CLAP_HOST_CLAMP_DBFS) && r[i] == omx_db_to_lin(CLAP_HOST_CLAMP_DBFS);
  CHECK(all, "a constant +40 dB block reads the ceiling on EVERY sample: expanded before the clamp");
  CHECK(atomic_load(&g.st.h.nonfinite_blocks) == 0, "the garbage past sample 0 was never scanned");
  CHECK(atomic_load(&g.st.constant_channels) == 4, "two channels x two blocks expanded (%u)", atomic_load(&g.st.constant_channels));
  CHECK(atomic_load(&g.st.h.clamped_samples) == 256, "every sample of both blocks counted clamped (%u)", atomic_load(&g.st.h.clamped_samples));
}

/* ---- 7. PROCESS ERROR / 8. NON-FINITE: discard, strike, fault ---- */
static void t_discard(enum kind kind) {
  struct rig g;
  rig_up(&g, kind, 2, QCAP);
  float l[MAXB], r[MAXB], dl[MAXB], dr[MAXB];
  ramp(l, 64), ramp(r, 64);
  run(&g, l, r, 64);
  ramp(l, 64), ramp(r, 64);
  run(&g, l, r, 64); /* steady wet */
  int *bad = kind == K_ERROR ? &g.f.error_on : &g.f.nan_on;
  *bad = 1;
  ramp(l, 64), ramp(r, 64);
  memcpy(dl, l, sizeof dl), memcpy(dr, r, sizeof dr);
  run(&g, l, r, 64);
  CHECK(bytes_equal(l, dl, 64) && bytes_equal(r, dr, 64), "%s: the lane is bit-identical dry", kind == K_ERROR ? "error" : "nan");
  CHECK(atomic_load(&g.st.h.nonfinite_blocks) == 1, "one strike");
  if (kind == K_ERROR) CHECK(atomic_load(&g.st.process_errors) == 1, "counted as a process error too");
  CHECK(atomic_load(&g.st.h.fault) == OMX_HOSTED_FAULT_NONE, "not yet faulted");
  *bad = 0;
  ramp(l, 64), ramp(r, 64);
  run(&g, l, r, 64); /* a recovery before the strike count fades back in */
  float want[MAXB], wet[MAXB];
  for (uint32_t i = 0; i < 64; i++) wet[i] = dl[i] * PAD_GAIN;
  omx_hosted_xfade(want, dl, wet, 64);
  CHECK(bytes_equal(l, want, 64), "a recovery fades back in from dry");
  *bad = 1;
  for (uint32_t k = 1; k < CLAP_HOST_NON_FINITE_STRIKES; k++) {
    ramp(l, 64), ramp(r, 64);
    run(&g, l, r, 64);
  }
  CHECK(atomic_load(&g.st.h.fault) == OMX_HOSTED_FAULT_NONFINITE, "faulted at the strike count (%u)", CLAP_HOST_NON_FINITE_STRIKES);
  *bad = 0;
  const uint32_t runs = atomic_load(&g.st.h.runs);
  ramp(l, 64), ramp(r, 64);
  memcpy(dl, l, sizeof dl);
  run(&g, l, r, 64);
  run(&g, l, r, 64);
  CHECK(bytes_equal(l, dl, 64), "a faulted stage stays dry after the plugin recovers");
  CHECK(atomic_load(&g.st.h.runs) == runs, "and never runs it again");
}

/* ---- 9. CLAMP ---- */
static void t_clamp(void) {
  struct rig g;
  rig_up(&g, K_HOT, 2, QCAP);
  float l[MAXB], r[MAXB];
  for (int pass = 0; pass < 2; pass++) {
    for (uint32_t i = 0; i < 64; i++) l[i] = r[i] = (i & 1) ? -0.5f : 0.5f;
    run(&g, l, r, 64);
  }
  int all = 1;
  for (uint32_t i = 0; i < 64; i++)
    all &= l[i] == ((i & 1) ? -omx_db_to_lin(CLAP_HOST_CLAMP_DBFS) : omx_db_to_lin(CLAP_HOST_CLAMP_DBFS));
  CHECK(all, "every +40 dB sample held at the +%.1f dBFS ceiling", (double)CLAP_HOST_CLAMP_DBFS);
  CHECK(atomic_load(&g.st.h.clamped_samples) == 256, "counted (%u)", atomic_load(&g.st.h.clamped_samples));
  CHECK(atomic_load(&g.st.h.nonfinite_blocks) == 0, "never a strike");
}

/* ---- 10. WARM-UP ---- */
static void t_prime(void) {
  struct rig g;
  memset(&g, 0, sizeof g);
  fake_up(&g.f, K_PAD, 2);
  omx_clap_stage_init(&g.st, &(struct omx_hosted_bounce){g.in_l, g.in_r, g.out_l, g.out_r, MAXB}, g.recs, QCAP);
  g.f.plugin.activate(&g.f.plugin, (double)g_rate, 1, MAXB);
  omx_clap_bind(&g.st, &g.f.plugin, 2);
  CHECK(omx_clap_prime(&g.st, 128) == 0, "a clean plugin warms up with no bad block");
  CHECK(g.f.runs == CLAP_HOST_WARMUP_BLOCKS, "warm-up ran %u blocks (%u)", CLAP_HOST_WARMUP_BLOCKS, g.f.runs);
  CHECK(g.f.starts == 1 && g.f.stops == 1, "the control thread held the audio role: start/stop_processing once each");
  CHECK(g.f.resets == 1, "the warm-up ends in ONE reset(), so no tail longer than its silent half survives (%u)", g.f.resets);
  CHECK(atomic_load(&g.st.h.runs) == 0, "the live counter is untouched");
  CHECK(g.st.steady_time == (int64_t)CLAP_HOST_WARMUP_BLOCKS * 128, "steady_time advanced through the warm-up");
  omx_clap_arm(&g.st);
  CHECK(omx_clap_prime(&g.st, 128) == UINT32_MAX, "a published stage refuses to warm up");

  struct rig b;
  memset(&b, 0, sizeof b);
  fake_up(&b.f, K_NAN, 2);
  b.f.nan_on = 1;
  omx_clap_stage_init(&b.st, &(struct omx_hosted_bounce){b.in_l, b.in_r, b.out_l, b.out_r, MAXB}, b.recs, QCAP);
  b.f.plugin.activate(&b.f.plugin, (double)g_rate, 1, MAXB);
  omx_clap_bind(&b.st, &b.f.plugin, 2);
  CHECK(omx_clap_prime(&b.st, 128) == CLAP_HOST_WARMUP_BLOCKS, "a non-finite plugin refuses publish");
}

/* ---- 11. STATE: stop request honoured by the RT ---- */
static void t_state(void) {
  struct rig g;
  rig_up(&g, K_PAD, 2, QCAP);
  float l[MAXB], r[MAXB], dl[MAXB];
  ramp(l, 64), ramp(r, 64);
  run(&g, l, r, 64);
  ramp(l, 64), ramp(r, 64);
  run(&g, l, r, 64);
  omx_clap_request_stop(&g.st);
  CHECK(atomic_load(&g.st.state) == OMX_CLAP_STOPPING, "the control thread asked");
  CHECK(!omx_clap_stopped(&g.st), "not stopped until the RT says so");
  ramp(l, 64), ramp(r, 64);
  memcpy(dl, l, sizeof dl);
  run(&g, l, r, 64); /* the RT stops: one fade-out, then dry */
  CHECK(g.f.stops == 1, "stop_processing called once by the RT (%u)", g.f.stops);
  CHECK(omx_clap_stopped(&g.st) && atomic_load(&g.st.state) == OMX_CLAP_STOPPED, "STOPPED published");
  ramp(l, 64), ramp(r, 64);
  run(&g, l, r, 64);
  CHECK(bytes_equal(l, dl, 64), "the lane carries dry after the stop");
  CHECK(g.f.runs == 2, "process() never again (%u)", g.f.runs);

  struct rig a;
  rig_up(&a, K_PAD, 2, QCAP);
  omx_clap_request_stop(&a.st);
  CHECK(omx_clap_stopped(&a.st) && a.f.stops == 0, "armed and never run: stopped at once, nothing to stop");
}

/* ---- 12. OUT EVENTS ---- */
static void t_out_events(void) {
  struct rig g;
  rig_up(&g, K_TALK, 2, QCAP);
  float l[MAXB], r[MAXB];
  fill(l, 64, 0.1f), fill(r, 64, 0.1f);
  run(&g, l, r, 64);
  CHECK(atomic_load(&g.st.out_events_seen) == 2, "both pushes accepted and counted (%u)", atomic_load(&g.st.out_events_seen));
  CHECK(atomic_load(&g.st.plugin_changed) == 1, "a PARAM_VALUE from the plugin raises plugin_changed");
  CHECK(atomic_load(&g.st.h.runs) == 1 && atomic_load(&g.st.h.fault) == 0, "TAIL is honoured as CONTINUE");
}

/* ---- 14. XFADE closed form ---- */
static void t_xfade(void) {
  float from[MAXB], to[MAXB], dst[MAXB];
  for (uint32_t i = 0; i < 128; i++) from[i] = 0.7f - 0.01f * (float)i, to[i] = -0.3f + 0.005f * (float)i;
  omx_hosted_xfade(dst, from, to, 128);
  double worst = 0;
  for (uint32_t i = 0; i < 128; i++) {
    const double g = (double)i / 128.0;
    const double want = (1.0 - g) * from[i] + g * to[i];
    const double err = fabs((double)dst[i] - want);
    if (err > worst) worst = err;
  }
  CHECK(worst < 1e-7, "crossfade matches (1-i/n)from + (i/n)to to < 1e-7 (worst %.3g)", worst);
  omx_hosted_xfade(dst, from, from, 128);
  CHECK(bytes_equal(dst, from, 128), "identical bodies read back bit-identical");
}

/* ---- 13. the allocation witness ---- */
static void t_alloc_witness(void) {
  struct rig g;
  rig_up(&g, K_ALLOC, 2, QCAP);
  float l[MAXB], r[MAXB];
  fill(l, 64, 0.1f), fill(r, 64, 0.1f);
  const int before = rt_allocs;
  run(&g, l, r, 64);
  CHECK(rt_allocs > before, "the witness: an allocation inside process() IS seen (%d)", rt_allocs - before);
  rt_allocs = before; /* the witness's own count is not the stage's */
}

/* ---- 9. CHANNEL MESSAGES: what each kind of message becomes, by the dialect the note input wants and the dialects its port declares ---- */
static struct omx_clap_stage NS;

static void ns_up(uint32_t inputs, uint32_t dialect, uint32_t dialects) {
  memset(&NS, 0, sizeof NS);
  NS.note_inputs = inputs;
  NS.note_dialect = dialect;
  NS.note_dialects = dialects;
  NS.bend_semitones = OMX_CLAP_BEND_SEMITONES_DEFAULT;
}

static void ns_in(uint32_t time, const uint8_t *m, size_t n) { omx_clap_note_in(&NS, time, m, n); }

static void t_channel_messages(void) {
  static const uint8_t NOTE_ON[] = {0x93, 60, 100}, NOTE_OFF[] = {0x80, 60, 0}, ON_VEL0[] = {0x92, 60, 0};
  static const uint8_t CTRL[] = {0xb0, 7, 100}, PROG[] = {0xc1, 5}, BEND_MIN[] = {0xe5, 0x00, 0x00};
  static const uint8_t BEND_MID[] = {0xe5, 0x00, 0x40}, BEND_MAX[] = {0xe5, 0x7f, 0x7f}, CHAN_PRESS[] = {0xd2, 127};
  static const uint8_t POLY[] = {0xa4, 61, 64}, CLOCK[] = {0xf8}, LONG[] = {0x90, 60, 100, 1}, SYSEX[] = {0xf0, 1, 2, 3, 0xf7};
  uint8_t big[OMX_CLAP_SYSEX_MAX_BYTES + 1];
  uint32_t k;

  /* a CLAP-dialect input whose port declares only CLAP: notes, and pitch bend and pressure as expressions */
  ns_up(1, CLAP_NOTE_DIALECT_CLAP, CLAP_NOTE_DIALECT_CLAP);
  ns_in(10, NOTE_ON, sizeof NOTE_ON);
  CHECK(NS.n_notes == 1 && NS.notes[0].header.type == CLAP_EVENT_NOTE_ON && NS.notes[0].header.time == 10,
        "CLAP: a note on is a CLAP note on at its frame (%u)", NS.n_notes);
  CHECK(NS.notes[0].note.channel == 3 && NS.notes[0].note.key == 60 && NS.notes[0].note.velocity == 100.0 / 127.0,
        "CLAP: the note on carries channel 3, key 60, velocity 100/127");
  ns_in(11, NOTE_OFF, sizeof NOTE_OFF);
  ns_in(12, ON_VEL0, sizeof ON_VEL0);
  CHECK(NS.n_notes == 3 && NS.notes[1].header.type == CLAP_EVENT_NOTE_OFF && NS.notes[2].header.type == CLAP_EVENT_NOTE_OFF,
        "CLAP: a note off and a note on with velocity 0 are both CLAP note offs");

  ns_up(1, CLAP_NOTE_DIALECT_CLAP, CLAP_NOTE_DIALECT_CLAP);
  ns_in(10, BEND_MIN, sizeof BEND_MIN);
  CHECK(NS.n_notes == 1 && NS.notes[0].header.type == CLAP_EVENT_NOTE_EXPRESSION, "CLAP: pitch bend is a note expression (%u)", NS.n_notes);
  CHECK(NS.notes[0].expr.expression_id == CLAP_NOTE_EXPRESSION_TUNING && NS.notes[0].expr.channel == 5 && NS.notes[0].expr.key == -1,
        "CLAP: the bend is tuning on channel 5, the whole channel");
  CHECK(NS.notes[0].expr.value == -2.0, "CLAP: the bottom of the bend is -2 semitones (%g)", NS.notes[0].expr.value);
  ns_up(1, CLAP_NOTE_DIALECT_CLAP, CLAP_NOTE_DIALECT_CLAP);
  ns_in(10, BEND_MID, sizeof BEND_MID);
  CHECK(NS.notes[0].expr.value == 0.0, "CLAP: the centre of the bend is no tuning (%g)", NS.notes[0].expr.value);
  ns_up(1, CLAP_NOTE_DIALECT_CLAP, CLAP_NOTE_DIALECT_CLAP);
  ns_in(10, BEND_MAX, sizeof BEND_MAX);
  CHECK(NS.notes[0].expr.value == 8191.0 / 8192.0 * 2.0, "CLAP: the top of the bend is 8191/8192 x 2 semitones (%g)", NS.notes[0].expr.value);

  /* a host-set range: full scale is the range, to the bit */
  ns_up(1, CLAP_NOTE_DIALECT_CLAP, CLAP_NOTE_DIALECT_CLAP);
  NS.bend_semitones = 12.0;
  ns_in(10, BEND_MAX, sizeof BEND_MAX);
  ns_in(11, BEND_MIN, sizeof BEND_MIN);
  CHECK(NS.notes[0].expr.value == 8191.0 / 8192.0 * 12.0 && NS.notes[1].expr.value == -12.0,
        "CLAP: with a 12 semitone range the bend spans -12 .. 8191/8192 x 12 (%g, %g)", NS.notes[0].expr.value, NS.notes[1].expr.value);

  ns_up(1, CLAP_NOTE_DIALECT_CLAP, CLAP_NOTE_DIALECT_CLAP);
  ns_in(10, CHAN_PRESS, sizeof CHAN_PRESS);
  CHECK(NS.n_notes == 1 && NS.notes[0].expr.expression_id == CLAP_NOTE_EXPRESSION_PRESSURE && NS.notes[0].expr.channel == 2 &&
        NS.notes[0].expr.key == -1 && NS.notes[0].expr.value == 1.0,
        "CLAP: channel pressure 127 is pressure 1.0 on channel 2, the whole channel");
  ns_up(1, CLAP_NOTE_DIALECT_CLAP, CLAP_NOTE_DIALECT_CLAP);
  ns_in(10, POLY, sizeof POLY);
  CHECK(NS.n_notes == 1 && NS.notes[0].expr.expression_id == CLAP_NOTE_EXPRESSION_PRESSURE && NS.notes[0].expr.channel == 4 &&
        NS.notes[0].expr.key == 61 && NS.notes[0].expr.value == 64.0 / 127.0,
        "CLAP: poly aftertouch is pressure on key 61 of channel 4");

  /* what no CLAP expression carries is counted, a clock is not ours, and a sysex a CLAP-only port cannot take is counted */
  ns_up(1, CLAP_NOTE_DIALECT_CLAP, CLAP_NOTE_DIALECT_CLAP);
  ns_in(10, CTRL, sizeof CTRL);
  ns_in(10, PROG, sizeof PROG);
  ns_in(10, CLOCK, sizeof CLOCK);
  ns_in(10, SYSEX, sizeof SYSEX);
  CHECK(NS.n_notes == 0 && atomic_load(&NS.notes_unmappable) == 3 && NS.n_sysex == 0,
        "CLAP only: a controller, a program change and a sysex are counted, none forwarded (%u notes, %u unmappable)", NS.n_notes,
        atomic_load(&NS.notes_unmappable));

  /* a channel message longer than three bytes is counted whatever the dialect, never forwarded */
  ns_up(1, CLAP_NOTE_DIALECT_CLAP, CLAP_NOTE_DIALECT_CLAP);
  ns_in(10, LONG, sizeof LONG);
  CHECK(NS.n_notes == 0 && atomic_load(&NS.notes_unmappable) == 1, "CLAP: a 4-byte channel message is counted (%u)", atomic_load(&NS.notes_unmappable));
  ns_up(1, CLAP_NOTE_DIALECT_MIDI, CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_CLAP);
  ns_in(10, LONG, sizeof LONG);
  CHECK(NS.n_notes == 0 && atomic_load(&NS.notes_unmappable) == 1, "MIDI: a 4-byte channel message is counted (%u)", atomic_load(&NS.notes_unmappable));

  /* a port that also declares MIDI: every channel message and sysex goes as CLAP_EVENT_MIDI, notes stay CLAP notes */
  ns_up(1, CLAP_NOTE_DIALECT_CLAP, CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI);
  ns_in(10, CTRL, sizeof CTRL);
  CHECK(NS.n_notes == 1 && NS.notes[0].header.type == CLAP_EVENT_MIDI && NS.notes[0].midi.data[0] == 0xb0 &&
        NS.notes[0].midi.data[1] == 7 && NS.notes[0].midi.data[2] == 100 && NS.notes[0].midi.port_index == 0 && NS.notes[0].header.time == 10,
        "CLAP+MIDI: a controller reaches the input as CLAP_EVENT_MIDI, bytes and frame intact");
  ns_in(11, PROG, sizeof PROG);
  CHECK(NS.notes[1].header.type == CLAP_EVENT_MIDI && NS.notes[1].midi.data[0] == 0xc1 && NS.notes[1].midi.data[1] == 5 &&
        NS.notes[1].midi.data[2] == 0, "CLAP+MIDI: a program change keeps its one data byte, the second zero");
  ns_in(12, BEND_MID, sizeof BEND_MID);
  CHECK(NS.notes[2].header.type == CLAP_EVENT_MIDI && NS.notes[2].midi.data[0] == 0xe5 && NS.notes[2].midi.data[2] == 0x40,
        "CLAP+MIDI: a pitch bend is raw MIDI, not an expression");
  ns_in(13, CHAN_PRESS, sizeof CHAN_PRESS);
  ns_in(14, POLY, sizeof POLY);
  CHECK(NS.n_notes == 5 && NS.notes[3].header.type == CLAP_EVENT_MIDI && NS.notes[4].header.type == CLAP_EVENT_MIDI &&
        NS.notes[4].midi.data[1] == 61, "CLAP+MIDI: channel and poly pressure are raw MIDI too");
  ns_in(15, NOTE_ON, sizeof NOTE_ON);
  CHECK(NS.n_notes == 6 && NS.notes[5].header.type == CLAP_EVENT_NOTE_ON && NS.notes[5].note.key == 60,
        "CLAP+MIDI: a note on is still a CLAP note on");
  ns_in(16, CLOCK, sizeof CLOCK);
  CHECK(NS.n_notes == 6 && atomic_load(&NS.notes_unmappable) == 0, "CLAP+MIDI: a clock is dropped, not counted");
  ns_in(17, SYSEX, sizeof SYSEX);
  CHECK(NS.n_notes == 7 && NS.notes[6].header.type == CLAP_EVENT_MIDI_SYSEX && NS.notes[6].sysex.size == sizeof SYSEX &&
        NS.notes[6].sysex.port_index == 0 && NS.notes[6].sysex.buffer != SYSEX && NS.n_sysex == 1,
        "CLAP+MIDI: a sysex is CLAP_EVENT_MIDI_SYSEX, copied into the block's own bytes");
  CHECK(NS.n_notes == 7 && memcmp(NS.notes[6].sysex.buffer, SYSEX, sizeof SYSEX) == 0, "CLAP+MIDI: the sysex bytes arrive intact");
  memset(big, 0xf0, sizeof big);
  big[OMX_CLAP_SYSEX_MAX_BYTES] = 0xf7;
  ns_in(18, big, sizeof big);
  CHECK(NS.n_sysex == 1 && atomic_load(&NS.notes_unmappable) == 1, "CLAP+MIDI: a sysex past %u bytes is counted, not held (%u)",
        OMX_CLAP_SYSEX_MAX_BYTES, atomic_load(&NS.notes_unmappable));
  ns_up(1, CLAP_NOTE_DIALECT_CLAP, CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI);
  for (k = 0; k < OMX_CLAP_SYSEX_PER_BLOCK + 1; k++) ns_in(20 + k, SYSEX, sizeof SYSEX);
  CHECK(NS.n_sysex == OMX_CLAP_SYSEX_PER_BLOCK && atomic_load(&NS.sysex_dropped) == 1,
        "CLAP+MIDI: a sysex past %u per block is counted and dropped (%u, %u)", OMX_CLAP_SYSEX_PER_BLOCK, NS.n_sysex,
        atomic_load(&NS.sysex_dropped));

  /* a MIDI-dialect input: every channel message raw, notes included, and sysex the same */
  ns_up(1, CLAP_NOTE_DIALECT_MIDI, CLAP_NOTE_DIALECT_MIDI);
  ns_in(10, NOTE_ON, sizeof NOTE_ON);
  ns_in(11, ON_VEL0, sizeof ON_VEL0);
  ns_in(12, CTRL, sizeof CTRL);
  ns_in(13, PROG, sizeof PROG);
  CHECK(NS.n_notes == 4 && NS.notes[0].header.type == CLAP_EVENT_MIDI && NS.notes[0].midi.data[0] == 0x93 && NS.notes[1].midi.data[0] == 0x92 &&
        NS.notes[2].midi.data[1] == 7 && NS.notes[3].midi.data[0] == 0xc1 && NS.notes[3].midi.data[2] == 0,
        "MIDI: every channel message reaches the input raw, in order (%u)", NS.n_notes);
  ns_in(14, SYSEX, sizeof SYSEX);
  CHECK(NS.n_notes == 5 && NS.notes[4].header.type == CLAP_EVENT_MIDI_SYSEX && NS.notes[4].sysex.size == sizeof SYSEX,
        "MIDI: a sysex is CLAP_EVENT_MIDI_SYSEX");

  /* no note input: nothing is forwarded or counted, whatever the port declares */
  ns_up(0, CLAP_NOTE_DIALECT_CLAP, CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI);
  ns_in(10, NOTE_ON, sizeof NOTE_ON);
  ns_in(10, CTRL, sizeof CTRL);
  ns_in(10, BEND_MIN, sizeof BEND_MIN);
  ns_in(10, SYSEX, sizeof SYSEX);
  CHECK(NS.n_notes == 0 && NS.n_sysex == 0 && atomic_load(&NS.notes_unmappable) == 0 && atomic_load(&NS.sysex_dropped) == 0,
        "no note input: no message reaches the stage, none counted");
  ns_up(0, CLAP_NOTE_DIALECT_MIDI, CLAP_NOTE_DIALECT_MIDI);
  ns_in(10, CTRL, sizeof CTRL);
  CHECK(NS.n_notes == 0, "no note input, MIDI dialect: nothing reaches the stage");
}

/** Events queued ahead of a block that never runs (a zero-length call) do not reach the next block. */
static void t_stale_events(void) {
  static const uint8_t NOTE_ON[] = {0x90, 60, 100}, SYSEX[] = {0xf0, 1, 2, 3, 0xf7};
  struct rig g;
  float l[MAXB], r[MAXB];
  rig_up(&g, K_PAD, 2, QCAP);
  g.st.note_inputs = 1;
  g.st.note_dialect = CLAP_NOTE_DIALECT_CLAP;
  g.st.note_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
  omx_clap_note_in(&g.st, 90, NOTE_ON, sizeof NOTE_ON);
  omx_clap_note_in(&g.st, 91, SYSEX, sizeof SYSEX);
  CHECK(g.st.n_notes == 2 && g.st.n_sysex == 1, "two events queued (%u notes, %u sysex)", g.st.n_notes, g.st.n_sysex);
  ramp(l, 64), ramp(r, 64);
  run(&g, l, r, 0);
  CHECK(g.st.n_notes == 0 && g.st.n_sysex == 0, "the early-return path clears the queue (%u notes, %u sysex)", g.st.n_notes, g.st.n_sysex);
  run(&g, l, r, 64);
  CHECK(g.f.blocks == 1 && g.f.ev_per_block[0] == 0, "the next block sees none of the stale events (%u)", g.f.ev_per_block[0]);
}

/** The sysex pool is a block's: a full pool is empty again after omx_clap_run and after omx_clap_run_io. */
static void t_sysex_pool_resets(void) {
  static const uint8_t SYSEX[] = {0xf0, 9, 8, 7, 0xf7};
  struct rig g;
  float a[MAXB], b[MAXB], c[MAXB], d[MAXB];
  const float *ins[2] = {a, b};
  float *outs[2] = {c, d};
  uint32_t k;
  rig_up(&g, K_PAD, 2, QCAP);
  g.st.note_inputs = 1;
  g.st.note_dialect = CLAP_NOTE_DIALECT_CLAP;
  g.st.note_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
  ramp(a, 64), ramp(b, 64), ramp(c, 64), ramp(d, 64);
  for (k = 0; k < OMX_CLAP_SYSEX_PER_BLOCK; k++) omx_clap_note_in(&g.st, k, SYSEX, sizeof SYSEX);
  CHECK(g.st.n_sysex == OMX_CLAP_SYSEX_PER_BLOCK, "the pool is full (%u)", g.st.n_sysex);
  run(&g, c, d, 64);
  CHECK(g.st.n_sysex == 0 && g.st.n_notes == 0, "omx_clap_run empties the pool (%u sysex, %u notes)", g.st.n_sysex, g.st.n_notes);
  for (k = 0; k < OMX_CLAP_SYSEX_PER_BLOCK; k++) omx_clap_note_in(&g.st, k, SYSEX, sizeof SYSEX);
  CHECK(g.st.n_sysex == OMX_CLAP_SYSEX_PER_BLOCK && atomic_load(&g.st.sysex_dropped) == 0,
        "the next block takes a full pool again, none dropped (%u)", atomic_load(&g.st.sysex_dropped));
  omx_clap_run_io(&g.st, ins, outs, 64);
  CHECK(g.st.n_sysex == 0 && g.st.n_notes == 0, "omx_clap_run_io empties the pool (%u sysex, %u notes)", g.st.n_sysex, g.st.n_notes);
  omx_clap_note_in(&g.st, 0, SYSEX, sizeof SYSEX);
  CHECK(g.st.n_sysex == 1 && g.st.notes[0].sysex.buffer == g.st.sysex_bytes[0] && atomic_load(&g.st.sysex_dropped) == 0,
        "and a sysex lands in slot 0 again");
}

int main(void) {
  for (size_t ri = 0; ri < sizeof RATES / sizeof RATES[0]; ri++) {
    g_rate = RATES[ri];
    const int before = failures;
    t_pad();
    t_mono();
    t_bypass_reset();
    t_events();
    t_constant();
    t_discard(K_ERROR);
    t_discard(K_NAN);
    t_clamp();
    t_prime();
    t_state();
    t_out_events();
    t_xfade();
    t_channel_messages();
    t_stale_events();
    t_sysex_pool_resets();
    CHECK(rt_allocs == 0, "no allocation inside omx_clap_run at %.0f (%d)", (double)g_rate, rt_allocs);
    t_alloc_witness();
    printf("clap_stage @ %.0f: %d failure(s)\n", (double)g_rate, failures - before);
  }
  if (failures) {
    fprintf(stderr, "clap_stage: %d failure(s)\n", failures);
    return 1;
  }
  printf("clap_stage: all passed at every rate (%zu rates, rt allocations: %d)\n", sizeof RATES / sizeof RATES[0], rt_allocs);
  return 0;
}
