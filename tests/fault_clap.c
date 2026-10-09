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
 * fault_clap.c: the qualifier's positive controls, one `.clap` per fault mode, each a plugin
 * that misbehaves in exactly one named way, so the host's qualification is proven to REFUSE it
 * with its named code — and mode 0, a clean -20 dB stereo pad, is proven to qualify.
 *
 *   make fixtures   (tests/fault-<n>.clap for every mode below; the ids stay org.openmixer.fault.<n>, which the
 *                    consumers that load them already name)
 *
 * Modes (OMX_FAULT_MODE):
 *   0 clean          a -20 dB 2x2 audio effect, latency 0 — qualifies
 *   1 instrument     descriptor features name no audio-effect      -> hosting.clap.not-audio-effect
 *   2 init-fails     init() returns false                          -> hosting.clap.headless-failed
 *   3 wide           6-channel main ports                          -> hosting.topology.wider-than-strip
 *   4 no-output      no output port at all                         -> hosting.topology.no-audio-output
 *   5 latency-lie    latency.get says 64, the audio arrives at 0   -> hosting.clap.latency-mismatch
 *   6 process-error  CLAP_PROCESS_ERROR under the sweep            -> hosting.clap.process-error
 *   7 main-call      a [main-thread] host call from process()      -> hosting.clap.thread-violation
 *   8 nan            NaN output under the sweep                    -> hosting.output.non-finite
 *   9 overrun        writes 64 frames PAST the block into the bounce (the guard page catches it)
 *  10 alloc          malloc inside process() (the RT-wrap witness)
 *  11 sidechain      one extra non-main input, fed silence          -> admitted (an auxiliary input)
 *  12 note-in        a note input port                             -> hosting.clap.note-input
 *  13 hang           init() never returns                          -> hosting.stability.crashed-live-on-this-rig (timed open)
 *  14 crash-on-param a 2x2 effect at `in x gain` (param 0, default 0.1) that ABORTS when a gain
 *                    >= 0.9 reaches it after it has processed 200 blocks — a plugin supervisor's
 *                    crash drill: the
 *                    write kills a plugin that has been running, and the same write replayed into
 *                    the fresh respawned plugin does not, so the drill ends with the state back.
 *                    Param 1 (read-only) is its process() call count, so a drill waits for "has
 *                    run 200 blocks" by reading it rather than by a time that holds at one quantum
 *  15 latent         NOT a fault: a -20 dB 2x2 effect that delays its input by exactly the latency it
 *                    reports (64 frames by default) — qualifies, declared = measured. Param 0
 *                    (`Latency`, frames, 0..512) changes it the CLAP way: the write asks the host for
 *                    a restart, and the next activate takes the new figure and tells the host
 *                    `latency.changed()`. The positive control for a latency the plugin announces.
 *  16 tempo          a 2x2 effect at `in x bpm / 1000`, the bpm read from `clap_process.transport`
 *                    when it carries CLAP_TRANSPORT_HAS_TEMPO, and SILENT when the transport is NULL
 *                    — the tempo the host gave it, reported as a level:
 *                    120 bpm is 0.12 (−18.42 dB), 60 bpm 0.06 (−24.44 dB)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <clap/clap.h>

#ifndef OMX_FAULT_MODE
#define OMX_FAULT_MODE 0
#endif
#define STR_(x) #x
#define STR(x) STR_(x)

static const char *const FEATURES_EFFECT[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_STEREO, NULL};
static const char *const FEATURES_INSTRUMENT[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, NULL};

static const clap_plugin_descriptor_t DESC = {
    CLAP_VERSION_INIT, "org.openmixer.fault." STR(OMX_FAULT_MODE), "omx fault " STR(OMX_FAULT_MODE),
    "omx-clap-host", "", "", "", "0", "a qualifier positive control",
    OMX_FAULT_MODE == 1 ? FEATURES_INSTRUMENT : FEATURES_EFFECT};

#define LATENT_MAX 512u
#define LATENT_RING 1024u /* a power of two above LATENT_MAX */

typedef struct {
  clap_plugin_t plugin;
  const clap_host_t *host;
  uint32_t calls;
  double gain; /* mode 14's param 0 */
#if OMX_FAULT_MODE == 15
  uint32_t lat;  /* the latency activate took, which latency.get reports and process applies */
  uint32_t want; /* param 0: the latency the next activate takes */
  uint32_t w;    /* the ring's write head */
  float ring[2][LATENT_RING];
#endif
} F;

