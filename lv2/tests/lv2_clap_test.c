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
 * lv2_clap_test.c — the CLAP face of the LV2 adapter, libomx-clap-lv2, against the fixture bundles.
 *
 * Through the face itself, with a host of this test's own:
 *   entry     configure once; the entry of a bundle, reference-counted, its factory listing that bundle's plugins
 *             only, each descriptor id its LV2 URI; a bundle that cannot be read refused.
 *   refusals  create_plugin refuses each twin of the fixture with the port reader's code, word for word on clap.log.
 *   fixture   clap.params as the ports declare them (the row, the held bypass, the read-only meter), clap.audio-ports,
 *             -26 dB with the default state, a parameter event read -16 dB from the block that carries it.
 *   latency   latency.get answers the figure of the last activate; a different reading is held for latency_hold_ms
 *             of blocks, then ONE request_restart, and the restart's activate takes it; a reading that moves and comes
 *             back inside the hold asks for nothing.
 *   pad       mix_lv2.test.c's pad read through process(): exactly 0.1 x, the inPlaceBroken twin identical with the
 *             host's input and output the same buffer, MXCSR re-asserted.
 *   worker    the worker gain's -20 dB reaches the block after the one that carried the event, with the worker pinned
 *             by omx_clap_lv2_worker_quiesce; the fake's drain order; a stray respond counted on clap.log.
 *   rt        no allocation inside process().
 *
 * Through libomx-clap-core's CLAP body, the arms of mix_lv2.test.c whose body moved there, every number unchanged:
 * the warm-up (64 blocks, the live counters untouched, a non-finite plugin refused), the first block fading in, the mono
 * mirror, the bypass crossfade and the steady bypass bit-identical, the non-finite strikes and the fault, the clamp,
 * the oversize block.
 *
 *   lv2_clap_test <build/lv2 directory>
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

#include <clap/clap.h>

#include <lv2/urid/urid.h>

#include "../../src/clap_host.h"
#include "omx_clap_lv2.h"
#include "fixtures/lv2_fakes.h"
#include "fixtures/lv2_worker_gain.h"
#include "lv2_test_util.h"

#define FIXTURE_URI "urn:openmixer:test:host-fixture"
#define BLOCK 64u
#define N_BLOCKS 16u
#define MAXB 256u
#define HOLD_MS 250u

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

static char g_fixture[PATH_MAX], g_wg[PATH_MAX], g_fakes[PATH_MAX], g_fakes_so[PATH_MAX + 32], g_wg_so[PATH_MAX + 32];

/* ---- the test's host ---- */

struct th
{
    clap_host_t host;
    char log[4096];
    int restarts, callbacks;
};

static volatile int g_audio;    // this thread holds the audio role while it calls process()

static void th_log(const clap_host_t *host, clap_log_severity severity, const char *msg)
{
    struct th *t = host->host_data;

    (void)severity;
    strncat(t->log, msg, sizeof(t->log) - strlen(t->log) - 2);
    strcat(t->log, "\n");
}

static bool th_is_main(const clap_host_t *host)
{
    (void)host;
    return !g_audio;
}

static bool th_is_audio(const clap_host_t *host)
{
    (void)host;
    return g_audio;
}

static const clap_host_log_t TH_LOG = { th_log };
static const clap_host_thread_check_t TH_THREAD = { th_is_main, th_is_audio };

static const void *th_get_extension(const clap_host_t *host, const char *id)
{
    (void)host;
    if (!strcmp(id, CLAP_EXT_LOG))
        return &TH_LOG;
    if (!strcmp(id, CLAP_EXT_THREAD_CHECK))
        return &TH_THREAD;
    return NULL;
}

static void th_restart(const clap_host_t *host)
{
    ((struct th *)host->host_data)->restarts++;
}

static void th_process(const clap_host_t *host)
{
    (void)host;
}

static void th_callback(const clap_host_t *host)
{
    ((struct th *)host->host_data)->callbacks++;
}

static void th_init(struct th *t)
{
    memset(t, 0, sizeof(*t));
    t->host.clap_version = (clap_version_t)CLAP_VERSION_INIT;
    t->host.host_data = t;
    t->host.name = "lv2_clap_test";
    t->host.vendor = "";
    t->host.url = "";
    t->host.version = "0";
    t->host.get_extension = th_get_extension;
    t->host.request_restart = th_restart;
    t->host.request_process = th_process;
    t->host.request_callback = th_callback;
}

/* ---- events ---- */

struct evlist
{
    clap_input_events_t list;
    clap_event_param_value_t ev[8];
    uint32_t n;
};

static uint32_t ev_size(const clap_input_events_t *list)
{
    return ((const struct evlist *)list->ctx)->n;
}

static const clap_event_header_t *ev_get(const clap_input_events_t *list, uint32_t i)
{
    const struct evlist *e = list->ctx;

    return i < e->n ? &e->ev[i].header : NULL;
}

static bool out_push(const clap_output_events_t *list, const clap_event_header_t *event)
{
    (void)list;
    (void)event;
    return true;
}

static const clap_output_events_t OUT_EVENTS = { NULL, out_push };

static void ev_init(struct evlist *e)
{
    memset(e, 0, sizeof(*e));
    e->list.ctx = e;
    e->list.size = ev_size;
    e->list.get = ev_get;
}

static void ev_param(struct evlist *e, clap_id id, double value)
{
    clap_event_param_value_t *v = &e->ev[e->n++];

    v->header.size = sizeof(*v);
    v->header.time = 0;
    v->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    v->header.type = CLAP_EVENT_PARAM_VALUE;
    v->param_id = id;
    v->note_id = -1;
    v->port_index = -1;
    v->channel = -1;
    v->key = -1;
    v->value = value;
}

/* ---- one plugin through the face ---- */

struct rig
{
    struct th host;
    const clap_plugin_entry_t *entry;
    const clap_plugin_t *plugin;
    const clap_plugin_params_t *params;
    const clap_plugin_latency_t *latency;
    const clap_plugin_audio_ports_t *ports;
    uint32_t legs;
};

