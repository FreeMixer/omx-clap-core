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
 * 14. XFADE: omx_wetdry_xfade_block against (1 - i/n)·from + (i/n)·to to < 1e-7; identical bodies bit-exact.
 * 15. WIDE AUXILIARY PORTS: three outputs of 1, 6 and 2 channels over nine scratch blocks, each channel its own block
 *     in port order, a bound output writing the caller's buffers; two inputs of 4 and 1 channels reading the silence
 *     until a port is bound, then the caller's buffers channel by channel, the other port still the silence.
 * 15. NOTE OUTPUT: what a plugin pushes on its note output, through omx_clap_note_out_push as the instance's sink calls
 *     it, comes out of omx_clap_note_out_take as MIDI at its frames: a note on, a program change of two bytes, a choke
 *     and a note off; a NOTE_END and a parameter's event are no note output; a note expression, a sysex, a note with no
 *     key, another port's note and MIDI are counted unmapped; 300 more in one block keep 256 and count the rest
 *     dropped, the order kept; a take of fewer than the ring holds leaves the rest for the next. All under the witness.
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
enum kind { K_PAD, K_CLOBBER, K_NAN, K_HOT, K_ERROR, K_CONSTANT, K_ALLOC, K_TALK, K_NOTES };
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
  uint32_t flood;        /* K_NOTES: note ons pushed on top of the block's own events */
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