#if OMX_FAULT_MODE == 14
/* a gain write reaches the plugin through process() or flush(); a running plugin dies of a hot one */
static void take_params(F *f, const clap_input_events_t *in) {
  if (!in) return;
  for (uint32_t i = 0, n = in->size(in); i < n; i++) {
    const clap_event_header_t *h = in->get(in, i);
    if (h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE) continue;
    const clap_event_param_value_t *v = (const clap_event_param_value_t *)h;
    if (v->param_id != 0) continue;
    if (v->value >= 0.9 && f->calls > 200) abort();
    f->gain = v->value;
  }
}
#endif

#if OMX_FAULT_MODE == 15
/* a latency write: a figure that differs from the one wanted asks for the restart that applies it */
static void take_params(F *f, const clap_input_events_t *in) {
  if (!in) return;
  for (uint32_t i = 0, n = in->size(in); i < n; i++) {
    const clap_event_header_t *h = in->get(in, i);
    if (h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE) continue;
    const clap_event_param_value_t *v = (const clap_event_param_value_t *)h;
    if (v->param_id != 0) continue;
    const double c = v->value < 0.0 ? 0.0 : v->value > LATENT_MAX ? LATENT_MAX : v->value;
    const uint32_t want = (uint32_t)(c + 0.5);
    if (want == f->want) continue;
    f->want = want;
    f->host->request_restart(f->host); /* [thread-safe]: a latency changes only while activating */
  }
}
#endif

static bool p_init(const clap_plugin_t *p) {
  (void)p;
#if OMX_FAULT_MODE == 2
  return false;
#elif OMX_FAULT_MODE == 13
  for (;;) sleep(1);
  return true; /* unreachable: the hang IS the fault */
#else
  return true;
#endif
}
static void p_destroy(const clap_plugin_t *p) { free(p->plugin_data); }
static bool p_activate(const clap_plugin_t *p, double sr, uint32_t a, uint32_t b) {
  (void)sr, (void)a, (void)b;
#if OMX_FAULT_MODE == 15
  F *f = (F *)p->plugin_data;
  memset(f->ring, 0, sizeof f->ring);
  f->w = 0;
  if (f->lat != f->want) {
    f->lat = f->want;
    const clap_host_latency_t *hl = f->host->get_extension(f->host, CLAP_EXT_LATENCY);
    if (hl) hl->changed(f->host); /* [main-thread & being-activated] */
  }
#else
  (void)p;
#endif
  return true;
}
static void p_deactivate(const clap_plugin_t *p) { (void)p; }
static bool p_start(const clap_plugin_t *p) { (void)p; return true; }
static void p_stop(const clap_plugin_t *p) { (void)p; }
static void p_reset(const clap_plugin_t *p) { (void)p; }
static void p_on_main(const clap_plugin_t *p) { (void)p; }