static int rig_up(struct rig *g, const char *bundle, const char *uri, double rate, uint32_t max)
{
    const clap_plugin_factory_t *factory;
    char why[OMX_CLAP_LV2_WHY_MAX] = "";

    memset(g, 0, sizeof(*g));
    th_init(&g->host);
    g->entry = omx_clap_lv2_entry(bundle, why);
    factory = g->entry ? g->entry->get_factory(CLAP_PLUGIN_FACTORY_ID) : NULL;
    g->plugin = factory ? factory->create_plugin(factory, &g->host.host, uri) : NULL;
    if (!g->plugin || !g->plugin->init(g->plugin))
    {
        CHECK(0, "rack %s (%s %s)", uri, why, g->host.log);
        return -1;
    }
    g->params = g->plugin->get_extension(g->plugin, CLAP_EXT_PARAMS);
    g->latency = g->plugin->get_extension(g->plugin, CLAP_EXT_LATENCY);
    g->ports = g->plugin->get_extension(g->plugin, CLAP_EXT_AUDIO_PORTS);
    if (rate > 0.0 && (!g->plugin->activate(g->plugin, rate, 1, max) || !g->plugin->start_processing(g->plugin)))
    {
        CHECK(0, "activate %s (%s)", uri, g->host.log);
        return -1;
    }
    if (g->ports)
    {
        clap_audio_port_info_t info;

        g->ports->get(g->plugin, 0, true, &info);
        g->legs = info.channel_count;
    }
    return 0;
}

static void rig_down(struct rig *g)
{
    if (g->plugin)
    {
        g->plugin->deactivate(g->plugin);
        g->plugin->destroy(g->plugin);
    }
    omx_clap_lv2_entry_release(g->entry);
}

/* one process() call: `in` to `out` (the same buffer when the host processes in place), `events` or none */
static clap_process_status process(struct rig *g, const float *in_l, const float *in_r, float *out_l, float *out_r, uint32_t n,
                                   struct evlist *events)
{
    float *ins[2] = { (float *)in_l, (float *)(in_r ? in_r : in_l) }, *outs[2] = { out_l, out_r ? out_r : out_l };
    clap_audio_buffer_t ib = { ins, NULL, g->legs, 0, 0 }, ob = { outs, NULL, g->legs, 0, 0 };
    struct evlist none;
    clap_process_t p;
    clap_process_status st;

    ev_init(&none);
    memset(&p, 0, sizeof(p));
    p.steady_time = -1;
    p.frames_count = n;
    p.audio_inputs = &ib;
    p.audio_outputs = &ob;
    p.audio_inputs_count = 1;
    p.audio_outputs_count = 1;
    p.in_events = events ? &events->list : &none.list;
    p.out_events = &OUT_EVENTS;
    g_audio = 1;
    in_rt = 1;
    st = g->plugin->process(g->plugin, &p);
    in_rt = 0;
    g_audio = 0;
    return st;
}

static double param(struct rig *g, clap_id id)
{
    double v = NAN;

    g->params->get_value(g->plugin, id, &v);
    return v;
}

static struct fake *fake_last(void)
{
    lv2_fakes_last_fn fn;

    *(void **)&fn = loaded_symbol(g_fakes_so, "lv2_fakes_last");
    return fn ? fn() : NULL;
}

static const char *fake_trace(void)
{
    lv2_fakes_trace_fn fn;

    *(void **)&fn = loaded_symbol(g_fakes_so, "lv2_fakes_trace");
    return fn ? fn() : "";
}

static void fake_trace_clear(void)
{
    lv2_fakes_trace_clear_fn fn;

    *(void **)&fn = loaded_symbol(g_fakes_so, "lv2_fakes_trace_clear");
    if (fn)
        fn();
}

/* ---- configure and the entry ---- */

static void unconfigured(void)
{
    char why[OMX_CLAP_LV2_WHY_MAX] = "";
    omx_clap_lv2_config_t config = { omx_clap_lv2_provided, 1000u, 4096u, 0u, HOLD_MS, NULL };
    static const char *const unknown[] = { "urn:openmixer:test:no-such-feature", NULL };

    CHECK(omx_clap_lv2_entry(g_fixture, why) == NULL && strcmp(why, "hosting.no-realisation") == 0, "no entry before the configure (%s)", why);
    CHECK(!lilv_mapped(), "and lilv was not loaded for it");
    config.features = unknown;
    CHECK(omx_clap_lv2_configure(&config, why) == -1, "a feature the adapter has no provider for refuses the configure (%s)", why);
    config.features = omx_clap_lv2_provided;
    CHECK(omx_clap_lv2_configure(&config, why) == 0, "omx_clap_lv2_provided is taken (%s)", why);
    CHECK(omx_clap_lv2_configure(&config, why) == 0, "the same configure again is taken");
    config.latency_hold_ms = HOLD_MS + 1u;
    CHECK(omx_clap_lv2_configure(&config, why) == -1, "a second configure with other numbers is refused (%s)", why);
    config.latency_hold_ms = HOLD_MS;
    config.features = (const char *const[]){ LV2_URID__map, NULL };
    CHECK(omx_clap_lv2_configure(&config, why) == -1, "a second configure with another list is refused (%s)", why);
}

static int has_plugin(const clap_plugin_factory_t *f, const char *uri, const char **name)
{
    uint32_t i;

    for (i = 0; i < f->get_plugin_count(f); i++)
    {
        const clap_plugin_descriptor_t *d = f->get_plugin_descriptor(f, i);

        if (d && strcmp(d->id, uri) == 0)
        {
            if (name)
                *name = d->name;
            return d->features && d->features[0] && strcmp(d->features[0], CLAP_PLUGIN_FEATURE_AUDIO_EFFECT) == 0 && !d->features[1];
        }
    }
    return 0;
}