/* K_NOTES: one block's note output, in frame order, then `flood` note ons on its last frame */
static void note_push(const clap_output_events_t *out, uint16_t type, uint32_t time, int16_t port, int16_t channel, int16_t key, double velocity) {
  clap_event_note_t e;
  memset(&e, 0, sizeof e);
  e.header = (clap_event_header_t){sizeof e, time, CLAP_CORE_EVENT_SPACE_ID, type, 0};
  e.note_id = -1, e.port_index = port, e.channel = channel, e.key = key, e.velocity = velocity;
  out->try_push(out, &e.header);
}
static void midi_push(const clap_output_events_t *out, uint32_t time, uint16_t port, uint8_t b0, uint8_t b1, uint8_t b2) {
  clap_event_midi_t m;
  memset(&m, 0, sizeof m);
  m.header = (clap_event_header_t){sizeof m, time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
  m.port_index = port, m.data[0] = b0, m.data[1] = b1, m.data[2] = b2;
  out->try_push(out, &m.header);
}
static void notes_push(struct fake *f, const clap_output_events_t *out, uint32_t n) {
  static const uint8_t SYSEX[] = {0xf0, 0x7e, 0x7f, 0x06, 0x01, 0xf7};
  clap_event_note_expression_t x;
  clap_event_midi_sysex_t sx;
  clap_event_param_value_t pv;
  note_push(out, CLAP_EVENT_NOTE_ON, 3, 0, 1, 60, 0.5);        /* 91 3c 40 */
  midi_push(out, 5, 0, 0xc2, 5, 0);                             /* c2 05: two bytes */
  note_push(out, CLAP_EVENT_NOTE_CHOKE, 7, -1, 1, 62, 1.0);    /* 81 3e 00 */
  note_push(out, CLAP_EVENT_NOTE_END, 8, 0, 1, 60, 0.0);       /* the host's: no output */
  memset(&pv, 0, sizeof pv);
  pv.header = (clap_event_header_t){sizeof pv, 8, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
  out->try_push(out, &pv.header);                               /* a parameter's: no output */
  memset(&x, 0, sizeof x);
  x.header = (clap_event_header_t){sizeof x, 9, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_EXPRESSION, 0};
  x.expression_id = CLAP_NOTE_EXPRESSION_PRESSURE, x.note_id = -1, x.port_index = 0, x.channel = 1, x.key = 60, x.value = 0.5;
  out->try_push(out, &x.header);                                /* unmapped */
  memset(&sx, 0, sizeof sx);
  sx.header = (clap_event_header_t){sizeof sx, 9, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI_SYSEX, 0};
  sx.buffer = SYSEX, sx.size = sizeof SYSEX;
  out->try_push(out, &sx.header);                               /* unmapped */
  note_push(out, CLAP_EVENT_NOTE_OFF, 9, 0, 1, -1, 0.0);       /* every key: unmapped */
  note_push(out, CLAP_EVENT_NOTE_ON, 9, 1, 1, 61, 1.0);        /* port 1: unmapped */
  midi_push(out, 9, 1, 0x91, 61, 100);                          /* port 1: unmapped */
  note_push(out, CLAP_EVENT_NOTE_OFF, n - 1, 0, 1, 60, 0.0);   /* 81 3c 00 */
  for (uint32_t i = 0; i < f->flood; i++) note_push(out, CLAP_EVENT_NOTE_ON, n - 1, 0, (int16_t)(i / 128 % 16), (int16_t)(i % 128), 1.0);
}

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
  if (f->kind == K_NOTES && pr->out_events) notes_push(f, pr->out_events, n);
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
  omx_wetdry_xfade_block(want, wet, dl, 100);
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
  omx_denormals_off();
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
  if (kind == K_ERROR) {
    CHECK(atomic_load(&g.st.process_errors) == 1 && atomic_load(&g.st.h.nonfinite_blocks) == 0, "error: one strike on the stage's error counter, counted once, none on the non-finite one");
  } else {
    CHECK(atomic_load(&g.st.h.nonfinite_blocks) == 1 && atomic_load(&g.st.process_errors) == 0, "nan: one strike on the non-finite counter, none on the error one");
  }
  CHECK(atomic_load(&g.st.h.fault) == OMX_HOSTED_FAULT_NONE, "not yet faulted");
  *bad = 0;
  ramp(l, 64), ramp(r, 64);
  run(&g, l, r, 64); /* a recovery before the strike count fades back in */
  float want[MAXB], wet[MAXB];
  for (uint32_t i = 0; i < 64; i++) wet[i] = dl[i] * PAD_GAIN;
  omx_wetdry_xfade_block(want, dl, wet, 64);
  CHECK(bytes_equal(l, want, 64), "a recovery fades back in from dry");
  *bad = 1;
  for (uint32_t k = 1; k < CLAP_HOST_NON_FINITE_STRIKES; k++) {
    ramp(l, 64), ramp(r, 64);
    run(&g, l, r, 64);
  }
  CHECK(atomic_load(&g.st.h.fault) == (kind == K_ERROR ? OMX_HOSTED_FAULT_PROCESS_ERROR : OMX_HOSTED_FAULT_NONFINITE),
        "faulted at the strike count (%u) as %s, never as the plugin's output when it did not run", CLAP_HOST_NON_FINITE_STRIKES,
        kind == K_ERROR ? "a process error" : "non-finite output");
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

/* ---- 15. NOTE OUTPUT ---- */
/* the instance's sink, reduced: every event the plugin pushes goes to the ring at the block's first frame */
struct note_sink {
  clap_output_events_t list;
  struct omx_clap_stage *st;
  struct omx_clap_note_out ring;
};
static bool note_sink_push(const clap_output_events_t *list, const clap_event_header_t *e) {
  struct note_sink *k = (struct note_sink *)list->ctx;
  omx_clap_note_out_push(&k->ring, e, k->st->steady_time);
  return true;
}
static uint32_t notes_take(struct note_sink *k, struct omx_clap_note_out_event *got, uint32_t cap) {
  in_rt = 1;
  const uint32_t n = omx_clap_note_out_take(&k->ring, got, cap);
  in_rt = 0;
  return n;
}
static int note_is(const struct omx_clap_note_out_event *e, int64_t frame, uint32_t time, uint8_t size, uint8_t b0, uint8_t b1, uint8_t b2) {
  return e->frame == frame && e->time == time && e->size == size && e->data[0] == b0 && e->data[1] == b1 && (size < 3 || e->data[2] == b2);
}
static void t_notes_out(void) {
  static struct rig g;
  static struct note_sink k;
  static struct omx_clap_note_out_event got[OMX_CLAP_NOTE_OUT_DEPTH + 64];
  float l[MAXB], r[MAXB];
  rig_up(&g, K_NOTES, 2, QCAP);
  memset(&k, 0, sizeof k);
  k.st = &g.st;
  k.list.ctx = &k;
  k.list.try_push = note_sink_push;
  g.st.proc.out_events = &k.list;
  fill(l, 64, 0.1f), fill(r, 64, 0.1f);
  run(&g, l, r, 64);  /* the first block: start_processing, the fade in */
  notes_take(&k, got, OMX_CLAP_NOTE_OUT_DEPTH + 64);
  const uint32_t unmapped0 = atomic_load(&k.ring.unmapped);
  CHECK(unmapped0 == 5, "five events with no MIDI form are counted: an expression, a sysex, a note on every key, port 1's note and MIDI (%u)", unmapped0);

  int64_t start = g.st.steady_time;
  run(&g, l, r, 64);
  uint32_t n = notes_take(&k, got, OMX_CLAP_NOTE_OUT_DEPTH + 64);
  CHECK(n == 4 && note_is(&got[0], start + 3, 3, 3, 0x91, 60, 64) && note_is(&got[1], start + 5, 5, 2, 0xc2, 5, 0) &&
            note_is(&got[2], start + 7, 7, 3, 0x81, 62, 0) && note_is(&got[3], start + 63, 63, 3, 0x81, 60, 0),
        "a note on, a program change, a choke and a note off come out as MIDI at their frames; the NOTE_END and the parameter do not (%u)", n);
  CHECK(atomic_load(&k.ring.unmapped) == unmapped0 + 5 && atomic_load(&k.ring.dropped) == 0, "five more unmapped, none dropped (%u, %u)",
        atomic_load(&k.ring.unmapped) - unmapped0, atomic_load(&k.ring.dropped));

  g.f.flood = 300;
  start = g.st.steady_time;
  run(&g, l, r, 64);
  n = notes_take(&k, got, OMX_CLAP_NOTE_OUT_DEPTH + 64);
  CHECK(n == OMX_CLAP_NOTE_OUT_DEPTH && atomic_load(&k.ring.dropped) == 304 - OMX_CLAP_NOTE_OUT_DEPTH,
        "304 events in one block: the ring keeps %u and counts %u dropped, never blocking (%u, %u)", OMX_CLAP_NOTE_OUT_DEPTH,
        304 - OMX_CLAP_NOTE_OUT_DEPTH, n, atomic_load(&k.ring.dropped));
  CHECK(n == OMX_CLAP_NOTE_OUT_DEPTH && note_is(&got[0], start + 3, 3, 3, 0x91, 60, 64) && note_is(&got[3], start + 63, 63, 3, 0x81, 60, 0) &&
            note_is(&got[4], start + 63, 63, 3, 0x90, 0, 127) && note_is(&got[n - 1], start + 63, 63, 3, 0x91, 123, 127),
        "the first %u are kept, in the order the plugin pushed them", OMX_CLAP_NOTE_OUT_DEPTH);

  g.f.flood = 0;
  start = g.st.steady_time;
  run(&g, l, r, 64);
  n = notes_take(&k, got, 3);
  const uint32_t rest = notes_take(&k, got + 3, 8);
  CHECK(n == 3 && rest == 1 && note_is(&got[3], start + 63, 63, 3, 0x81, 60, 0), "a take of three leaves the fourth for the next (%u, %u)", n, rest);
  CHECK(notes_take(&k, got, 8) == 0, "and the ring is empty after it");
}

/* ---- 14. XFADE closed form ---- */
static void t_xfade(void) {
  float from[MAXB], to[MAXB], dst[MAXB];
  for (uint32_t i = 0; i < 128; i++) from[i] = 0.7f - 0.01f * (float)i, to[i] = -0.3f + 0.005f * (float)i;
  omx_wetdry_xfade_block(dst, from, to, 128);
  double worst = 0;
  for (uint32_t i = 0; i < 128; i++) {
    const double g = (double)i / 128.0;
    const double want = (1.0 - g) * from[i] + g * to[i];
    const double err = fabs((double)dst[i] - want);
    if (err > worst) worst = err;
  }
  CHECK(worst < 1e-7, "crossfade matches (1-i/n)from + (i/n)to to < 1e-7 (worst %.3g)", worst);
  omx_wetdry_xfade_block(dst, from, from, 128);
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
  NS.out_events.ctx = &NS;      // the stage's own sink, as omx_clap_stage_init sets it: the note ids' sink passes to it
  NS.out_events.try_push = omx_clap_out_push;
}

static void ns_in(uint32_t time, const uint8_t *m, size_t n) { omx_clap_note_in(&NS, time, m, n); }

/*
 * MIDI 2.0: a port that speaks only MIDI 2.0 gets every channel message as the MIDI 2.0 channel voice message the UMP
 * specification translates it to, word for word: UMP message type 4, group 0, the status and channel, the index; 7-bit
 * values scaled to 16 (velocity) or 32 bits, a 14-bit bend to 32, by min-centre-max scaling. A CLAP port that also
 * declares MIDI 2.0 (and not MIDI) keeps its notes, bends and pressure as CLAP, and takes what CLAP cannot carry as MIDI 2.0.
 */
static int m2_is(uint32_t i, uint32_t w0, uint32_t w1) {
  return NS.notes[i].header.type == CLAP_EVENT_MIDI2 && NS.notes[i].header.size == sizeof(clap_event_midi2_t) &&
         NS.notes[i].midi2.data[0] == w0 && NS.notes[i].midi2.data[1] == w1 && NS.notes[i].midi2.data[2] == 0 &&
         NS.notes[i].midi2.data[3] == 0;
}

static void t_midi2(void) {
  static const uint8_t NOTE_ON[] = {0x93, 60, 100}, ON_MAX[] = {0x90, 69, 127}, ON_MIN[] = {0x90, 69, 1};
  static const uint8_t ON_VEL0[] = {0x92, 60, 0}, NOTE_OFF[] = {0x81, 60, 64}, POLY[] = {0xa4, 61, 64};
  static const uint8_t CTRL[] = {0xb0, 7, 127}, CTRL0[] = {0xb0, 7, 0}, PROG[] = {0xc3, 5}, CHAN_PRESS[] = {0xd2, 127};
  static const uint8_t BEND_MIN[] = {0xe5, 0x00, 0x00}, BEND_MID[] = {0xe5, 0x00, 0x40}, BEND_MAX[] = {0xe5, 0x7f, 0x7f};
  static const uint8_t SYSEX[] = {0xf0, 1, 2, 3, 0xf7}, SHORT[] = {0x90, 60}, CLOCK[] = {0xf8};

  CHECK(omx_clap_midi2_scale_up(0, 7, 16) == 0 && omx_clap_midi2_scale_up(64, 7, 16) == 0x8000u &&
            omx_clap_midi2_scale_up(127, 7, 16) == 0xffffu && omx_clap_midi2_scale_up(100, 7, 16) == 51492u &&
            omx_clap_midi2_scale_up(1, 7, 16) == 0x200u,
        "MIDI 2.0: 7 to 16 bits by min-centre-max: 0, 64, 127, 100, 1 -> 0, 0x8000, 0xffff, 51492, 0x200");
  CHECK(omx_clap_midi2_scale_up(127, 7, 32) == 0xffffffffu && omx_clap_midi2_scale_up(64, 7, 32) == 0x80000000u &&
            omx_clap_midi2_scale_up(16383, 14, 32) == 0xffffffffu && omx_clap_midi2_scale_up(8192, 14, 32) == 0x80000000u &&
            omx_clap_midi2_scale_up(8193, 14, 32) == 0x80040020u,
        "MIDI 2.0: 7 and 14 to 32 bits: the top is all ones, the centre is the top bit (%08x)", omx_clap_midi2_scale_up(8193, 14, 32));
  for (uint32_t v = 1; v < 128; v++)
    if (omx_clap_midi2_scale_up(v, 7, 16) >> 9 != v || omx_clap_midi2_scale_up(v, 7, 16) <= omx_clap_midi2_scale_up(v - 1, 7, 16)) {
      CHECK(0, "MIDI 2.0: 7 to 16 bits rises with the value and its top 7 bits read it back (%u)", v);
      break;
    }

  ns_up(1, CLAP_NOTE_DIALECT_MIDI2, CLAP_NOTE_DIALECT_MIDI2);
  ns_in(10, NOTE_ON, sizeof NOTE_ON);
  ns_in(11, ON_MAX, sizeof ON_MAX);
  ns_in(12, ON_MIN, sizeof ON_MIN);
  ns_in(13, ON_VEL0, sizeof ON_VEL0);
  ns_in(14, NOTE_OFF, sizeof NOTE_OFF);
  CHECK(NS.n_notes == 5 && NS.notes[0].header.time == 10 && NS.notes[4].header.time == 14, "MIDI 2.0: five notes, at their frames (%u)", NS.n_notes);
  CHECK(m2_is(0, 0x40933c00u, 51492u << 16), "MIDI 2.0: note on ch 3 key 60 velocity 100 (%08x %08x)", NS.notes[0].midi2.data[0], NS.notes[0].midi2.data[1]);
  CHECK(m2_is(1, 0x40904500u, 0xffff0000u) && m2_is(2, 0x40904500u, 0x02000000u), "MIDI 2.0: velocity 127 is 0xffff, velocity 1 is 0x0200");
  CHECK(m2_is(3, 0x40823c00u, 0x80000000u), "MIDI 2.0: a note on of velocity 0 is a note off of velocity 0x8000 (%08x %08x)",
        NS.notes[3].midi2.data[0], NS.notes[3].midi2.data[1]);
  CHECK(m2_is(4, 0x40813c00u, 0x80000000u), "MIDI 2.0: a note off keeps its release velocity, 64 is 0x8000");

  ns_up(1, CLAP_NOTE_DIALECT_MIDI2, CLAP_NOTE_DIALECT_MIDI2);
  ns_in(0, POLY, sizeof POLY);
  ns_in(1, CTRL, sizeof CTRL);
  ns_in(2, CTRL0, sizeof CTRL0);
  ns_in(3, PROG, sizeof PROG);
  ns_in(4, CHAN_PRESS, sizeof CHAN_PRESS);
  ns_in(5, BEND_MIN, sizeof BEND_MIN);
  ns_in(6, BEND_MID, sizeof BEND_MID);
  ns_in(7, BEND_MAX, sizeof BEND_MAX);
  CHECK(NS.n_notes == 8, "MIDI 2.0: every channel message is delivered (%u)", NS.n_notes);
  CHECK(m2_is(0, 0x40a43d00u, 0x80000000u), "MIDI 2.0: poly pressure 64 on key 61 of channel 4 is 0x80000000");
  CHECK(m2_is(1, 0x40b00700u, 0xffffffffu) && m2_is(2, 0x40b00700u, 0), "MIDI 2.0: controller 7 at 127 and 0 is 0xffffffff and 0");
  CHECK(m2_is(3, 0x40c30000u, 0x05000000u), "MIDI 2.0: program 5 on channel 3, no bank (%08x %08x)", NS.notes[3].midi2.data[0], NS.notes[3].midi2.data[1]);
  CHECK(m2_is(4, 0x40d20000u, 0xffffffffu), "MIDI 2.0: channel pressure 127 is 0xffffffff");
  CHECK(m2_is(5, 0x40e50000u, 0) && m2_is(6, 0x40e50000u, 0x80000000u) && m2_is(7, 0x40e50000u, 0xffffffffu),
        "MIDI 2.0: the bend's bottom, centre and top are 0, 0x80000000 and 0xffffffff");
  ns_in(8, SYSEX, sizeof SYSEX);
  ns_in(9, SHORT, sizeof SHORT);
  ns_in(10, CLOCK, sizeof CLOCK);
  CHECK(NS.n_notes == 8 && atomic_load(&NS.notes_unmappable) == 2,
        "MIDI 2.0 only: a sysex and a short note on are counted, a clock byte dropped (%u, %u)", NS.n_notes, atomic_load(&NS.notes_unmappable));

  /* a release keeps its reserve as MIDI 2.0 too: with the block full up to the reserve, a note off still goes */
  ns_up(1, CLAP_NOTE_DIALECT_MIDI2, CLAP_NOTE_DIALECT_MIDI2);
  for (uint32_t k = 0; k < CLAP_HOST_NOTES_PER_BLOCK - OMX_CLAP_NOTES_RESERVED + 1u; k++)
    ns_in(0, ON_MAX, sizeof ON_MAX);
  ns_in(1, ON_VEL0, sizeof ON_VEL0);
  CHECK(NS.n_notes == CLAP_HOST_NOTES_PER_BLOCK - OMX_CLAP_NOTES_RESERVED + 1u && atomic_load(&NS.notes_dropped) == 1 &&
            m2_is(NS.n_notes - 1, 0x40823c00u, 0x80000000u),
        "MIDI 2.0: past the reserve a note on is dropped and a release still goes (%u, %u)", NS.n_notes, atomic_load(&NS.notes_dropped));

  /* a sysex on a port that declares MIDI 2.0 and MIDI goes as a sysex */
  ns_up(1, CLAP_NOTE_DIALECT_MIDI2, CLAP_NOTE_DIALECT_MIDI2 | CLAP_NOTE_DIALECT_MIDI);
  ns_in(0, SYSEX, sizeof SYSEX);
  CHECK(NS.n_notes == 1 && NS.notes[0].header.type == CLAP_EVENT_MIDI_SYSEX, "MIDI 2.0 and MIDI: a sysex is a CLAP sysex");

  /* CLAP and MIDI 2.0: CLAP's own messages stay CLAP, a controller and a program change go as MIDI 2.0, a sysex is counted */
  ns_up(1, CLAP_NOTE_DIALECT_CLAP, CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI2);
  ns_in(0, NOTE_ON, sizeof NOTE_ON);
  ns_in(1, BEND_MAX, sizeof BEND_MAX);
  ns_in(2, CTRL, sizeof CTRL);
  ns_in(3, PROG, sizeof PROG);
  ns_in(4, SYSEX, sizeof SYSEX);
  CHECK(NS.n_notes == 4 && NS.notes[0].header.type == CLAP_EVENT_NOTE_ON && NS.notes[1].header.type == CLAP_EVENT_NOTE_EXPRESSION &&
            m2_is(2, 0x40b00700u, 0xffffffffu) && m2_is(3, 0x40c30000u, 0x05000000u) && atomic_load(&NS.notes_unmappable) == 1,
        "CLAP and MIDI 2.0: note and bend as CLAP, controller and program as MIDI 2.0, the sysex counted (%u, %u)", NS.n_notes,
        atomic_load(&NS.notes_unmappable));
}

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

/* ---- 9b. NOTE IDS AND MPE: a CLAP-dialect input fed with the stage's note ids ---- */
static struct omx_clap_note_ids NI;
static struct omx_clap_mpe_zones MZ;

/* a CLAP-only input with note ids, held `hold` frames after a note off; the block's notes emptied */
static void ni_up(uint32_t hold) {
  ns_up(1, CLAP_NOTE_DIALECT_CLAP, CLAP_NOTE_DIALECT_CLAP);
  memset(&NI, 0, sizeof NI);
  memset(&MZ, 0, sizeof MZ);
  omx_clap_note_ids_bind(&NI, &NS, hold);
}
static void ni_in(uint32_t time, const uint8_t *m, size_t n) { omx_clap_note_in_ids(&NS, &NI, &MZ, time, m, n); }
static void ni_block(void) { NS.n_notes = 0; NS.n_sysex = 0; }
/* the plugin's NOTE_END for `note_id` (-1: by channel and key), through the sink process() is handed */
static void ni_end(int32_t note_id, int16_t channel, int16_t key) {
  clap_event_note_t e;
  memset(&e, 0, sizeof e);
  e.header.size = sizeof e, e.header.space_id = CLAP_CORE_EVENT_SPACE_ID, e.header.type = CLAP_EVENT_NOTE_END;
  e.note_id = note_id, e.channel = channel, e.key = key;
  NS.proc.out_events->try_push(NS.proc.out_events, &e.header);
}
static int ni_expr(uint32_t i, int32_t expr, int32_t id, int16_t channel, int16_t key, double value) {
  const clap_event_note_expression_t *e = &NS.notes[i].expr;
  return i < NS.n_notes && NS.notes[i].header.type == CLAP_EVENT_NOTE_EXPRESSION && e->expression_id == expr &&
         e->note_id == id && e->channel == channel && e->key == key && e->value == value;
}

/* every note on gets a fresh id, its note off the same; NOTE_END frees it; the hold frees an id a plugin never ends */
static void t_note_ids(void) {
  static const uint8_t ON60[] = {0x90, 60, 100}, ON62[] = {0x90, 62, 90}, OFF60[] = {0x80, 60, 0}, OFF62[] = {0x90, 62, 0};
  static const uint8_t POLY62[] = {0xa0, 62, 64}, OFF70[] = {0x80, 70, 0}, ALL_OFF[] = {0xb0, 123, 0};
  static const int RATES_C[] = OMX_ORACLE_FULL_RATES_INIT;
  int32_t a, b;
  uint32_t i, ri;

  ni_up(1000);
  ni_in(0, ON60, 3);
  ni_in(1, ON62, 3);
  a = NS.notes[0].note.note_id, b = NS.notes[1].note.note_id;
  CHECK(NS.n_notes == 2 && a >= 0 && b >= 0 && a != b && NI.n_free == OMX_CLAP_NOTE_IDS - 2,
        "two note ons take two fresh ids (%d, %d, %u free)", a, b, NI.n_free);
  ni_in(2, POLY62, 3);
  CHECK(ni_expr(2, CLAP_NOTE_EXPRESSION_PRESSURE, b, 0, 62, 64.0 / 127.0), "poly aftertouch on key 62 carries its note's id %d", b);
  ni_in(3, OFF60, 3);
  ni_in(4, OFF62, 3);
  CHECK(NS.notes[3].header.type == CLAP_EVENT_NOTE_OFF && NS.notes[3].note.note_id == a && NS.notes[3].note.key == 60 &&
        NS.notes[4].header.type == CLAP_EVENT_NOTE_OFF && NS.notes[4].note.note_id == b,
        "each note off carries its note on's id, a note on with velocity 0 too (%d, %d)", NS.notes[3].note.note_id, NS.notes[4].note.note_id);
  CHECK(NI.n_free == OMX_CLAP_NOTE_IDS - 2, "a released id is not free until NOTE_END (%u)", NI.n_free);
  ni_end(a, 0, 60);
  CHECK(NI.n_free == OMX_CLAP_NOTE_IDS - 1 && atomic_load(&NI.notes_ended) == 1 && atomic_load(&NS.out_events_seen) == 1,
        "the plugin's NOTE_END frees id %d, counted on the stage's sink too (%u free)", a, NI.n_free);
  ni_end(-1, 0, 62);
  CHECK(NI.n_free == OMX_CLAP_NOTE_IDS && atomic_load(&NI.notes_ended) == 2, "a NOTE_END by channel and key frees that key's id (%u)", NI.n_free);
  ni_end(a, 0, 60);
  ni_end(OMX_CLAP_NOTE_IDS + 3, 0, 60);
  CHECK(atomic_load(&NI.notes_ended) == 2, "a NOTE_END for an id no note holds frees nothing (%u)", atomic_load(&NI.notes_ended));

  /* a key struck again while it sounds: the old note ends under its id before the new one starts under another */
  ni_up(1000);
  ni_in(0, ON60, 3);
  a = NS.notes[0].note.note_id;
  ni_in(1, ON60, 3);
  b = NS.notes[2].note.note_id;
  CHECK(NS.n_notes == 3 && NS.notes[1].header.type == CLAP_EVENT_NOTE_OFF && NS.notes[1].note.note_id == a &&
        NS.notes[2].header.type == CLAP_EVENT_NOTE_ON && b >= 0 && b != a, "a key struck twice ends id %d before it takes %d", a, b);
  ni_in(2, OFF70, 3);
  CHECK(NS.n_notes == 4 && NS.notes[3].header.type == CLAP_EVENT_NOTE_OFF && NS.notes[3].note.note_id == -1,
        "a note off of a key no id holds goes with id -1");
  ni_in(3, ALL_OFF, 3);
  CHECK(NI.ids[b].state == 2, "all notes off releases every id of the channel");

  /* the pool runs out: a note on is skipped and counted, and its note off dropped; the hold gives the ids back, at every rate */
  for (ri = 0; ri < sizeof RATES_C / sizeof RATES_C[0]; ri++) {
    const uint32_t hold = (uint32_t)lround(RATES_C[ri] * OMX_CLAP_NOTE_ID_HOLD_MS / 1000.0);
    uint8_t on[3] = {0x90, 0, 100}, off[3] = {0x80, 0, 0};

    ni_up(hold);
    for (i = 0; i < OMX_CLAP_NOTE_IDS; i++) {
      on[0] = (uint8_t)(0x90 | i / 128), on[1] = (uint8_t)(i % 128);
      off[0] = (uint8_t)(0x80 | i / 128), off[1] = on[1];
      ni_block();
      ni_in(0, on, 3);
      ni_in(1, off, 3);
    }
    CHECK(NI.n_free == 0 && atomic_load(&NS.notes_delivered) == 0, "@%d: %u notes played and released hold every id (%u free)", RATES_C[ri],
          OMX_CLAP_NOTE_IDS, NI.n_free);
    ni_block();
    on[0] = 0x95, off[0] = 0x85, on[1] = off[1] = 1;
    ni_in(0, on, 3);
    ni_in(1, off, 3);
    CHECK(NS.n_notes == 0 && atomic_load(&NI.notes_skipped) == 1, "@%d: a note on past the pool is skipped and counted, its note off dropped (%u events, %u skipped)",
          RATES_C[ri], NS.n_notes, atomic_load(&NI.notes_skipped));
    NS.steady_time = hold - 2;     // the last id was released at frame 1
    ni_in(0, on, 3);
    CHECK(NS.n_notes == 0 && atomic_load(&NI.notes_skipped) == 2, "@%d: one frame short of the %u-frame hold, still none free", RATES_C[ri], hold);
    NS.steady_time = hold + 1;
    ni_in(0, on, 3);
    CHECK(NS.n_notes == 1 && NS.notes[0].note.note_id >= 0 && atomic_load(&NI.ids_expired) == OMX_CLAP_NOTE_IDS,
          "@%d: once the hold (%u frames, %u ms) has run out, the ids come back (%u expired)", RATES_C[ri], hold, OMX_CLAP_NOTE_ID_HOLD_MS,
          atomic_load(&NI.ids_expired));
  }

  /* #138: a NOTE_END the plugin sends late, past the hold, for the old voice of an id the pool has handed to a new note:
   * the new note keeps its id. Note i of the loop took id i, on channel i / 128 and key i % 128; the new note is on
   * channel 6, key 1. */
  {
    const int32_t x = NS.notes[0].note.note_id;
    const uint8_t off61[] = {0x85, 1, 0};

    CHECK(x >= 0 && NI.ids[x].state == 1 && NI.key_id[5][1] == x, "the new note on channel 6, key 1 holds the reused id %d", x);
    ni_end(x, (int16_t)(x / 128), (int16_t)(x % 128));
    CHECK(NI.ids[x].state == 1 && NI.key_id[5][1] == x && atomic_load(&NI.notes_ended) == 0,
          "a late NOTE_END for the old voice of id %d (channel %d, key %d) leaves the live note its id (state %u, key holds %d)",
          x, x / 128 + 1, x % 128, NI.ids[x].state, NI.key_id[5][1]);
    ni_block();
    ni_in(0, off61, 3);
    CHECK(NS.n_notes == 1 && NS.notes[0].header.type == CLAP_EVENT_NOTE_OFF && NS.notes[0].note.note_id == x,
          "the new note's note off still carries id %d (%d)", x, NS.n_notes ? NS.notes[0].note.note_id : -9);
    ni_end(x, 5, 1);
    CHECK(NI.ids[x].state == 0 && atomic_load(&NI.notes_ended) == 1, "its own NOTE_END, channel 6 and key 1, frees id %d", x);
    ni_end(-1, -1, -1);
  }

  /* a released id's NOTE_END that names another channel and key is not its own either; one with -1 in either matches */
  ni_up(1000);
  ni_in(0, ON60, 3);
  a = NS.notes[0].note.note_id;
  ni_in(1, OFF60, 3);
  ni_end(a, 0, 61);
  CHECK(NI.ids[a].state == 2 && atomic_load(&NI.notes_ended) == 0, "a NOTE_END for id %d on key 61 does not free key 60's id", a);
  ni_end(a, -1, 60);
  CHECK(NI.ids[a].state == 0 && atomic_load(&NI.notes_ended) == 1, "one on key 60 with channel -1 does");
}

/* an MPE zone: each member channel's bend, pressure and CC74 are per-note expressions of the note sounding on it */
static void t_mpe(void) {
  static const uint8_t MCM_LOWER[][3] = {{0xb0, 101, 0}, {0xb0, 100, 6}, {0xb0, 6, 15}};
  static const uint8_t ON2[] = {0x91, 60, 100}, ON3[] = {0x92, 64, 100}, BEND2_MAX[] = {0xe1, 0x7f, 0x7f};
  static const uint8_t PRESS3[] = {0xd2, 64}, BRIGHT2[] = {0xb1, 74, 127}, BEND1_MIN[] = {0xe0, 0, 0};
  static const uint8_t RPN0_MEMBER[][3] = {{0xb2, 101, 0}, {0xb2, 100, 0}, {0xb2, 6, 24}}, BEND3_MIN[] = {0xe2, 0, 0};
  static const uint8_t OFF2[] = {0x81, 60, 0}, BRIGHT2_ALONE[] = {0xb1, 74, 0};
  static const uint8_t MCM_UPPER2[][3] = {{0xbf, 101, 0}, {0xbf, 100, 6}, {0xbf, 6, 2}}, BEND14[] = {0xed, 0, 0}, BEND13[] = {0xec, 0, 0};
  int32_t a, b;
  uint32_t i;

  ni_up(1000);
  for (i = 0; i < 3; i++) ni_in(0, MCM_LOWER[i], 3);
  CHECK(MZ.zone[0].members == 15 && MZ.zone[1].members == 0, "the MPE configuration message on channel 1 sets the lower zone, 15 members");
  ni_block();
  ni_in(0, ON2, 3);
  ni_in(1, ON3, 3);
  a = NS.notes[0].note.note_id, b = NS.notes[1].note.note_id;
  ni_in(2, BEND2_MAX, 3);
  ni_in(3, PRESS3, 2);
  ni_in(4, BRIGHT2, 3);
  ni_in(5, BEND1_MIN, 3);
  CHECK(ni_expr(2, CLAP_NOTE_EXPRESSION_TUNING, a, 1, 60, 8191.0 / 8192.0 * 48.0),
        "a member's bend is TUNING of the note on it (id %d, channel 2, key 60), 48 semitones full scale (%g)", a, NS.notes[2].expr.value);
  CHECK(ni_expr(3, CLAP_NOTE_EXPRESSION_PRESSURE, b, 2, 64, 64.0 / 127.0), "a member's channel pressure is PRESSURE of its note (id %d)", b);
  CHECK(ni_expr(4, CLAP_NOTE_EXPRESSION_BRIGHTNESS, a, 1, 60, 1.0), "a member's controller 74 is BRIGHTNESS of its note (id %d)", a);
  CHECK(ni_expr(5, CLAP_NOTE_EXPRESSION_TUNING, a, 1, 60, -2.0) && ni_expr(6, CLAP_NOTE_EXPRESSION_TUNING, b, 2, 64, -2.0),
        "the master's bend reaches each note of its zone, with its id, 2 semitones full scale (%g, %g)", NS.notes[5].expr.value,
        NS.notes[6].expr.value);
  CHECK(NS.n_notes == 7 && atomic_load(&NS.notes_unmappable) == 0, "nothing of the zone is counted unmappable (%u)", atomic_load(&NS.notes_unmappable));

  /* RPN 0 on a member sets the members' bend range */
  ni_block();
  for (i = 0; i < 3; i++) ni_in(0, RPN0_MEMBER[i], 3);
  ni_in(1, BEND3_MIN, 3);
  CHECK(MZ.zone[0].member_bend == 24.0 && ni_expr(0, CLAP_NOTE_EXPRESSION_TUNING, b, 2, 64, -24.0),
        "RPN 0 on a member sets 24 semitones: the bottom of the bend is -24 (%g)", NS.notes[0].expr.value);
  /* a member with no note sounding: its expressions reach the whole channel */
  ni_block();
  ni_in(0, OFF2, 3);
  ni_in(1, BRIGHT2_ALONE, 3);
  CHECK(NS.notes[0].note.note_id == a && ni_expr(1, CLAP_NOTE_EXPRESSION_BRIGHTNESS, -1, 1, -1, 0.0),
        "after its note off, a member's controller 74 is BRIGHTNESS of the whole channel");

  /* the upper zone of two members: channels 15 and 14 are members, 13 is outside and keeps its plain mapping */
  ni_up(1000);
  for (i = 0; i < 3; i++) ni_in(0, MCM_UPPER2[i], 3);
  ni_block();
  ni_in(0, BEND14, 3);
  ni_in(1, BEND13, 3);
  CHECK(MZ.zone[1].members == 2 && MZ.zone[0].members == 0 && ni_expr(0, CLAP_NOTE_EXPRESSION_TUNING, -1, 13, -1, -48.0) &&
        ni_expr(1, CLAP_NOTE_EXPRESSION_TUNING, -1, 12, -1, -2.0),
        "the upper zone of 2: channel 14 is a member (-48), channel 13 a plain channel (-2) (%g, %g)", NS.notes[0].expr.value,
        NS.notes[1].expr.value);
  ni_in(2, (const uint8_t[]){0xbf, 6, 0}, 3);
  CHECK(MZ.zone[1].members == 0, "a configuration message of 0 members ends the zone");

  /* a zone the control thread asks for is taken ahead of the next message */
  ni_up(1000);
  omx_clap_mpe_zones_want(&MZ, OMX_CLAP_MPE_LOWER, 3);
  CHECK(MZ.zone[0].members == 0, "a zone asked for waits for the RT");
  ni_in(0, ON2, 3);
  ni_in(1, BRIGHT2, 3);
  CHECK(MZ.zone[0].members == 3 && ni_expr(1, CLAP_NOTE_EXPRESSION_BRIGHTNESS, NS.notes[0].note.note_id, 1, 60, 1.0),
        "the zone asked for is the input's from the next message: controller 74 is the note's BRIGHTNESS");

  /* a zone asked for before the first bind, or while deactivated, lasts across the bind (the header's promise) */
  ns_up(1, CLAP_NOTE_DIALECT_CLAP, CLAP_NOTE_DIALECT_CLAP);
  memset(&NI, 0, sizeof NI);
  memset(&MZ, 0, sizeof MZ);
  omx_clap_mpe_zones_want(&MZ, OMX_CLAP_MPE_LOWER, 3);
  omx_clap_note_ids_bind(&NI, &NS, 1000);
  ni_in(0, ON2, 3);
  CHECK(MZ.zone[0].members == 3, "a zone asked for before the first activate is the input's at its first message");
  omx_clap_mpe_zones_want(&MZ, OMX_CLAP_MPE_UPPER, 5);
  omx_clap_note_ids_bind(&NI, &NS, 1000);
  omx_clap_note_ids_reset(&NI);
  ni_in(0, ON2, 3);
  CHECK(MZ.zone[1].members == 5 && MZ.zone[0].members == 3,
        "a zone asked for while deactivated lasts across the next activate and a reset, beside the lower (%u, %u members)",
        MZ.zone[1].members, MZ.zone[0].members);

  /* the stage with no note ids reads no zone: the same messages go as before, channel-wide, id -1 */
  ns_up(1, CLAP_NOTE_DIALECT_CLAP, CLAP_NOTE_DIALECT_CLAP);
  for (i = 0; i < 3; i++) ns_in(0, MCM_LOWER[i], 3);
  ns_in(1, ON2, 3);
  ns_in(2, BEND2_MAX, 3);
  CHECK(NS.notes[0].note.note_id == -1 && ni_expr(1, CLAP_NOTE_EXPRESSION_TUNING, -1, 1, -1, 8191.0 / 8192.0 * 2.0),
        "with no note ids: note id -1 and a channel-wide bend of 2 semitones");

  /* a MIDI_MPE-dialect input: every message raw, the zone the plugin's own */
  ns_up(1, CLAP_NOTE_DIALECT_MIDI_MPE, CLAP_NOTE_DIALECT_MIDI_MPE);
  ns_in(0, ON2, 3);
  ns_in(1, BEND2_MAX, 3);
  CHECK(NS.n_notes == 2 && NS.notes[0].header.type == CLAP_EVENT_MIDI && NS.notes[0].midi.data[0] == 0x91 &&
        NS.notes[1].header.type == CLAP_EVENT_MIDI && NS.notes[1].midi.data[0] == 0xe1, "MIDI_MPE: note on and bend reach the plugin raw");
}

/* ---- 9c. TWO MPE ZONES HELD TOGETHER (#139) ---- */
/* the MPE configuration message: RPN 6 on the zone's master channel (0 lower, 15 upper) with `members` */
static void ni_mcm(uint8_t master, uint8_t members) {
  const uint8_t m[3][3] = {{(uint8_t)(0xb0 | master), 101, 0}, {(uint8_t)(0xb0 | master), 100, 6}, {(uint8_t)(0xb0 | master), 6, members}};
  for (int i = 0; i < 3; i++) ni_in(0, m[i], 3);
}
/* RPN 0 (bend range) on channel `ch`, `semitones` */
static void ni_rpn0(uint8_t ch, uint8_t semitones) {
  const uint8_t m[3][3] = {{(uint8_t)(0xb0 | ch), 101, 0}, {(uint8_t)(0xb0 | ch), 100, 0}, {(uint8_t)(0xb0 | ch), 6, semitones}};
  for (int i = 0; i < 3; i++) ni_in(0, m[i], 3);
}
static void ni_on(uint8_t ch, uint8_t key) { const uint8_t m[3] = {(uint8_t)(0x90 | ch), key, 100}; ni_in(0, m, 3); }
static void ni_bend(uint8_t ch, uint8_t lsb, uint8_t msb) { const uint8_t m[3] = {(uint8_t)(0xe0 | ch), lsb, msb}; ni_in(0, m, 3); }
static void ni_press(uint8_t ch, uint8_t v) { const uint8_t m[2] = {(uint8_t)(0xd0 | ch), v}; ni_in(0, m, 2); }
static void ni_cc74(uint8_t ch, uint8_t v) { const uint8_t m[3] = {(uint8_t)(0xb0 | ch), 74, v}; ni_in(0, m, 3); }
/* the id of the note just started: the NOTE_ON event at index `i` */
static int32_t ni_id_at(uint32_t i) { return NS.notes[i].header.type == CLAP_EVENT_NOTE_ON ? NS.notes[i].note.note_id : -2; }
/* the number of expression events of block with `id` */
static uint32_t ni_expr_count(int32_t id) {
  uint32_t c = 0;
  for (uint32_t i = 0; i < NS.n_notes; i++)
    if (NS.notes[i].header.type == CLAP_EVENT_NOTE_EXPRESSION && NS.notes[i].expr.note_id == id) c++;
  return c;
}

static void t_mpe_two_zones(void) {
  const double FULL = 8191.0 / 8192.0;
  int32_t lo1, lo2, up1, up2;

  /* lower: master 1, members 2..5 (4); upper: master 16, members 15, 14, 13 (3); a note in each of two members of both */
  ni_up(1000);
  ni_mcm(0, 4);
  ni_mcm(15, 3);
  CHECK(MZ.zone[0].members == 4 && MZ.zone[1].members == 3, "both zones live at once: lower 4 members, upper 3 (%u, %u)",
        MZ.zone[0].members, MZ.zone[1].members);
  ni_block();
  ni_on(1, 60); ni_on(2, 62); ni_on(14, 72); ni_on(13, 74);
  lo1 = ni_id_at(0), lo2 = ni_id_at(1), up1 = ni_id_at(2), up2 = ni_id_at(3);
  CHECK(lo1 >= 0 && lo2 >= 0 && up1 >= 0 && up2 >= 0 && NS.n_notes == 4, "a note in each of four members takes four ids (%d %d %d %d)", lo1, lo2, up1, up2);

  /* the lower master's bend, pressure and controller 74 reach the lower zone's two notes and no note of the upper zone */
  ni_block();
  ni_bend(0, 0x7f, 0x7f);
  CHECK(NS.n_notes == 2 && ni_expr(0, CLAP_NOTE_EXPRESSION_TUNING, lo1, 1, 60, FULL * 2.0) &&
        ni_expr(1, CLAP_NOTE_EXPRESSION_TUNING, lo2, 2, 62, FULL * 2.0),
        "the lower master's bend is TUNING of the two lower notes only, 2 semitones (%u events, %g)", NS.n_notes, NS.notes[0].expr.value);
  ni_block();
  ni_press(0, 127);
  CHECK(NS.n_notes == 2 && ni_expr(0, CLAP_NOTE_EXPRESSION_PRESSURE, lo1, 1, 60, 1.0) && ni_expr(1, CLAP_NOTE_EXPRESSION_PRESSURE, lo2, 2, 62, 1.0) &&
        ni_expr_count(up1) == 0 && ni_expr_count(up2) == 0, "the lower master's pressure reaches the lower notes only");
  ni_block();
  ni_cc74(0, 127);
  CHECK(NS.n_notes == 2 && ni_expr(0, CLAP_NOTE_EXPRESSION_BRIGHTNESS, lo1, 1, 60, 1.0) && ni_expr_count(up1) + ni_expr_count(up2) == 0,
        "the lower master's controller 74 reaches the lower notes only");

  /* and the upper master's reach the upper zone's notes only */
  ni_block();
  ni_bend(15, 0, 0);
  CHECK(NS.n_notes == 2 && ni_expr(0, CLAP_NOTE_EXPRESSION_TUNING, up1, 14, 72, -2.0) && ni_expr(1, CLAP_NOTE_EXPRESSION_TUNING, up2, 13, 74, -2.0) &&
        ni_expr_count(lo1) + ni_expr_count(lo2) == 0, "the upper master's bend is TUNING of the two upper notes only (%u events, %g)", NS.n_notes,
        NS.notes[0].expr.value);
  ni_block();
  ni_press(15, 64);
  CHECK(NS.n_notes == 2 && ni_expr(0, CLAP_NOTE_EXPRESSION_PRESSURE, up1, 14, 72, 64.0 / 127.0) && ni_expr_count(lo1) + ni_expr_count(lo2) == 0,
        "the upper master's pressure reaches the upper notes only");

  /* a member's own expression stays on its own note, 48 semitones, whichever zone */
  ni_block();
  ni_bend(14, 0x7f, 0x7f);
  ni_bend(2, 0, 0);
  CHECK(NS.n_notes == 2 && ni_expr(0, CLAP_NOTE_EXPRESSION_TUNING, up1, 14, 72, FULL * 48.0) && ni_expr(1, CLAP_NOTE_EXPRESSION_TUNING, lo2, 2, 62, -48.0),
        "a member's bend is its own note's, 48 semitones, in either zone");

  /* a master of a zone with no note held makes no event */
  ni_up(1000);
  ni_mcm(0, 2);
  ni_mcm(15, 2);
  ni_block();
  ni_bend(0, 0x7f, 0x7f);
  ni_press(15, 100);
  CHECK(NS.n_notes == 0, "a master of a zone with no note held sends nothing (%u)", NS.n_notes);

  /* the configuration messages set each zone alone: lower to 1 member leaves the upper as it is, and a held note of
     the channel that left the zone keeps its id and sounds, its channel no longer per note */
  ni_up(1000);
  ni_mcm(0, 4);
  ni_mcm(15, 3);
  ni_block();
  ni_on(1, 60); ni_on(2, 62); ni_on(14, 72);
  lo1 = ni_id_at(0), lo2 = ni_id_at(1), up1 = ni_id_at(2);
  ni_mcm(0, 1);
  CHECK(MZ.zone[0].members == 1 && MZ.zone[1].members == 3, "a lower configuration message of 1 leaves the upper zone's 3 (%u, %u)", MZ.zone[0].members,
        MZ.zone[1].members);
  CHECK(NI.ids[lo2].state == 1 && NI.key_id[2][62] == lo2, "the note on the channel that left the zone keeps its id %d", lo2);
  ni_block();
  ni_bend(0, 0x7f, 0x7f);
  CHECK(NS.n_notes == 1 && ni_expr(0, CLAP_NOTE_EXPRESSION_TUNING, lo1, 1, 60, FULL * 2.0),
        "the lower master now reaches the one lower note, not the note of the channel that left (%u events)", NS.n_notes);
  ni_block();
  ni_press(2, 50);
  CHECK(NS.n_notes == 1 && ni_expr(0, CLAP_NOTE_EXPRESSION_PRESSURE, -1, 2, -1, 50.0 / 127.0),
        "the channel that left the zone is a plain channel: pressure is channel-wide, id -1");
  ni_block();
  ni_bend(15, 0, 0);
  CHECK(NS.n_notes == 1 && ni_expr(0, CLAP_NOTE_EXPRESSION_TUNING, up1, 14, 72, -2.0), "the upper zone is untouched by the lower message");
  ni_block();
  { const uint8_t off[3] = {0x82, 62, 0}; ni_in(0, off, 3); }
  CHECK(NS.n_notes == 1 && NS.notes[0].header.type == CLAP_EVENT_NOTE_OFF && NS.notes[0].note.note_id == lo2, "its note off still carries id %d", lo2);

  /* a configuration message of 0 ends one zone alone */
  ni_mcm(15, 0);
  CHECK(MZ.zone[1].members == 0 && MZ.zone[0].members == 1, "the upper message of 0 ends the upper zone alone (%u, %u)", MZ.zone[1].members, MZ.zone[0].members);

  /* overlap: the zone just configured wins, the other keeps what is left */
  ni_up(1000);
  ni_mcm(0, 10);
  ni_mcm(15, 6);
  CHECK(MZ.zone[1].members == 6 && MZ.zone[0].members == 8, "upper 6 configured last wins: lower 10 is cut to 14 - 6 = 8 (%u, %u)", MZ.zone[1].members,
        MZ.zone[0].members);
  ni_mcm(0, 12);
  CHECK(MZ.zone[0].members == 12 && MZ.zone[1].members == 2, "lower 12 configured last wins: upper 6 is cut to 2 (%u, %u)", MZ.zone[0].members, MZ.zone[1].members);
  ni_mcm(0, 3);
  CHECK(MZ.zone[0].members == 3 && MZ.zone[1].members == 2, "a smaller lower zone gives nothing back to the upper (%u, %u)", MZ.zone[0].members, MZ.zone[1].members);
  ni_mcm(0, 15);
  CHECK(MZ.zone[0].members == 15 && MZ.zone[1].members == 0, "lower 15 reaches channel 16, the upper master: the upper zone ends (%u, %u)", MZ.zone[0].members,
        MZ.zone[1].members);
  ni_mcm(15, 4);
  CHECK(MZ.zone[1].members == 4 && MZ.zone[0].members == 10, "upper 4 after lower 15: lower is cut to 10 (%u, %u)", MZ.zone[1].members, MZ.zone[0].members);
  ni_mcm(15, 15);
  CHECK(MZ.zone[1].members == 15 && MZ.zone[0].members == 0, "upper 15 ends the lower zone (%u, %u)", MZ.zone[1].members, MZ.zone[0].members);

  /* a note that starts later begins with its zone's master values; the other zone's notes begin with none */
  ni_up(1000);
  ni_mcm(0, 4);
  ni_mcm(15, 3);
  ni_block();
  ni_bend(0, 0x7f, 0x7f);
  ni_press(0, 127);
  ni_on(3, 55);
  lo1 = ni_id_at(0);
  CHECK(NS.n_notes == 3 && lo1 >= 0 && ni_expr(1, CLAP_NOTE_EXPRESSION_TUNING, lo1, 3, 55, FULL * 2.0) && ni_expr(2, CLAP_NOTE_EXPRESSION_PRESSURE, lo1, 3, 55, 1.0),
        "a lower note started after the master's bend and pressure begins with them, and not with controller 74 (%u events)", NS.n_notes);
  ni_block();
  ni_on(13, 70);
  CHECK(NS.n_notes == 1 && ni_id_at(0) >= 0, "an upper note starts with none of the lower master's values (%u events)", NS.n_notes);
  ni_block();
  ni_mcm(0, 4);
  ni_on(2, 57);
  CHECK(NS.n_notes == 1, "the lower zone's configuration message clears its kept values (%u events)", NS.n_notes);
  ni_block();
  ni_bend(15, 0x7f, 0x7f);
  ni_on(12, 68);
  CHECK(NS.n_notes == 3 && ni_expr(2, CLAP_NOTE_EXPRESSION_TUNING, ni_id_at(1), 12, 68, FULL * 2.0),
        "an upper note started after the upper master's bend begins with it (%u events)", NS.n_notes);

  /* the bend range is per zone; a zone's configuration message returns only its own to the default */
  ni_up(1000);
  ni_mcm(0, 4);
  ni_mcm(15, 3);
  ni_rpn0(0, 12);
  ni_rpn0(14, 24);
  CHECK(MZ.zone[0].master_bend == 12.0 && MZ.zone[1].master_bend == 2.0 && MZ.zone[1].member_bend == 24.0 && MZ.zone[0].member_bend == 48.0,
        "RPN 0 on the lower master and on an upper member set those ranges only (%g %g %g %g)", MZ.zone[0].master_bend, MZ.zone[1].master_bend,
        MZ.zone[1].member_bend, MZ.zone[0].member_bend);
  ni_block();
  ni_on(1, 60); ni_on(14, 72);
  lo1 = ni_id_at(0), up1 = ni_id_at(1);
  ni_block();
  ni_bend(0, 0, 0);
  ni_bend(15, 0, 0);
  ni_bend(14, 0, 0);
  CHECK(NS.n_notes == 3 && ni_expr(0, CLAP_NOTE_EXPRESSION_TUNING, lo1, 1, 60, -12.0) && ni_expr(1, CLAP_NOTE_EXPRESSION_TUNING, up1, 14, 72, -2.0) &&
        ni_expr(2, CLAP_NOTE_EXPRESSION_TUNING, up1, 14, 72, -24.0), "each master and member bends over its own zone's range (%g %g %g)",
        NS.notes[0].expr.value, NS.notes[1].expr.value, NS.notes[2].expr.value);
  ni_mcm(0, 4);
  CHECK(MZ.zone[0].master_bend == 2.0 && MZ.zone[0].member_bend == 48.0 && MZ.zone[1].member_bend == 24.0,
        "the lower message returns the lower ranges to the defaults and leaves the upper's 24 (%g %g)", MZ.zone[0].master_bend, MZ.zone[1].member_bend);

  /* the control thread asks for one zone at a time: LOWER or UPPER with 0 ends that zone, OFF ends both */
  ni_up(1000);
  omx_clap_mpe_zones_want(&MZ, OMX_CLAP_MPE_LOWER, 4);
  omx_clap_mpe_zones_want(&MZ, OMX_CLAP_MPE_UPPER, 3);
  ni_block();
  ni_on(1, 60);
  CHECK(MZ.zone[0].members == 4 && MZ.zone[1].members == 3, "the host asks for both zones: the next message takes them (%u, %u)", MZ.zone[0].members,
        MZ.zone[1].members);
  omx_clap_mpe_zones_want(&MZ, OMX_CLAP_MPE_LOWER, 0);
  ni_on(2, 61);
  CHECK(MZ.zone[0].members == 0 && MZ.zone[1].members == 3, "LOWER with 0 members ends the lower zone alone (%u, %u)", MZ.zone[0].members, MZ.zone[1].members);
  omx_clap_mpe_zones_want(&MZ, OMX_CLAP_MPE_LOWER, 5);
  omx_clap_mpe_zones_want(&MZ, OMX_CLAP_MPE_OFF, 0);
  ni_on(3, 62);
  CHECK(MZ.zone[0].members == 0 && MZ.zone[1].members == 0, "OFF ends both zones");
}

/* The survivors of a flood are the messages fed at times 0..n-1 in order: every survivor's time names the message it
 * came from, the times only rise, and each one carries that message's kind and key. Returns the survivors checked. */
static uint32_t np_survivors_in_order(const uint8_t (*m)[3], uint32_t n, const char *what) {
  uint32_t i, last = 0, bad = 0;

  for (i = 0; i < NS.n_notes; i++) {
    const union omx_clap_note *e = &NS.notes[i];
    const uint32_t t = e->header.time;

    if (t >= n || (i > 0 && t <= last)) { bad++; continue; }
    last = t;
    if (e->header.type == CLAP_EVENT_MIDI)
      bad += e->midi.data[0] != m[t][0] || e->midi.data[1] != m[t][1] || e->midi.data[2] != m[t][2];
    else if (e->header.type == CLAP_EVENT_NOTE_ON)
      bad += (m[t][0] & 0xf0) != 0x90 || m[t][2] == 0 || e->note.key != m[t][1];
    else if (e->header.type == CLAP_EVENT_NOTE_OFF)
      bad += ((m[t][0] & 0xf0) == 0x90 ? m[t][2] != 0 : (m[t][0] & 0xf0) != 0x80 && (m[t][0] & 0xf0) != 0xb0) ||
             ((m[t][0] & 0xf0) != 0xb0 && e->note.key != m[t][1]);
    else if (e->header.type == CLAP_EVENT_NOTE_CHOKE)
      bad += (m[t][0] & 0xf0) != 0xb0 || m[t][1] != 120 || e->note.key != -1;
  }
  CHECK(bad == 0, "%s: the %u survivors keep the order and the content they arrived with (%u out of place)", what, NS.n_notes, bad);
  return NS.n_notes;
}

/* Feed `n` messages at times 0..n-1. */
static void np_feed(const uint8_t (*m)[3], uint32_t n) {
  uint32_t i;

  for (i = 0; i < n; i++) ns_in(i, m[i], (m[i][0] & 0xf0) == 0xd0 || (m[i][0] & 0xf0) == 0xc0 ? 2 : 3);
}

#define NP_FLOOD 300u
static uint8_t NP[NP_FLOOD + 8][3];

/* `ons` note ons on channel 0, keys cycling, then the `tail` messages after them */
static uint32_t np_flood(uint32_t ons, const uint8_t (*tail)[3], uint32_t n_tail) {
  uint32_t i;

  for (i = 0; i < ons; i++) { NP[i][0] = 0x90; NP[i][1] = (uint8_t)(i % 128); NP[i][2] = 100; }
  for (i = 0; i < n_tail; i++) memcpy(NP[ons + i], tail[i], 3);
  return ons + n_tail;
}

/* The block's note list is bounded, and a full one drops what can be lost first: a note off, a note on with velocity
 * 0, all notes off, all sound off and a sustain pedal let go always arrive, in the order they came. */
static void t_note_priority(void) {
  static const uint8_t OFF[][3] = {{0x80, 60, 0}}, VEL0[][3] = {{0x90, 61, 0}};
  static const uint8_t PANIC[][3] = {{0xb2, 123, 0}, {0xb2, 120, 0}}, PEDAL_UP[][3] = {{0xb0, 64, 0}};
  static const uint32_t DIALECTS[][2] = {{CLAP_NOTE_DIALECT_CLAP, CLAP_NOTE_DIALECT_CLAP},
                                         {CLAP_NOTE_DIALECT_CLAP, CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI},
                                         {CLAP_NOTE_DIALECT_MIDI, CLAP_NOTE_DIALECT_MIDI}};
  static const char *const NAMES[] = {"CLAP", "CLAP+MIDI", "MIDI"};
  uint32_t d, n, i;
  const size_t align = _Alignof(struct omx_clap_stage);
  const size_t before = offsetof(struct omx_clap_stage, start_refused) + sizeof NS.start_refused;

  const size_t ptr_align = _Alignof(float *);
  const size_t wide_at = (before + sizeof NS.releases_dropped + ptr_align - 1) / ptr_align * ptr_align;
  const size_t wide_end = offsetof(struct omx_clap_stage, wide) + sizeof NS.wide;

  /* releases_dropped (ABI 5) follows start_refused with no gap, and the wide bounce (ABI 13) follows it: every member
   * before them keeps its offset. The ABI check cannot see the atomic ones (libabigail leaves _Atomic members out), so
   * this is the check. */
  CHECK(offsetof(struct omx_clap_stage, releases_dropped) == before && offsetof(struct omx_clap_stage, wide_in) == wide_at &&
        sizeof(struct omx_clap_stage) == (wide_end + align - 1) / align * align,
        "releases_dropped follows start_refused (%zu) and the wide bounce follows it (%zu): size %zu",
        offsetof(struct omx_clap_stage, releases_dropped), offsetof(struct omx_clap_stage, wide_in), sizeof(struct omx_clap_stage));

  for (d = 0; d < 3; d++) {
    const char *what = NAMES[d];
    const union omx_clap_note *last;

    /* 300 note ons and then one note off, all in one block: the note off arrives, the last event of the block */
    ns_up(1, DIALECTS[d][0], DIALECTS[d][1]);
    n = np_flood(NP_FLOOD, OFF, 1);
    np_feed(NP, n);
    np_survivors_in_order(NP, n, what);
    last = &NS.notes[NS.n_notes - 1];
    CHECK(last->header.time == NP_FLOOD &&
          (last->header.type == CLAP_EVENT_NOTE_OFF ? last->note.key == 60 : last->midi.data[0] == 0x80 && last->midi.data[1] == 60),
          "%s: %u note ons then a note off: the note off arrives (the block ends at time %u, type %u)", what, NP_FLOOD,
          last->header.time, last->header.type);
    CHECK(NS.n_notes <= CLAP_HOST_NOTES_PER_BLOCK && NS.n_notes + atomic_load(&NS.notes_dropped) == n,
          "%s: every message is delivered or counted (%u + %u of %u)", what, NS.n_notes, atomic_load(&NS.notes_dropped), n);
    CHECK(NS.n_notes == CLAP_HOST_NOTES_PER_BLOCK - OMX_CLAP_NOTES_RESERVED + 1 &&
          atomic_load(&NS.notes_dropped) == NP_FLOOD - (CLAP_HOST_NOTES_PER_BLOCK - OMX_CLAP_NOTES_RESERVED) &&
          atomic_load(&NS.releases_dropped) == 0,
          "%s: the note ons stop %u slots short of the end, the note off takes one of them (%u delivered, %u dropped, %u releases)",
          what, OMX_CLAP_NOTES_RESERVED, NS.n_notes, atomic_load(&NS.notes_dropped), atomic_load(&NS.releases_dropped));

    /* a note on with velocity 0 is a note off */
    ns_up(1, DIALECTS[d][0], DIALECTS[d][1]);
    n = np_flood(NP_FLOOD, VEL0, 1);
    np_feed(NP, n);
    np_survivors_in_order(NP, n, what);
    CHECK(NS.notes[NS.n_notes - 1].header.time == NP_FLOOD, "%s: a note on with velocity 0 after the flood arrives", what);

    /* all notes off and all sound off after the flood: both arrive, in order */
    ns_up(1, DIALECTS[d][0], DIALECTS[d][1]);
    n = np_flood(NP_FLOOD, PANIC, 2);
    np_feed(NP, n);
    np_survivors_in_order(NP, n, what);
    CHECK(NS.n_notes >= 2 && NS.notes[NS.n_notes - 2].header.time == NP_FLOOD && NS.notes[NS.n_notes - 1].header.time == NP_FLOOD + 1,
          "%s: all notes off and all sound off after the flood both arrive (last at %u)", what, NS.notes[NS.n_notes - 1].header.time);

    /* 300 note offs: the block fills with them and the rest is counted */
    ns_up(1, DIALECTS[d][0], DIALECTS[d][1]);
    for (i = 0; i < NP_FLOOD; i++) { NP[i][0] = 0x80; NP[i][1] = (uint8_t)(i % 128); NP[i][2] = 0; }
    np_feed(NP, NP_FLOOD);
    np_survivors_in_order(NP, NP_FLOOD, what);
    CHECK(NS.n_notes == CLAP_HOST_NOTES_PER_BLOCK && atomic_load(&NS.notes_dropped) == NP_FLOOD - CLAP_HOST_NOTES_PER_BLOCK,
          "%s: %u note offs: %u delivered, the %u past the block counted (%u, %u)", what, NP_FLOOD, CLAP_HOST_NOTES_PER_BLOCK,
          NP_FLOOD - CLAP_HOST_NOTES_PER_BLOCK, NS.n_notes, atomic_load(&NS.notes_dropped));
    CHECK(atomic_load(&NS.releases_dropped) == NP_FLOOD - CLAP_HOST_NOTES_PER_BLOCK,
          "%s: the note offs past the block are counted as releases (%u)", what, atomic_load(&NS.releases_dropped));

    /* the flood, then exactly enough note offs to fill the reserve but one, then all notes off: the list ends full, on it */
    ns_up(1, DIALECTS[d][0], DIALECTS[d][1]);
    {
      static uint8_t M[NP_FLOOD + OMX_CLAP_NOTES_RESERVED][3];

      for (i = 0; i < NP_FLOOD; i++) { M[i][0] = 0x90; M[i][1] = (uint8_t)(i % 128); M[i][2] = 100; }
      for (i = 0; i < OMX_CLAP_NOTES_RESERVED - 1; i++) { M[NP_FLOOD + i][0] = 0x80; M[NP_FLOOD + i][1] = (uint8_t)i; M[NP_FLOOD + i][2] = 0; }
      M[NP_FLOOD + OMX_CLAP_NOTES_RESERVED - 1][0] = 0xb0;
      M[NP_FLOOD + OMX_CLAP_NOTES_RESERVED - 1][1] = 123;
      M[NP_FLOOD + OMX_CLAP_NOTES_RESERVED - 1][2] = 0;
      np_feed(M, NP_FLOOD + OMX_CLAP_NOTES_RESERVED);
      np_survivors_in_order(M, NP_FLOOD + OMX_CLAP_NOTES_RESERVED, what);
      CHECK(NS.n_notes == CLAP_HOST_NOTES_PER_BLOCK && NS.notes[NS.n_notes - 1].header.time == NP_FLOOD + OMX_CLAP_NOTES_RESERVED - 1 &&
            atomic_load(&NS.releases_dropped) == 0,
            "%s: %u note ons, %u note offs and all notes off: the block is full and ends on all notes off (%u, %u releases dropped)",
            what, NP_FLOOD, OMX_CLAP_NOTES_RESERVED - 1, NS.n_notes, atomic_load(&NS.releases_dropped));
    }

    /* past the flood a pedal pressed, an expression or a program change is not a release: dropped, never a release */
    ns_up(1, DIALECTS[d][0], DIALECTS[d][1]);
    {
      static const uint8_t NOT_RELEASES[][3] = {{0xb0, 64, 127}, {0xe0, 0, 0x40}, {0xd0, 90, 0}, {0xb0, 122, 0}};

      n = np_flood(NP_FLOOD, NOT_RELEASES, 4);
      np_feed(NP, n);
      np_survivors_in_order(NP, n, what);
      CHECK(NS.n_notes == CLAP_HOST_NOTES_PER_BLOCK - OMX_CLAP_NOTES_RESERVED && atomic_load(&NS.releases_dropped) == 0,
            "%s: a pedal pressed, a bend, a pressure and local control after the flood are dropped, none a release (%u, %u)", what,
            NS.n_notes, atomic_load(&NS.releases_dropped));
    }
  }

  /* a CLAP-only port has no MIDI: all notes off is a note off and all sound off a choke, every key of the channel */
  ns_up(1, CLAP_NOTE_DIALECT_CLAP, CLAP_NOTE_DIALECT_CLAP);
  n = np_flood(NP_FLOOD, PANIC, 2);
  np_feed(NP, n);
  CHECK(NS.n_notes >= 2 && NS.notes[NS.n_notes - 2].header.type == CLAP_EVENT_NOTE_OFF && NS.notes[NS.n_notes - 2].note.key == -1 &&
        NS.notes[NS.n_notes - 2].note.channel == 2 && NS.notes[NS.n_notes - 2].note.note_id == -1 &&
        NS.notes[NS.n_notes - 2].note.port_index == 0,
        "CLAP: all notes off is a note off on every key of channel 2");
  CHECK(NS.n_notes >= 1 && NS.notes[NS.n_notes - 1].header.type == CLAP_EVENT_NOTE_CHOKE && NS.notes[NS.n_notes - 1].note.key == -1 &&
        NS.notes[NS.n_notes - 1].note.channel == 2,
        "CLAP: all sound off is a choke on every key of channel 2");

  /* the sustain pedal let go after a flood arrives where MIDI travels */
  ns_up(1, CLAP_NOTE_DIALECT_MIDI, CLAP_NOTE_DIALECT_MIDI);
  n = np_flood(NP_FLOOD, PEDAL_UP, 1);
  np_feed(NP, n);
  np_survivors_in_order(NP, n, "MIDI");
  CHECK(NS.notes[NS.n_notes - 1].header.time == NP_FLOOD && NS.notes[NS.n_notes - 1].midi.data[1] == 64,
        "MIDI: the sustain pedal let go after the flood arrives");
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

/* ---- 15. WIDE AUXILIARY PORTS: every channel of every port its own block ---- */
static void t_aux_bind_wide(void) {
  static struct rig g;
  static struct omx_clap_aux_in a;
  static float blocks[9][MAXB], silence[MAXB], side[6][MAXB];
  float *scratch[9], *bufs[6];
  const struct omx_clap_port outs[3] = {{.channels = 1}, {.channels = 6}, {.channels = 2}};
  const struct omx_clap_port ins[2] = {{.channels = 4}, {.channels = 1}};

  memset(&g, 0, sizeof g);
  memset(&a, 0, sizeof a);
  CHECK(omx_clap_stage_init(&g.st, &(struct omx_hosted_bounce){g.in_l, g.in_r, g.out_l, g.out_r, MAXB}, g.recs, QCAP) == 0, "aux: stage_init");
  for (uint32_t k = 0; k < 9; k++) scratch[k] = blocks[k];
  for (uint32_t k = 0; k < 6; k++) bufs[k] = side[k];  /* six: the 6-channel output reads them all */
  CHECK(omx_clap_bind_aux(&g.st, scratch, 3, outs) == 0, "aux: three outputs of 1, 6 and 2 channels over nine blocks");
  CHECK(g.st.n_aux == 3 && g.st.aout[1].channel_count == 1 && g.st.aout[2].channel_count == 6 && g.st.aout[3].channel_count == 2,
        "aux: the outputs keep their widths (%u, %u, %u)", g.st.aout[1].channel_count, g.st.aout[2].channel_count, g.st.aout[3].channel_count);
  CHECK(g.st.aout[1].data32[0] == scratch[0] && g.st.aout[2].data32[0] == scratch[1] && g.st.aout[2].data32[5] == scratch[6] &&
            g.st.aout[3].data32[0] == scratch[7] && g.st.aout[3].data32[1] == scratch[8],
        "aux: each output channel is its own scratch block, in port order");
  const struct omx_clap_port too_wide = {.channels = CLAP_HOST_PORT_CHANNELS + 1};
  CHECK(omx_clap_bind_aux(&g.st, scratch, 1, &too_wide) == -1, "aux: an output wider than the limit is refused");

  static float *bound[CLAP_HOST_AUX_INPUTS * CLAP_HOST_PORT_CHANNELS];
  const uint32_t base[2] = {0, 4};
  CHECK(omx_clap_bind_aux_in(&a, silence, 2, ins, bound, base) == 0, "aux: two inputs of 4 and 1 channels, none bound");
  CHECK(a.n_aux_in == 2 && a.ains[1].channel_count == 4 && a.ains[2].channel_count == 1, "aux: the inputs keep their widths");
  CHECK(a.ains[1].data32[3] == silence && a.ains[2].data32[0] == silence, "aux: an unbound channel reads the silence");
  omx_clap_set_aux_in(&a, 0, 4, bufs);
  CHECK(a.ains[1].data32[0] == bufs[0] && a.ains[1].data32[3] == bufs[3], "aux: a bound port reads the caller's buffers, channel by channel");
  CHECK(a.ains[2].data32[0] == silence, "aux: and the other port still reads the silence");
  omx_clap_set_aux_in(&a, 0, 4, NULL);
  CHECK(a.ains[1].data32[3] == silence, "aux: unbound again, back to the silence");

  omx_clap_set_aux_out(&g.st, 1, 6, bufs);
  CHECK(g.st.aout[2].data32[3] == bufs[3], "aux: a bound output writes the caller's buffers");
  omx_clap_set_aux_out(&g.st, 1, 6, NULL);
  CHECK(g.st.aout[2].data32[3] == scratch[4], "aux: unbound, back to its scratch");
}

/* ---- the wide path: a 6-channel main pair over blocks of its own, every channel carried, bypass switched hard ---- */
static int wide_wet(float (*outs)[MAXB], float (*ins)[MAXB], float gain) {
  for (uint32_t c = 0; c < 6; c++)
    for (uint32_t i = 0; i < MAXB; i++)
      if (outs[c][i] != ins[c][i] * gain) return 0;
  return 1;
}

static void t_run_ports_wide(void) {
  static struct rig g;
  static float win[6][MAXB], wout[6][MAXB], ins[6][MAXB], outs[6][MAXB];
  float *wi[6], *wo[6], *op[6];
  const float *ip[6];

  memset(&g, 0, sizeof g);
  fake_up(&g.f, K_PAD, 6);
  CHECK(omx_clap_stage_init(&g.st, &(struct omx_hosted_bounce){g.in_l, g.in_r, g.out_l, g.out_r, MAXB}, g.recs, QCAP) == 0, "wide: stage_init");
  CHECK(g.f.plugin.init(&g.f.plugin) && g.f.plugin.activate(&g.f.plugin, (double)g_rate, 1, MAXB), "wide: init, activate");
  for (uint32_t c = 0; c < 6; c++) {
    wi[c] = win[c];
    wo[c] = wout[c];
    ip[c] = ins[c];
    op[c] = outs[c];
    fill(ins[c], MAXB, (float)c * 0.1f);
  }
  CHECK(omx_clap_bind_wide(&g.st, wi, CLAP_HOST_PORT_CHANNELS + 1, wo, 6) == -1, "wide: an input wider than the limit is refused");
  wi[5] = NULL;
  CHECK(omx_clap_bind_wide(&g.st, wi, 6, wo, 6) == -1, "wide: a channel with no block is refused");
  wi[5] = win[5];
  CHECK(g.st.wide == 0, "wide: nothing bound by a refusal");
  CHECK(omx_clap_bind_wide(&g.st, wi, 6, wo, 6) == 0 && g.st.wide == 1, "wide: six blocks a side bind the wide bounce");
  CHECK(omx_clap_bind_ports(&g.st, &g.f.plugin, 6, 6) == 0, "wide: bind the 6-channel pair");
  CHECK(g.st.ain.data32 == g.st.wide_in && g.st.aout[0].data32 == g.st.wide_out && g.st.wide_in[5] == win[5] && g.st.wide_out[5] == wout[5],
        "wide: the main buffers are the wide blocks");
  omx_clap_arm(&g.st);
  in_rt = 1;
  omx_clap_run_ports(&g.st, ip, op, MAXB);
  in_rt = 0;
  CHECK(wide_wet(outs, ins, PAD_GAIN), "wide: every output channel is the gain times its input from the first block (%g, %g)",
        (double)outs[5][0], (double)ins[5][0]);
  omx_clap_set_bypass(&g.st, 1);
  in_rt = 1;
  omx_clap_run_ports(&g.st, ip, op, MAXB);
  in_rt = 0;
  CHECK(wide_wet(outs, ins, 1.0f), "wide: bypass is dry on the next block, no fade (%g, %g)", (double)outs[5][0], (double)ins[5][0]);
  CHECK(g.f.runs == 1 && atomic_load(&g.st.h.runs) == 1, "wide: the plugin idles bypassed (%u runs)", g.f.runs);
  omx_clap_set_bypass(&g.st, 0);
  in_rt = 1;
  omx_clap_run_ports(&g.st, ip, op, MAXB);
  in_rt = 0;
  CHECK(wide_wet(outs, ins, PAD_GAIN), "wide: back to wet on the next block, no fade");

  /* the guards, per channel: a hot block clamped on every channel past the ceiling, a non-finite one delivered dry and struck */
  g.f.kind = K_HOT;
  in_rt = 1;
  omx_clap_run_ports(&g.st, ip, op, MAXB);
  in_rt = 0;
  CHECK(atomic_load(&g.st.h.clamped_samples) == 4 * MAXB, "wide: every sample of the four channels past +24 dBFS clamped (%u)",
        atomic_load(&g.st.h.clamped_samples));
  CHECK(outs[5][0] == omx_db_to_lin(CLAP_HOST_CLAMP_DBFS), "wide: the sixth channel at the ceiling (%g)", (double)outs[5][0]);
  g.f.kind = K_NAN;
  g.f.nan_on = 1;
  in_rt = 1;
  omx_clap_run_ports(&g.st, ip, op, MAXB);
  in_rt = 0;
  CHECK(wide_wet(outs, ins, 1.0f) && atomic_load(&g.st.h.nonfinite_blocks) == 1, "wide: a non-finite block delivers dry and strikes (%u)",
        atomic_load(&g.st.h.nonfinite_blocks));

  /* not yet armed, the wide path carries dry: 2 to 6 copies the second input past it, 0 to 4 (a source) is silent */
  static struct rig d;
  memset(&d, 0, sizeof d);
  fake_up(&d.f, K_PAD, 6);
  CHECK(omx_clap_stage_init(&d.st, &(struct omx_hosted_bounce){d.in_l, d.in_r, d.out_l, d.out_r, MAXB}, d.recs, QCAP) == 0,
        "wide dry: stage_init");
  CHECK(omx_clap_bind_wide(&d.st, wi, 2, wo, 6) == 0 && omx_clap_bind_ports(&d.st, &d.f.plugin, 2, 6) == 0, "wide dry: bind 2 to 6");
  for (uint32_t c = 0; c < 6; c++) fill(outs[c], MAXB, 9.0f);
  in_rt = 1;
  omx_clap_run_ports(&d.st, ip, op, MAXB);
  in_rt = 0;
  int dry = bytes_equal(outs[0], ins[0], MAXB);
  for (uint32_t c = 1; c < 6; c++) dry = dry && bytes_equal(outs[c], ins[1], MAXB);
  CHECK(dry && d.f.runs == 0, "wide dry: 2 to 6 before arm, out 0 is in 0 and every later output in 1 (%g, %g)", (double)outs[0][0],
        (double)outs[5][0]);
  memset(&d, 0, sizeof d);
  fake_up(&d.f, K_PAD, 4);
  CHECK(omx_clap_stage_init(&d.st, &(struct omx_hosted_bounce){d.in_l, d.in_r, d.out_l, d.out_r, MAXB}, d.recs, QCAP) == 0,
        "wide dry: stage_init for a source");
  CHECK(omx_clap_bind_wide(&d.st, NULL, 0, wo, 4) == 0 && omx_clap_bind_ports(&d.st, &d.f.plugin, 0, 4) == 0, "wide dry: bind 0 to 4");
  for (uint32_t c = 0; c < 6; c++) fill(outs[c], MAXB, 9.0f);
  in_rt = 1;
  omx_clap_run_ports(&d.st, NULL, op, MAXB);
  in_rt = 0;
  dry = 1;
  for (uint32_t c = 0; c < 4; c++)
    for (uint32_t i = 0; i < MAXB; i++) dry = dry && outs[c][i] == 0.0f;
  CHECK(dry && outs[4][0] == 9.0f && d.f.runs == 0, "wide dry: a source of four before arm writes silence on its four outputs only");

  /* a stage that is not wide runs the two-leg path, bit for bit as omx_clap_run_io */
  static struct rig a, b;
  static float l[MAXB], r[MAXB], la[MAXB], ra[MAXB], lb[MAXB], rb[MAXB];
  const float *two_in[2] = {l, r};
  float *oa[2] = {la, ra}, *ob[2] = {lb, rb};
  rig_up(&a, K_PAD, 2, QCAP);
  rig_up(&b, K_PAD, 2, QCAP);
  for (int k = 0; k < 3; k++) {
    ramp(l, MAXB);
    fill(r, MAXB, 0.25f);
    in_rt = 1;
    omx_clap_run_ports(&a.st, two_in, oa, MAXB);
    omx_clap_run_io(&b.st, two_in, ob, MAXB);
    in_rt = 0;
    CHECK(a.st.wide == 0 && bytes_equal(la, lb, MAXB) && bytes_equal(ra, rb, MAXB), "wide: a two-leg stage through run_ports is run_io (block %d)", k);
  }
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
    t_midi2();
    t_note_ids();
    t_mpe();
    t_mpe_two_zones();
    t_note_priority();
    t_stale_events();
    t_sysex_pool_resets();
    t_notes_out();
    t_aux_bind_wide();
    t_run_ports_wide();
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