static clap_process_status p_process(const clap_plugin_t *p, const clap_process_t *pr) {
  F *f = (F *)p->plugin_data;
  f->calls++;
  const uint32_t n = pr->frames_count;
  if (pr->audio_outputs_count < 1) return CLAP_PROCESS_CONTINUE;
  const clap_audio_buffer_t *ai = pr->audio_inputs_count ? &pr->audio_inputs[0] : NULL;
  const clap_audio_buffer_t *ao = &pr->audio_outputs[0];
  const uint32_t ch = ao->channel_count;
#if OMX_FAULT_MODE == 14
  take_params(f, pr->in_events);
  const float gain = (float)f->gain;
#elif OMX_FAULT_MODE == 16
  const clap_event_transport_t *t = pr->transport;
  const float gain = t && (t->flags & CLAP_TRANSPORT_HAS_TEMPO) ? (float)(t->tempo / 1000.0) : 0.0f;
#elif OMX_FAULT_MODE == 15
  take_params(f, pr->in_events);
  const float gain = 0.1f;
  if (ch <= 2u && ai && ai->channel_count >= ch) {
    for (uint32_t i = 0; i < n; i++) {
      const uint32_t at = (f->w + i) & (LATENT_RING - 1u);
      const uint32_t from = (f->w + i - f->lat) & (LATENT_RING - 1u);
      for (uint32_t c = 0; c < ch; c++) {
        f->ring[c][at] = ai->data32[c][i];
        ao->data32[c][i] = f->ring[c][from] * gain; /* written before read: lat 0 is a straight pad */
      }
    }
    f->w = (f->w + n) & (LATENT_RING - 1u);
    return CLAP_PROCESS_CONTINUE;
  }
#else
  const float gain = 0.1f;
#endif
#if OMX_FAULT_MODE == 6
  if (f->calls > 100 && f->calls % 3 == 0) return CLAP_PROCESS_ERROR;
#endif
#if OMX_FAULT_MODE == 7
  if (f->calls > 100 && f->calls % 5 == 0) {
    const clap_host_params_t *hp = f->host->get_extension(f->host, CLAP_EXT_PARAMS);
    if (hp) hp->rescan(f->host, CLAP_PARAM_RESCAN_VALUES); /* [main-thread], from the audio thread */
  }
#endif
#if OMX_FAULT_MODE == 10
  { void *w = malloc(16); *(volatile char *)w = 1; free(w); }
#endif
  for (uint32_t c = 0; c < ch; c++) {
    float *out = ao->data32[c];
    const float *in = ai && c < ai->channel_count ? ai->data32[c] : NULL;
    for (uint32_t i = 0; i < n; i++) out[i] = in ? in[i] * gain : 0.0f;
#if OMX_FAULT_MODE == 8
    if (f->calls > 100) out[0] = NAN;
#endif
#if OMX_FAULT_MODE == 9
    for (uint32_t i = n; i < n + 64u; i++) out[i] = 1.0f; /* past the block: the guard page */
#endif
  }
  return CLAP_PROCESS_CONTINUE;
}

/* ---- audio ports ---- */
static uint32_t ap_count(const clap_plugin_t *p, bool is_input) {
  (void)p;
#if OMX_FAULT_MODE == 4
  return is_input ? 1 : 0;
#elif OMX_FAULT_MODE == 11
  return is_input ? 2 : 1;
#else
  (void)is_input;
  return 1;
#endif
}
static bool ap_get(const clap_plugin_t *p, uint32_t index, bool is_input, clap_audio_port_info_t *info) {
  (void)p;
  if (index >= ap_count(p, is_input)) return false;
  memset(info, 0, sizeof *info);
  info->id = index;
  snprintf(info->name, sizeof info->name, "%s%u", is_input ? "in" : "out", index);
  info->flags = index == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0;
#if OMX_FAULT_MODE == 3
  info->channel_count = 6;
  info->port_type = NULL;
#else
  info->channel_count = 2;
  info->port_type = CLAP_PORT_STEREO;
#endif
  info->in_place_pair = CLAP_INVALID_ID;
  return true;
}
static const clap_plugin_audio_ports_t AUDIO_PORTS = {ap_count, ap_get};

/* ---- latency ---- */
static uint32_t lat_get(const clap_plugin_t *p) {
  (void)p;
#if OMX_FAULT_MODE == 5
  return 64;
#elif OMX_FAULT_MODE == 15
  return ((const F *)p->plugin_data)->lat;
#else
  return 0;
#endif
}
static const clap_plugin_latency_t LATENCY = {lat_get};