static void t_entry(void)
{
    char why[OMX_CLAP_LV2_WHY_MAX] = "";
    const clap_plugin_entry_t *a = omx_clap_lv2_entry(g_fixture, why), *b = omx_clap_lv2_entry(g_fixture, why);
    const clap_plugin_entry_t *w = omx_clap_lv2_entry(g_wg, why);
    const clap_plugin_factory_t *fa = a ? a->get_factory(CLAP_PLUGIN_FACTORY_ID) : NULL;
    const clap_plugin_factory_t *fw = w ? w->get_factory(CLAP_PLUGIN_FACTORY_ID) : NULL;
    const char *name = NULL;

    CHECK(a && a == b, "the entry of a bundle, the same entry for a second reference");
    CHECK(a && a->init(g_fixture) && clap_version_is_compatible(a->clap_version), "its init answers and its CLAP version is compatible");
    CHECK(fa && fa->get_plugin_count(fa) == 10, "its factory lists the fixture bundle's 10 plugins (%u)", fa ? fa->get_plugin_count(fa) : 0);
    CHECK(fa && has_plugin(fa, FIXTURE_URI, &name) && name && strcmp(name, "openmixer host fixture") == 0,
          "descriptor id = the LV2 URI, name = doap:name (%s), features: audio-effect only", name ? name : "");
    CHECK(a && a->get_factory("clap.no-such-factory") == NULL, "no factory but the plugin factory");
    CHECK(w && w != a && fw && fw != fa && fw->get_plugin_count(fw) == 1 && has_plugin(fw, LV2_WORKER_GAIN_URI, NULL) && !has_plugin(fa, LV2_WORKER_GAIN_URI, NULL),
          "a second bundle: an entry and a factory of its own, each listing its own bundle's plugins");
    CHECK(omx_clap_lv2_entry("/nonexistent/omx.lv2", why) == NULL && strcmp(why, "hosting.no-realisation") == 0, "a bundle that cannot be read: no entry (%s)", why);
    CHECK(omx_clap_lv2_entry("relative.lv2", why) == NULL, "a bundle path that is not absolute: no entry");
    omx_clap_lv2_entry_release(b);
    CHECK(a->get_factory(CLAP_PLUGIN_FACTORY_ID) == fa, "one release of two: the entry still serves its factory");
    a->deinit();
    omx_clap_lv2_entry_release(a);
    omx_clap_lv2_entry_release(w);
    a = omx_clap_lv2_entry(g_fixture, why);
    fa = a ? a->get_factory(CLAP_PLUGIN_FACTORY_ID) : NULL;
    CHECK(fa && fa->get_plugin_count(fa) == 10, "after the last release the bundle is loaded afresh for the next entry");
    omx_clap_lv2_entry_release(a);
}

static void t_refusals(void)
{
    static const struct { const char *uri, *code; } cases[] =
    {
        { FIXTURE_URI "#atom", "hosting.features.midi-in-fed-empty" },
        { FIXTURE_URI "#cv", "hosting.features.cv-ports" },
        { FIXTURE_URI "#no-in", "hosting.topology.no-audio-input" },
        { FIXTURE_URI "#no-out", "hosting.topology.no-audio-output" },
        { FIXTURE_URI "#three-in", "hosting.topology.extra-inputs-fed-silence" },
        { FIXTURE_URI "#wide", "hosting.topology.wider-than-strip" },
        { FIXTURE_URI "#fixed", "hosting.features.missing" },
        { FIXTURE_URI "#mappath", "hosting.features.missing" },
        { FIXTURE_URI "#makepath", "hosting.features.missing" },
        { "urn:openmixer:test:not-in-this-bundle", "hosting.no-realisation" },
    };
    char why[OMX_CLAP_LV2_WHY_MAX] = "";
    const clap_plugin_entry_t *e = omx_clap_lv2_entry(g_fixture, why);
    const clap_plugin_factory_t *f = e ? e->get_factory(CLAP_PLUGIN_FACTORY_ID) : NULL;
    size_t i;

    for (i = 0; f && i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        char want[128];
        struct th t;

        th_init(&t);
        snprintf(want, sizeof(want), "%s\n", cases[i].code);
        CHECK(f->create_plugin(f, &t.host, cases[i].uri) == NULL && strcmp(t.log, want) == 0,
              "create_plugin refuses %s, and clap.log carries %s word for word (got '%.*s')", cases[i].uri, cases[i].code,
              (int)(strlen(t.log) ? strlen(t.log) - 1 : 0), t.log);
    }
    omx_clap_lv2_entry_release(e);
}

/* ---- the fixture through the face ---- */

static double gain_db(const float *in, const float *out, uint32_t from, uint32_t to)
{
    double ei = 0, eo = 0;
    uint32_t i;

    for (i = from * BLOCK; i < to * BLOCK; i++)
    {
        ei += (double)in[i] * in[i];
        eo += (double)out[i] * out[i];
    }
    return 10.0 * log10(eo / ei);
}

static void t_fixture(double rate)
{
    static float in[N_BLOCKS * BLOCK], out[N_BLOCKS * BLOCK];
    clap_param_info_t info;
    clap_audio_port_info_t port;
    struct evlist ev;
    struct rig g;
    char text[64];
    uint32_t b, hold_blocks;
    double v;

    if (rig_up(&g, g_fixture, FIXTURE_URI, rate, BLOCK) != 0)
        return;
    CHECK(g.params && g.params->count(g.plugin) == 3, "fixture %.0f: three parameters, one per control port but the latency", rate);
    CHECK(g.params->get_info(g.plugin, 0, &info) && info.id == 2 && info.flags == (CLAP_PARAM_IS_BYPASS | CLAP_PARAM_IS_STEPPED) && info.default_value == 1.0,
          "fixture %.0f: lv2:enabled is id 2, IS_BYPASS | IS_STEPPED, held at 1", rate);
    CHECK(g.params->get_info(g.plugin, 1, &info) && info.id == 3 && strcmp(info.name, "Gain") == 0 && info.min_value == -60.0
          && info.max_value == 0.0 && info.default_value == -20.0 && info.flags == CLAP_PARAM_IS_AUTOMATABLE,
          "fixture %.0f: gain_db is id 3 (its port index), \"Gain\", -60..0, default -20", rate);
    CHECK(g.params->get_info(g.plugin, 2, &info) && info.id == 4 && info.flags == CLAP_PARAM_IS_READONLY, "fixture %.0f: the meter is id 4, read-only", rate);
    CHECK(g.params->value_to_text(g.plugin, 3, -20.0, text, sizeof(text)) && strcmp(text, "-20") == 0
          && g.params->text_to_value(g.plugin, 3, "-12.5", &v) && v == -12.5, "fixture %.0f: value and text both ways", rate);
    CHECK(g.ports->count(g.plugin, true) == 1 && g.ports->count(g.plugin, false) == 1 && g.ports->get(g.plugin, 0, false, &port)
          && port.flags == CLAP_AUDIO_PORT_IS_MAIN && port.channel_count == 1 && strcmp(port.port_type, CLAP_PORT_MONO) == 0
          && port.in_place_pair == CLAP_INVALID_ID, "fixture %.0f: one main mono port each way", rate);
    CHECK(g.latency->get(g.plugin) == 0, "fixture %.0f: latency.get answers 0 at the first activate: the port has not been read", rate);
    hold_blocks = (uint32_t)((uint64_t)(rate * HOLD_MS / 1000.0) + BLOCK - 1) / BLOCK;
    for (b = 0; b < N_BLOCKS; b++)
    {
        tone(in + b * BLOCK, BLOCK, 0.5f, b * BLOCK);
        process(&g, in + b * BLOCK, NULL, out + b * BLOCK, NULL, BLOCK, NULL);
    }
    CHECK(fabs(gain_db(in, out, 2, N_BLOCKS) - -26.0) < 0.01, "fixture %.0f: reads %.3f dB: the -20 dB row plus the -6 dB default state", rate, gain_db(in, out, 2, N_BLOCKS));
    CHECK(fabs(param(&g, 4) - 0.5 * pow(10.0, -26.0 / 20.0)) < 0.01, "fixture %.0f: the meter reads back through params (%.4f)", rate, param(&g, 4));
    ev_init(&ev);
    ev_param(&ev, 3, -10.0);
    ev_param(&ev, 2, 0.0);          // the held bypass is never written
    for (b = 0; b < N_BLOCKS; b++)
        process(&g, in + b * BLOCK, NULL, out + b * BLOCK, NULL, BLOCK, b == 0 ? &ev : NULL);
    CHECK(fabs(gain_db(in, out, 0, N_BLOCKS) - -16.0) < 0.01, "fixture %.0f: the event's -10 dB is read from the block that carries it: %.3f dB",
          rate, gain_db(in, out, 0, N_BLOCKS));
    CHECK(param(&g, 3) == -10.0 && param(&g, 2) == 1.0, "fixture %.0f: gain_db reads -10, the bypass still 1", rate);
    for (b = 2u * N_BLOCKS; b < hold_blocks; b++)
        process(&g, in, NULL, out, NULL, BLOCK, NULL);
    CHECK(g.host.restarts == 1, "fixture %.0f: 7 frames held for %u ms of blocks, then one restart asked for (%d)", rate, HOLD_MS, g.host.restarts);
    g.plugin->stop_processing(g.plugin);
    g.plugin->deactivate(g.plugin);
    CHECK(g.plugin->activate(g.plugin, rate, 1, BLOCK) && g.latency->get(g.plugin) == 7, "fixture %.0f: the restart's activate takes 7 frames (%u)", rate, g.latency->get(g.plugin));
    CHECK(param(&g, 3) == -10.0, "fixture %.0f: the row keeps its value across the restart", rate);
    rig_down(&g);
}

/* ---- the port properties as CLAP flags ---- */

static int info_of(struct rig *g, clap_id id, clap_param_info_t *info)
{
    uint32_t i;

    for (i = 0; i < g->params->count(g->plugin); i++)
        if (g->params->get_info(g->plugin, i, info) && info->id == id)
            return 1;
    return 0;
}

static void t_props(void)
{
    const uint32_t A = CLAP_PARAM_IS_AUTOMATABLE, S = CLAP_PARAM_IS_STEPPED;
    clap_param_info_t i;
    struct rig g;
    char text[64];
    double v;

    if (rig_up(&g, g_fakes, FAKE_PROPS, 0.0, MAXB) != 0)
        return;
    CHECK(g.params->count(g.plugin) == 8, "props: eight control inputs, eight parameters (%u)", g.params->count(g.plugin));
    CHECK(info_of(&g, 3, &i) && i.flags == (A | S) && i.min_value == 0.0 && i.max_value == 8.0 && i.default_value == 2.0, "props: lv2:integer is IS_STEPPED");
    CHECK(info_of(&g, 4, &i) && i.flags == (A | S) && i.min_value == 0.0 && i.max_value == 1.0 && i.default_value == 1.0, "props: lv2:toggled is IS_STEPPED over 0..1");
    CHECK(info_of(&g, 5, &i) && i.flags == (A | S | CLAP_PARAM_IS_ENUM), "props: lv2:enumeration with a point on every integer is IS_STEPPED | IS_ENUM");
    CHECK(g.params->value_to_text(g.plugin, 5, 1.0, text, sizeof(text)) && strcmp(text, "mid") == 0
          && g.params->text_to_value(g.plugin, 5, "high", &v) && v == 2.0, "props: its value_to_text is the point's rdfs:label (%s), and back", text);
    CHECK(info_of(&g, 6, &i) && i.flags == (A | S), "props: an enumeration whose points miss integers is IS_STEPPED without IS_ENUM");
    CHECK(g.params->value_to_text(g.plugin, 6, 3.0, text, sizeof(text)) && strcmp(text, "square") == 0
          && g.params->value_to_text(g.plugin, 6, 1.0, text, sizeof(text)) && strcmp(text, "1") == 0, "props: a labelled point reads its label, another value its number");
    CHECK(info_of(&g, 7, &i) && i.flags == (A | CLAP_PARAM_IS_HIDDEN), "props: pprops:notOnGUI is IS_HIDDEN");
    CHECK(info_of(&g, 8, &i) && i.flags == A && i.min_value == 20.0 && i.max_value == 20000.0, "props: pprops:logarithmic changes no CLAP flag (CLAP has none)");
    CHECK(info_of(&g, 9, &i) && i.flags == (A | S), "props: pprops:trigger is IS_STEPPED");
    CHECK(info_of(&g, 10, &i) && i.default_value == -3.0, "props: no lv2:default: the default is the minimum");
    rig_down(&g);
}