/* ---- one parameter, so the sweep has something to sweep ---- */
#if OMX_FAULT_MODE == 14
#define PARAM_COUNT 2 /* + param 1, read-only: the process() calls so far */
#else
#define PARAM_COUNT 1
#endif
static uint32_t pa_count(const clap_plugin_t *p) { (void)p; return PARAM_COUNT; }
static bool pa_info(const clap_plugin_t *p, uint32_t i, clap_param_info_t *info) {
  (void)p;
  if (i >= PARAM_COUNT) return false;
  memset(info, 0, sizeof *info);
  if (i == 1) {
    info->id = 1;
    info->flags = CLAP_PARAM_IS_READONLY;
    snprintf(info->name, sizeof info->name, "Calls");
    info->max_value = 4294967295.0;
    return true;
  }
  info->id = 0;
#if OMX_FAULT_MODE == 15
  info->flags = CLAP_PARAM_IS_STEPPED;
  snprintf(info->name, sizeof info->name, "Latency");
  info->min_value = 0.0;
  info->max_value = LATENT_MAX;
  info->default_value = 64.0;
#else
  info->flags = CLAP_PARAM_IS_AUTOMATABLE;
  snprintf(info->name, sizeof info->name, "Gain");
  info->min_value = 0.0;
  info->max_value = 1.0;
  info->default_value = 0.1;
#endif
  return true;
}
static bool pa_value(const clap_plugin_t *p, clap_id id, double *v) {
  if (id >= PARAM_COUNT) return false;
#if OMX_FAULT_MODE == 15
  *v = (double)((const F *)p->plugin_data)->want;
#else
  *v = id == 1 ? (double)((const F *)p->plugin_data)->calls : ((const F *)p->plugin_data)->gain;
#endif
  return true;
}
static bool pa_to_text(const clap_plugin_t *p, clap_id id, double v, char *out, uint32_t cap) { (void)p, (void)id; snprintf(out, cap, "%g", v); return true; }
static bool pa_from_text(const clap_plugin_t *p, clap_id id, const char *t, double *v) { (void)p, (void)id; *v = atof(t); return true; }
static void pa_flush(const clap_plugin_t *p, const clap_input_events_t *in, const clap_output_events_t *out) {
  (void)out;
#if OMX_FAULT_MODE == 14 || OMX_FAULT_MODE == 15
  take_params((F *)p->plugin_data, in);
#else
  (void)p, (void)in;
#endif
}
static const clap_plugin_params_t PARAMS = {pa_count, pa_info, pa_value, pa_to_text, pa_from_text, pa_flush};

#if OMX_FAULT_MODE == 12
static uint32_t np_count(const clap_plugin_t *p, bool is_input) { (void)p; return is_input ? 1 : 0; }
static bool np_get(const clap_plugin_t *p, uint32_t i, bool is_input, clap_note_port_info_t *info) {
  (void)p;
  if (!is_input || i != 0) return false;
  memset(info, 0, sizeof *info);
  info->id = 0;
  info->supported_dialects = info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
  snprintf(info->name, sizeof info->name, "notes");
  return true;
}
static const clap_plugin_note_ports_t NOTE_PORTS = {np_count, np_get};
#endif

static const void *p_get_extension(const clap_plugin_t *p, const char *id) {
  (void)p;
  if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &AUDIO_PORTS;
  if (!strcmp(id, CLAP_EXT_LATENCY)) return &LATENCY;
  if (!strcmp(id, CLAP_EXT_PARAMS)) return &PARAMS;
#if OMX_FAULT_MODE == 12
  if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) return &NOTE_PORTS;
#endif
  return NULL;
}

/* ---- factory / entry ---- */
static uint32_t fac_count(const clap_plugin_factory_t *f) { (void)f; return 1; }
static const clap_plugin_descriptor_t *fac_desc(const clap_plugin_factory_t *f, uint32_t i) { (void)f; return i == 0 ? &DESC : NULL; }
static const clap_plugin_t *fac_create(const clap_plugin_factory_t *fac, const clap_host_t *host, const char *id) {
  (void)fac;
  if (!id || strcmp(id, DESC.id) != 0) return NULL;
  F *f = calloc(1, sizeof *f);
  f->host = host;
  f->gain = 0.1;
#if OMX_FAULT_MODE == 15
  f->lat = f->want = 64u;
#endif
  f->plugin.desc = &DESC;
  f->plugin.plugin_data = f;
  f->plugin.init = p_init;
  f->plugin.destroy = p_destroy;
  f->plugin.activate = p_activate;
  f->plugin.deactivate = p_deactivate;
  f->plugin.start_processing = p_start;
  f->plugin.stop_processing = p_stop;
  f->plugin.reset = p_reset;
  f->plugin.process = p_process;
  f->plugin.get_extension = p_get_extension;
  f->plugin.on_main_thread = p_on_main;
  return &f->plugin;
}
static const clap_plugin_factory_t FACTORY = {fac_count, fac_desc, fac_create};

static bool e_init(const char *path) { (void)path; return true; }
static void e_deinit(void) {}
static const void *e_factory(const char *id) { return strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &FACTORY : NULL; }

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, e_init, e_deinit, e_factory};