/* ---- the latency hold, on the pad ---- */

static void t_latency_hold(double rate)
{
    const uint32_t want = pad_latency_frames(rate), hold = (uint32_t)(rate * HOLD_MS / 1000.0);
    const uint32_t blocks = (hold + BLOCK - 1) / BLOCK;
    float l[BLOCK] = { 0 };
    struct fake *f;
    struct rig g;
    uint32_t b;
    int before;

    if (rig_up(&g, g_fakes, FAKE_PAD, rate, MAXB) != 0 || !(f = fake_last()))
        return;
    for (b = 0; b + 1 < blocks; b++)
        process(&g, l, NULL, l, NULL, BLOCK, NULL);
    before = g.host.restarts;
    process(&g, l, NULL, l, NULL, BLOCK, NULL);
    CHECK(before == 0 && g.host.restarts == 1, "latency %.0f: %u frames read; the restart is asked for on the block that completes %u ms (%u blocks), not before", rate, want, HOLD_MS, blocks);
    process(&g, l, NULL, l, NULL, BLOCK, NULL);
    CHECK(g.host.restarts == 1 && g.latency->get(g.plugin) == 0, "latency %.0f: asked once, and latency.get still answers the old figure", rate);
    g.plugin->deactivate(g.plugin);
    CHECK(g.plugin->activate(g.plugin, rate, 1, MAXB) && g.latency->get(g.plugin) == want, "latency %.0f: the restart takes %u frames (%u)", rate, want, g.latency->get(g.plugin));
    f->latency_bias = 1;
    for (b = 0; b + 1 < blocks; b++)
        process(&g, l, NULL, l, NULL, BLOCK, NULL);
    f->latency_bias = 0;
    for (b = 0; b < 2u * blocks; b++)
        process(&g, l, NULL, l, NULL, BLOCK, NULL);
    CHECK(g.host.restarts == 1, "latency %.0f: a reading that moved and came back inside the hold asked for nothing", rate);
    f->latency_bias = 1;
    for (b = 0; b < blocks; b++)
        process(&g, l, NULL, l, NULL, BLOCK, NULL);
    CHECK(g.host.restarts == 2, "latency %.0f: one that stayed asked again", rate);
    g.plugin->deactivate(g.plugin);
    CHECK(g.plugin->activate(g.plugin, rate, 1, MAXB) && g.latency->get(g.plugin) == want + 1, "latency %.0f: and its restart takes %u (%u)", rate, want + 1, g.latency->get(g.plugin));
    rig_down(&g);
}

/* ---- the pad, the twin, MXCSR through process() ---- */

static void t_pad(double rate)
{
    struct rig a, b;
    float x[128], la[128], lb[128], worst = 0.0f;
    int blk, i;

    if (rig_up(&a, g_fakes, FAKE_PAD, rate, MAXB) != 0 || rig_up(&b, g_fakes, FAKE_BROKEN, rate, MAXB) != 0)
        return;
    for (blk = 0; blk < 3; blk++)
    {
        tone(x, 128, 0.5f, (uint32_t)blk * 128u);
        memcpy(lb, x, sizeof(lb));
        process(&a, x, NULL, la, NULL, 128, NULL);
        process(&b, lb, NULL, lb, NULL, 128, NULL);     // the host's input and output are one buffer
    }
    for (i = 0; i < 128; i++)
        worst = fmaxf(worst, fabsf(la[i] - x[i] * PAD_GAIN));
    CHECK(worst == 0.0f, "pad %.0f: process() reads exactly 0.1 x in, worst |err| %g", rate, worst);
    CHECK(bits_equal(la, lb, 128), "pad %.0f: the inPlaceBroken twin processed in place reads the pad's samples", rate);
    rig_down(&a);
    rig_down(&b);
#if defined(__x86_64__) || defined(__i386__)
    {
        struct rig c;

        if (rig_up(&c, g_fakes, FAKE_CLOBBER, rate, MAXB) == 0)
        {
            _mm_setcsr(_mm_getcsr() | 0x8040u);
            process(&c, x, NULL, la, NULL, 64, NULL);
            CHECK((_mm_getcsr() & 0x8040u) == 0x8040u, "pad %.0f: FTZ/DAZ re-asserted after process() of a plugin that clears them", rate);
        }
        rig_down(&c);
    }
#endif
}

/* ---- the worker through the face ---- */

/* the peak of the output over the peak of the input */
static double max_abs_ratio(const float *y, const float *x)
{
    float a = 0.0f, b = 0.0f;
    uint32_t i;

    for (i = 0; i < BLOCK; i++)
    {
        a = fmaxf(a, fabsf(y[i]));
        b = fmaxf(b, fabsf(x[i]));
    }
    return (double)a / (double)b;
}

static void t_worker(double rate)
{
    float x[BLOCK], y[BLOCK];
    struct lv2_worker_gain_probe *pr;
    struct evlist ev;
    struct rig g;
    double db0, db1;
    int b;

    if (rig_up(&g, g_wg, LV2_WORKER_GAIN_URI, rate, BLOCK) != 0 || !(pr = loaded_symbol(g_wg_so, "lv2_worker_gain_probe")))
        return;
    tone(x, BLOCK, 0.5f, 0);
    for (b = 0; b < 4; b++)
    {
        process(&g, x, NULL, y, NULL, BLOCK, NULL);
        omx_clap_lv2_worker_quiesce(g.plugin);
    }
    ev_init(&ev);
    ev_param(&ev, 2, -20.0);
    process(&g, x, NULL, y, NULL, BLOCK, &ev);
    db0 = 20.0 * log10(max_abs_ratio(y, x));
    omx_clap_lv2_worker_quiesce(g.plugin);
    process(&g, x, NULL, y, NULL, BLOCK, NULL);
    db1 = 20.0 * log10(max_abs_ratio(y, x));
    CHECK(fabs(db0) < 0.01 && fabs(db1 - -20.0) < 0.01,
          "worker %.0f: the block that carries -20 dB schedules it (%.3f dB), the quiesced worker answers, the next block applies it (%.3f dB)", rate, db0, db1);
    CHECK(!pthread_equal(pr->work_thread, pthread_self()), "worker %.0f: work() ran on the adapter's worker thread", rate);
    rig_down(&g);

    if (rig_up(&g, g_fakes, FAKE_WORKER, rate, MAXB) == 0)
    {
        struct fake *f = fake_last();
        float l[32] = { 0 };

        fake_trace_clear();
        process(&g, l, NULL, l, NULL, 32, NULL);
        omx_clap_lv2_worker_quiesce(g.plugin);
        process(&g, l, NULL, l, NULL, 32, NULL);
        CHECK(!strcmp(fake_trace(), "run0,end0,work0,resp0,run1,end1"), "worker %.0f: the drain order through process(): %s", rate, fake_trace());
        omx_clap_lv2_worker_quiesce(g.plugin);
        CHECK(f && f->respond && f->respond(f->respond_handle, 4, "abcd") == LV2_WORKER_ERR_UNKNOWN, "worker %.0f: a respond outside work() is refused", rate);
        process(&g, l, NULL, l, NULL, 32, NULL);
        CHECK(g.host.callbacks > 0, "worker %.0f: the counter moved: the plugin asks for the main thread", rate);
        g.plugin->on_main_thread(g.plugin);
        CHECK(strstr(g.host.log, "respond_strikes 1") != NULL, "worker %.0f: and its clap.log line counts the strike (%s)", rate, g.host.log);

        // reset with the worker live: it is joined before LV2 deactivate and started again after activate
        omx_clap_lv2_worker_quiesce(g.plugin);
        {
            const uint32_t acts = f ? f->activations : 0, deacts = f ? f->deactivations : 0;
            uint32_t seq;

            g.plugin->reset(g.plugin);
            CHECK(f && f->deactivations == deacts + 1 && f->activations == acts + 1, "worker %.0f: reset is one LV2 deactivate and one activate", rate);
            fake_trace_clear();
            seq = f ? f->seq : 0;
            process(&g, l, NULL, l, NULL, 32, NULL);
            omx_clap_lv2_worker_quiesce(g.plugin);
            process(&g, l, NULL, l, NULL, 32, NULL);
            {
                char want[64];

                snprintf(want, sizeof(want), "run%u,end%u,work%u,resp%u,run%u,end%u", seq, seq, seq, seq, seq + 1, seq + 1);
                CHECK(!strcmp(fake_trace(), want), "worker %.0f: after the reset the worker answers again: %s", rate, fake_trace());
            }
        }
        rig_down(&g);
    }
}

/* ---- the CLAP body of libomx-clap-core around the adapter ---- */

static struct omx_clap_instance *core_open(const clap_plugin_entry_t *entry, const char *uri, double rate, char why[OMX_CLAP_WHY_MAX])
{
    struct omx_clap_instance *in = NULL;

    if (omx_clap_host_open_entry(entry, uri, &in, why) != 0)
        return NULL;
    if (omx_clap_host_activate(in, rate, MAXB, why) != 0)
    {
        omx_clap_host_close(in);
        return NULL;
    }
    omx_clap_host_publish(in, pthread_self());
    return in;
}

static void core_run(struct omx_clap_instance *in, float *l, float *r, uint32_t n)
{
    in_rt = 1;
    omx_clap_run(&in->stage, l, r, n);
    in_rt = 0;
}

static void t_core(double rate)
{
    char why[OMX_CLAP_LV2_WHY_MAX] = "", cwhy[OMX_CLAP_WHY_MAX] = "";
    const clap_plugin_entry_t *entry = omx_clap_lv2_entry(g_fakes, why);
    struct omx_clap_instance *in, *bad = NULL;
    float l[64], r[64], x[64], want[64], worst = 0.0f;
    static float big[MAXB + 1], bx[MAXB + 1];
    struct fake *f;
    uint32_t i, k, runs, acts, deacts, resets;
    int blk, all;

    // the warm-up, the first block, the latency taken at the restart after the warm-up
    in = entry ? core_open(entry, FAKE_PAD, rate, cwhy) : NULL;
    f = fake_last();
    CHECK(in && f, "core %.0f: the pad through omx_clap_host_open_entry and activate (%s)", rate, cwhy);
    if (!in || !f)
        return;
    CHECK(f->runs == CLAP_HOST_WARMUP_BLOCKS && atomic_load(&in->stage.h.runs) == 0, "core %.0f: the warm-up ran %u blocks, the live counters untouched", rate, f->runs);
    CHECK(in->stage.h.in_l[0] == 0.0f, "core %.0f: the warm-up ends silent", rate);
    CHECK(omx_clap_host_latency(in) == pad_latency_frames(rate), "core %.0f: the latency the restart after the warm-up took: %u frames", rate, omx_clap_host_latency(in));
    tone(x, 64, 0.5f, 0);
    memcpy(l, x, sizeof(l));
    core_run(in, l, NULL, 64);
    CHECK(l[0] == x[0], "core %.0f: the first block opens dry", rate);
    for (i = 0; i < 64; i++)
        worst = fmaxf(worst, fabsf(l[i] - (x[i] + (x[i] * PAD_GAIN - x[i]) * ((float)i / 64.0f))));
    CHECK(worst < 1e-7f, "core %.0f: and fades in on the ramp (worst %g)", rate, worst);

    // bypass: the closed-form crossfade, then bit-identical and never run, then the fade back
    memcpy(l, x, sizeof(l));
    core_run(in, l, NULL, 64);
    omx_clap_host_bypass(in, 1);
    memcpy(l, x, sizeof(l));
    core_run(in, l, NULL, 64);
    worst = 0.0f;
    for (i = 0; i < 64; i++)
        worst = fmaxf(worst, fabsf(l[i] - (x[i] * PAD_GAIN + (x[i] - x[i] * PAD_GAIN) * ((float)i / 64.0f))));
    CHECK(worst < 1e-7f, "core %.0f: the bypass transition is the closed-form crossfade, worst %g", rate, worst);
    runs = f->runs;
    for (blk = 0, all = 1; blk < 5; blk++)
    {
        tone(x, 64, 0.5f, (uint32_t)blk * 64u);
        memcpy(l, x, sizeof(l));
        core_run(in, l, NULL, 64);
        all &= bits_equal(l, x, 64);
    }
    CHECK(all && f->runs == runs, "core %.0f: steady bypass is bit-identical and never runs the plugin", rate);
    acts = f->activations;
    deacts = f->deactivations;
    resets = atomic_load(&in->stage.resets);
    omx_clap_host_bypass(in, 0);
    CHECK(f->deactivations == deacts + 1 && f->activations == acts + 1 && f->runs == runs
              && atomic_load(&in->stage.resets) == resets + 1,
          "core %.0f: the bypass-off verb resets the plugin itself, before any process(): LV2 deactivate %u -> %u, activate %u -> %u",
          rate, deacts, f->deactivations, acts, f->activations);
    tone(x, 64, 0.5f, 0);
    memcpy(l, x, sizeof(l));
    core_run(in, l, NULL, 64);
    CHECK(fabsf(l[0] - x[0]) < 1e-7f && fabsf(l[63] - (x[63] + (x[63] * PAD_GAIN - x[63]) * 63.0f / 64.0f)) < 1e-7f, "core %.0f: un-bypass fades dry -> wet over one block", rate);

    // a 1x1 plugin on a stereo lane returns its L on both legs
    for (blk = 0; blk < 2; blk++)
    {
        tone(l, 64, 0.25f, 0);
        tone(r, 64, -0.4f, 9);
        core_run(in, l, r, 64);
    }
    tone(x, 64, 0.25f, 0);
    for (i = 0; i < 64; i++)
        want[i] = x[i] * PAD_GAIN;
    CHECK(bits_equal(l, want, 64) && bits_equal(r, want, 64), "core %.0f: a 1x1 plugin on a stereo lane returns its L on both legs", rate);

    // a block past the bounce: passed through, not run, counted
    tone(bx, MAXB + 1, 0.5f, 0);
    memcpy(big, bx, sizeof(big));
    runs = f->runs;
    core_run(in, big, NULL, MAXB + 1);
    CHECK(bits_equal(big, bx, MAXB + 1) && f->runs == runs && atomic_load(&in->stage.h.oversize_blocks) == 1, "core %.0f: a block past the bounce is passed through, not run, counted", rate);
    omx_clap_host_unpublish(in, CLAP_HOST_UNPUBLISH_POLL_US, CLAP_HOST_UNPUBLISH_TIMEOUT_US);
    omx_clap_host_close(in);

    // the mono mirror: a 2x2 plugin on a mono lane sees the lane on both inputs, and only L comes back
    in = core_open(entry, FAKE_PAD2, rate, cwhy);
    f = fake_last();
    if (in && f)
    {
        for (blk = 0; blk < 2; blk++)
        {
            tone(x, 64, 0.25f, 3u + (uint32_t)blk * 64u);
            memcpy(l, x, sizeof(l));
            core_run(in, l, NULL, 64);
        }
        for (i = 0; i < 64; i++)
            want[i] = x[i] * PAD_GAIN;
        CHECK(f->saw_in_r0 == x[0], "core %.0f: the 2x2 plugin's R input carried the mono lane (%g vs %g)", rate, f->saw_in_r0, x[0]);
        CHECK(bits_equal(l, want, 64), "core %.0f: only the plugin's L comes back to the mono lane", rate);
        omx_clap_host_unpublish(in, CLAP_HOST_UNPUBLISH_POLL_US, CLAP_HOST_UNPUBLISH_TIMEOUT_US);
        omx_clap_host_close(in);
    }

    // non-finite: one strike and a recovery that fades back in, then the strike-out and the fault for good
    in = core_open(entry, FAKE_NAN, rate, cwhy);
    f = fake_last();
    if (in && f)
    {
        tone(l, 64, 0.5f, 0);
        core_run(in, l, NULL, 64);
        core_run(in, l, NULL, 64);
        f->nan_on = 1;
        tone(x, 64, 0.5f, 0);
        memcpy(l, x, sizeof(l));
        core_run(in, l, NULL, 64);
        CHECK(bits_equal(l, x, 64) && atomic_load(&in->stage.h.nonfinite_blocks) == 1 && atomic_load(&in->stage.h.fault) == OMX_HOSTED_FAULT_NONE,
              "core %.0f: a NaN block is discarded: the lane bit-identical dry, one strike, not faulted", rate);
        f->nan_on = 0;
        memcpy(l, x, sizeof(l));
        core_run(in, l, NULL, 64);
        CHECK(l[0] == x[0] && fabsf(l[32] - (x[32] + (x[32] * PAD_GAIN - x[32]) * 0.5f)) < 1e-7f, "core %.0f: recovery fades dry -> wet over one block", rate);
        f->nan_on = 1;
        for (k = 1, all = 1; k < CLAP_HOST_NON_FINITE_STRIKES; k++)
        {
            memcpy(l, x, sizeof(l));
            core_run(in, l, NULL, 64);
            all &= bits_equal(l, x, 64);
        }
        CHECK(all && atomic_load(&in->stage.h.fault) == OMX_HOSTED_FAULT_NONFINITE, "core %.0f: faulted at %u strikes, each discarded", rate, CLAP_HOST_NON_FINITE_STRIKES);
        f->nan_on = 0;
        runs = f->runs;
        for (blk = 0, all = 1; blk < 4; blk++)
        {
            memcpy(l, x, sizeof(l));
            core_run(in, l, NULL, 64);
            all &= bits_equal(l, x, 64);
        }
        CHECK(all && f->runs == runs, "core %.0f: faulted stays dry after the plugin recovers, and is never run again", rate);
        omx_clap_host_unpublish(in, CLAP_HOST_UNPUBLISH_POLL_US, CLAP_HOST_UNPUBLISH_TIMEOUT_US);
        omx_clap_host_close(in);
    }

    // the clamp: a +40 dB plugin at 0.5 in is held at the ceiling, counted, never a strike
    in = core_open(entry, FAKE_HOT, rate, cwhy);
    if (in)
    {
        for (blk = 0; blk < 2; blk++)
        {
            for (i = 0; i < 64; i++)
                l[i] = (i & 1) ? -0.5f : 0.5f;
            core_run(in, l, NULL, 64);
        }
        for (i = 0, all = 1; i < 64; i++)
            all &= l[i] == ((i & 1) ? -omx_hosted_db_to_lin(CLAP_HOST_CLAMP_DBFS) : omx_hosted_db_to_lin(CLAP_HOST_CLAMP_DBFS));
        CHECK(all, "core %.0f: every +40 dB sample held at the +%.1f dBFS ceiling", rate, (double)CLAP_HOST_CLAMP_DBFS);
        CHECK(atomic_load(&in->stage.h.clamped_samples) == 128, "core %.0f: 128 clamped samples counted (%u)", rate, atomic_load(&in->stage.h.clamped_samples));
        CHECK(atomic_load(&in->stage.h.nonfinite_blocks) == 0 && atomic_load(&in->stage.h.fault) == OMX_HOSTED_FAULT_NONE, "core %.0f: a clamp is never a strike", rate);
        omx_clap_host_unpublish(in, CLAP_HOST_UNPUBLISH_POLL_US, CLAP_HOST_UNPUBLISH_TIMEOUT_US);
        omx_clap_host_close(in);
    }

    // a plugin non-finite from its first block: the warm-up refuses the activate
    CHECK(omx_clap_host_open_entry(entry, FAKE_NAN_ALWAYS, &bad, cwhy) == 0 && omx_clap_host_activate(bad, rate, MAXB, cwhy) == -1
          && strcmp(cwhy, CLAP_HOST_CODE_OUTPUT_NON_FINITE) == 0, "core %.0f: a non-finite plugin refuses publish (%s)", rate, cwhy);
    if (bad)
        omx_clap_host_close(bad);

    // a refusal reaches the core as no plugin
    CHECK(omx_clap_host_open_entry(entry, "urn:omx:test:none", &bad, cwhy) == -1, "core %.0f: a URI the bundle does not hold opens nothing (%s)", rate, cwhy);
    omx_clap_lv2_entry_release(entry);
}

int main(int argc, char **argv)
{
    omx_clap_lv2_config_t config = { omx_clap_lv2_provided, 1000u, 4096u, 0u, HOLD_MS, NULL };
    char dir[PATH_MAX + 32], build[PATH_MAX], why[OMX_CLAP_LV2_WHY_MAX];
    void *volatile probe;
    size_t r;

    if (argc < 2 || abs_dir(argv[1], build) != 0)
    {
        printf("FAIL usage: lv2_clap_test <build/lv2>\n");
        return 1;
    }
    snprintf(dir, sizeof(dir), "%somx-host-fixture.lv2", build);
    abs_dir(dir, g_fixture);
    snprintf(dir, sizeof(dir), "%somx-worker-gain.lv2", build);
    abs_dir(dir, g_wg);
    snprintf(dir, sizeof(dir), "%somx-lv2-fakes.lv2", build);
    abs_dir(dir, g_fakes);
    snprintf(g_fakes_so, sizeof(g_fakes_so), "%somx-lv2-fakes.so", g_fakes);
    snprintf(g_wg_so, sizeof(g_wg_so), "%somx-worker-gain.so", g_wg);
    in_rt = 1;
    probe = malloc(16);
    free(probe);
    in_rt = 0;
    CHECK(rt_allocs == 2, "allocation witness sees malloc+free (%d)", rt_allocs);
    rt_allocs = 0;

    in_child("configure", unconfigured);
    CHECK(omx_clap_lv2_configure(&config, why) == 0, "configure with omx_clap_lv2_provided, a hold of %u ms (%s)", HOLD_MS, why);
    t_entry();
    t_refusals();
    t_props();
    for (r = 0; r < N_RATES; r++)
    {
        const int before = g_failures;

        t_fixture(RATES[r]);
        t_latency_hold(RATES[r]);
        t_pad(RATES[r]);
        t_worker(RATES[r]);
        t_core(RATES[r]);
        printf("lv2 clap @ %.0f: %d failure(s)\n", RATES[r], g_failures - before);
    }
    CHECK(rt_allocs == 0, "no allocation inside process() or the CLAP body's run (%d)", rt_allocs);
    printf("%s\n", g_failures == 0 ? "lv2 clap test ok" : "lv2 clap test FAILED");
    return g_failures == 0 ? 0 : 1;
}
