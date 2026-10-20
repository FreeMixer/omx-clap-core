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
 *             back inside the hold asks for nothing. The restart keeps the LV2 instance active, no LV2 deactivate or
 *             activate between; a deactivate no restart asked for is LV2's as well.
 *   pad       the pad read through process(): exactly 0.1 x, the inPlaceBroken twin identical with the
 *             host's input and output the same buffer, MXCSR re-asserted.
 *   worker    the worker gain's -20 dB reaches the block after the one that carried the event, with the worker pinned
 *             by omx_clap_lv2_worker_quiesce; the fake's drain order; a stray respond counted on clap.log.
 *             A reset whose worker cannot start again fails process() and says so on clap.log until the next activate.
 *   atom      the atom messages beside MIDI: the string and path properties written through omx_clap_lv2_patch_text_*
 *             (a patch:Set on the next block), read back from the plugin's patch:Put body and patch:Set, refused past
 *             their limit, while busy and when not listed, a reply past 4095 bytes dropped and counted, sent again to a
 *             new LV2 instance; a numeric reply forged as an atom:Blank read as the parameter's output event, another
 *             object naming the gain ignored, the notify output offered a buffer every block. The MIDI input that also
 *             takes time:Position: the transport's object first, at frame 0, by the change rule, then the MIDI; CLAP
 *             note on, off and choke as MIDI, the velocity scaled and rounded, a wildcard key or channel, another port
 *             and a note expression not taken.
 *   cv        each CV port a mono CLAP audio port, not main, named by its lv2:name, the inputs after the side chains and
 *             the outputs after the main one; the audio and the CV output bit-identical with the fixture's binary run
 *             directly with the same buffers, over blocks of several lengths; an input handed no buffer reads silence;
 *             through the core, the CV input an auxiliary input it binds and the CV output an auxiliary output.
 *   make-path state:makePath under the configured state root: no directory until the plugin asks for a path; a save
 *             copies the file its state names to <dir>.saved and names the copy; the instance's directory removed with
 *             it, the copy kept; a second instance loading the state reads the level back from the copy, out = in x 2.5
 *             on every frame; a path outside the directory and an absolute one answered NULL, nothing written, counted
 *             as make_path_refused 2; a path through a link, a/../b, an overlong one, a request on the audio role and
 *             one through a file refused by code; an instance never saved leaves nothing; stale copies of an earlier process of the
 *             same pid are never taken as an instance's own. Without a root the feature is not offered (a child),
 *             and a relative root refuses the configure (a child).
 *   rt        no allocation inside process().
 *
 * Through libomx-clap-core's CLAP body, the arms that belong to the CLAP body:
 * the warm-up (64 blocks, the live counters untouched, a non-finite plugin refused), the first block fading in, the mono
 * mirror, the bypass crossfade and the steady bypass bit-identical, the non-finite strikes and the fault, the clamp,
 * the oversize block, and the warm-up run at the activated block size, bit-identical with a plain LV2 loop after it,
 * activated at the live block size and at twice it.
 *
 *   lv2_clap_test <build/lv2 directory>
 */
#include <dirent.h>
#include <ftw.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include <stdint.h>
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

#include <clap/clap.h>

#include <lv2/urid/urid.h>
#include <lv2/state/state.h>

#include "../../src/clap_host.h"
#include "omx_clap_lv2.h"
#include "fixtures/lv2_fakes.h"
#include "fixtures/lv2_worker_gain.h"
#include "fixtures/lv2_time_probe.h"
#include "fixtures/lv2_path_probe.h"
#include "fixtures/lv2_atom_probe.h"
#include "fixtures/lv2_host_fixture.h"
#include "fixtures/lv2_patch_fixture.h"
struct lv2_midi_echo_two { unsigned runs, a, b, in; };
#include "lv2_test_util.h"

#define FIXTURE_URI "urn:openmixer:test:host-fixture"
#define FIXTURE_PATCH_URI "urn:openmixer:test:patch-fixture"
#define BLOCK 64u
#define N_BLOCKS 16u
#define MAXB 256u
#define WARMB 512u      // an activation larger than the live blocks (a host that activates at its largest quantum); not
                        // 1024: 64 x 1024 and 64 x 256 frames leave the HOLD_PERIOD of 3 at the same phase
#define HOLD_MS 250u
#define MIDI_URI "urn:openmixer:test:midi-echo"

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

/* ---- a worker thread that cannot start, on demand ---- */
static volatile int g_refuse_threads = 0;
int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{
    return g_refuse_threads ? EAGAIN : __real_pthread_create(t, a, fn, arg);
}

static char g_fixture[PATH_MAX], g_wg[PATH_MAX], g_fakes[PATH_MAX], g_fakes_so[PATH_MAX + 32], g_wg_so[PATH_MAX + 32];
static char g_patch[PATH_MAX], g_patch_so[PATH_MAX + 32];
static char g_midi[PATH_MAX], g_midi_so[PATH_MAX + 32];
static char g_time[PATH_MAX], g_time_so[PATH_MAX + 32];
static char g_sc[PATH_MAX];
static char g_path[PATH_MAX], g_path_so[PATH_MAX + 32], g_root[PATH_MAX + 64], g_build[PATH_MAX], g_fixture_so[PATH_MAX + 32];
static char g_atom[PATH_MAX], g_atom_so[PATH_MAX + 32];
static char g_cv[PATH_MAX], g_cv_so[PATH_MAX + 32];

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

/* the parameter events of the last process() call's output, as the plugin wrote them */
static clap_id g_out_id[8];
static double g_out_value[8];
static uint32_t g_out_n;

static bool out_push(const clap_output_events_t *list, const clap_event_header_t *event)
{
    const clap_event_param_value_t *v = (const clap_event_param_value_t *)event;

    (void)list;
    if (event->type == CLAP_EVENT_PARAM_VALUE && g_out_n < 8u)
    {
        g_out_id[g_out_n] = v->param_id;
        g_out_value[g_out_n++] = v->value;
    }
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
static clap_process_status process_with(struct rig *g, const float *in_l, const float *in_r, float *out_l, float *out_r, uint32_t n,
                                        const clap_input_events_t *in_events, const clap_output_events_t *out_events)
{
    float *ins[2] = { (float *)in_l, (float *)(in_r ? in_r : in_l) }, *outs[2] = { out_l, out_r ? out_r : out_l };
    clap_audio_buffer_t ib = { ins, NULL, g->legs, 0, 0 }, ob = { outs, NULL, g->legs, 0, 0 };
    clap_process_t p;
    clap_process_status st;

    memset(&p, 0, sizeof(p));
    p.steady_time = -1;
    p.frames_count = n;
    p.audio_inputs = &ib;
    p.audio_outputs = &ob;
    p.audio_inputs_count = 1;
    p.audio_outputs_count = 1;
    p.in_events = in_events;
    p.out_events = out_events;
    g_audio = 1;
    in_rt = 1;
    st = g->plugin->process(g->plugin, &p);
    in_rt = 0;
    g_audio = 0;
    return st;
}

static clap_process_status process(struct rig *g, const float *in_l, const float *in_r, float *out_l, float *out_r, uint32_t n,
                                   struct evlist *events)
{
    struct evlist none;

    ev_init(&none);
    return process_with(g, in_l, in_r, out_l, out_r, n, events ? &events->list : &none.list, &OUT_EVENTS);
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

/* the worker fake's gate: held, its work() waits before it traces, so a block's trace holds the RT's events alone (the
 * worker is woken at once by schedule_work, and would otherwise trace beside the block's end_run) */
static void fake_worker_hold(int on)
{
    lv2_fakes_worker_hold_fn fn;

    *(void **)&fn = loaded_symbol(g_fakes_so, "lv2_fakes_worker_hold");
    if (fn)
        fn(on);
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
    omx_clap_lv2_config_t config = { omx_clap_lv2_provided, 1000u, 4096u, 0u, HOLD_MS, NULL, NULL };
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
    CHECK(fa && fa->get_plugin_count(fa) == 14, "its factory lists the fixture bundle's 14 plugins (%u)", fa ? fa->get_plugin_count(fa) : 0);
    CHECK(fa && has_plugin(fa, FIXTURE_URI, &name) && name && strcmp(name, "openmixer host fixture") == 0,
          "descriptor id = the LV2 URI, name = doap:name (%s), features: audio-effect only", name ? name : "");
    CHECK(a && a->get_factory("clap.no-such-factory") == NULL, "no factory but the plugin factory");
    CHECK(w && w != a && fw && fw != fa && fw->get_plugin_count(fw) == 2 && has_plugin(fw, LV2_WORKER_GAIN_URI, NULL) && !has_plugin(fa, LV2_WORKER_GAIN_URI, NULL),
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
    CHECK(fa && fa->get_plugin_count(fa) == 14, "after the last release the bundle is loaded afresh for the next entry");
    omx_clap_lv2_entry_release(a);
}

static void t_refusals(void)
{
    static const struct { const char *uri, *code; } cases[] =
    {
        { FIXTURE_URI "#atom", "hosting.features.atom-in-unfed" },
        { FIXTURE_URI "#ctl-only", "hosting.topology.no-audio-output" },
        { FIXTURE_URI "#ctl-alone", "hosting.topology.no-audio-output" },
        { FIXTURE_URI "#three-in", "hosting.topology.extra-inputs-fed-silence" },
        { FIXTURE_URI "#wide", "hosting.topology.wider-than-strip" },
        { FIXTURE_URI "#fixed", "hosting.features.missing" },
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
        CHECK(strcmp(omx_clap_lv2_last_refusal(), cases[i].code) == 0, "the last refusal reads %s (got '%s')", cases[i].code,
              omx_clap_lv2_last_refusal());
    }
    // a create that succeeds clears it: the code of an earlier add never names a later one
    if (f)
    {
        struct th t;
        const clap_plugin_t *p;

        th_init(&t);
        p = f->create_plugin(f, &t.host, FIXTURE_URI);
        CHECK(p && strcmp(omx_clap_lv2_last_refusal(), "") == 0, "a create that succeeds leaves no last refusal (got '%s')",
              omx_clap_lv2_last_refusal());
        if (p)
            p->destroy(p);
    }
    omx_clap_lv2_entry_release(e);
}

/* a plugin with two MIDI inputs is refused with its code, and the fixture with no MIDI has no note ports */
static void t_midi_refusal(void)
{
    char why[OMX_CLAP_LV2_WHY_MAX] = "";
    const clap_plugin_entry_t *e = omx_clap_lv2_entry(g_midi, why);
    const clap_plugin_factory_t *f = e ? e->get_factory(CLAP_PLUGIN_FACTORY_ID) : NULL;
    struct th t;
    struct rig g;
    const clap_plugin_note_ports_t *np;

    th_init(&t);
    {
        const clap_plugin_t *p = f ? f->create_plugin(f, &t.host, MIDI_URI "#two-in") : NULL;

        CHECK(p != NULL && t.log[0] == '\0', "create_plugin admits a second MIDI input (fed empty), nothing on clap.log (got '%.*s')",
              (int)strcspn(t.log, "\n"), t.log);
        if (p)
            p->destroy(p);
    }
    omx_clap_lv2_entry_release(e);
    if (rig_up(&g, g_fixture, FIXTURE_URI, 0.0, BLOCK) == 0)
    {
        np = g.plugin->get_extension(g.plugin, CLAP_EXT_NOTE_PORTS);
        CHECK(!np || (np->count(g.plugin, true) == 0 && np->count(g.plugin, false) == 0), "the fixture with no MIDI has no note ports");
        rig_down(&g);
    }
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
    CHECK(omx_clap_lv2_param_symbol(g.plugin, 2) && strcmp(omx_clap_lv2_param_symbol(g.plugin, 2), "enabled") == 0
          && omx_clap_lv2_param_symbol(g.plugin, 3) && strcmp(omx_clap_lv2_param_symbol(g.plugin, 3), "gain_db") == 0
          && omx_clap_lv2_param_symbol(g.plugin, 4) && strcmp(omx_clap_lv2_param_symbol(g.plugin, 4), "meter") == 0,
          "fixture %.0f: ids 2, 3 and 4 answer the symbols enabled, gain_db and meter", rate);
    CHECK(!omx_clap_lv2_param_symbol(g.plugin, 0) && !omx_clap_lv2_param_symbol(g.plugin, 5) && !omx_clap_lv2_param_symbol(g.plugin, 99)
          && !omx_clap_lv2_param_symbol(NULL, 3), "fixture %.0f: an audio port, the latency port, an id past the ports and no plugin answer no symbol", rate);
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

/* ---- clap.state over memory: the host's streams ---- */

struct mem_out
{
    clap_ostream_t stream;
    char buf[8192];
    size_t n;
};

struct mem_in
{
    clap_istream_t stream;
    const char *buf;
    size_t n, at;
};

static int64_t mo_write(const clap_ostream_t *s, const void *b, uint64_t size)
{
    struct mem_out *m = s->ctx;

    if (m->n + size > sizeof(m->buf))
        return 0;
    memcpy(m->buf + m->n, b, size);
    m->n += size;
    return (int64_t)size;
}

static int64_t mi_read(const clap_istream_t *s, void *b, uint64_t size)
{
    struct mem_in *m = s->ctx;
    uint64_t take = m->n - m->at < size ? m->n - m->at : size;

    memcpy(b, m->buf + m->at, take);
    m->at += take;
    return (int64_t)take;
}

/* a state followed by pad bytes of blank, or by blanks for ever when pad is UINT64_MAX */
struct pad_in
{
    clap_istream_t stream;
    struct mem_in head;
    uint64_t pad;
};

static int64_t pi_read(const clap_istream_t *s, void *b, uint64_t size)
{
    struct pad_in *p = s->ctx;
    int64_t r = mi_read(&p->head.stream, b, size);

    if (r > 0)
        return r;
    if (size > p->pad)
        size = p->pad;
    memset(b, ' ', size);
    if (p->pad != UINT64_MAX)
        p->pad -= size;
    return (int64_t)size;
}

static void pi_init(struct pad_in *p, const char *buf, size_t n, uint64_t pad)
{
    memset(p, 0, sizeof(*p));
    p->head.buf = buf;
    p->head.n = n;
    p->head.stream.ctx = &p->head;
    p->pad = pad;
    p->stream.ctx = p;
    p->stream.read = pi_read;
}

static void mo_init(struct mem_out *m)
{
    memset(m, 0, sizeof(*m));
    m->stream.ctx = m;
    m->stream.write = mo_write;
}

static void mi_init(struct mem_in *m, const char *buf, size_t n)
{
    memset(m, 0, sizeof(*m));
    m->buf = buf;
    m->n = n;
    m->stream.ctx = m;
    m->stream.read = mi_read;
}

/* the rig stopped, then activated again at the same rate: the restart a load while active asks for */
static void restart(struct rig *g, double rate)
{
    g->plugin->stop_processing(g->plugin);
    g->plugin->deactivate(g->plugin);
    CHECK(g->plugin->activate(g->plugin, rate, 1, BLOCK) && g->plugin->start_processing(g->plugin), "restart at %.0f", rate);
}

static const clap_plugin_state_t *state_of(struct rig *g)
{
    return g->plugin->get_extension(g->plugin, CLAP_EXT_STATE);
}

/* the state a rig is saved to, loaded before its activate, and loaded while active: the same blocks, the same bits; and
 * loaded while a latency restart is pending: applied by it */
static void t_state(double rate)
{
    static float in[N_BLOCKS * BLOCK], a[N_BLOCKS * BLOCK], b[N_BLOCKS * BLOCK], c[N_BLOCKS * BLOCK];
    struct mem_out saved;
    struct mem_in given;
    struct evlist ev;
    struct rig ra, rb, rc, rf;
    uint32_t blk;

    for (blk = 0; blk < N_BLOCKS; blk++)
        tone(in + blk * BLOCK, BLOCK, 0.5f, blk * BLOCK);

    // A: the gain at -10 from the first block, the default state's -6 beside it; saved as it stands
    if (rig_up(&ra, g_fixture, FIXTURE_URI, rate, BLOCK) != 0)
        return;
    ev_init(&ev);
    ev_param(&ev, 3, -10.0);
    for (blk = 0; blk < N_BLOCKS; blk++)
        process(&ra, in + blk * BLOCK, NULL, a + blk * BLOCK, NULL, BLOCK, blk == 0 ? &ev : NULL);
    CHECK(fabs(gain_db(in, a, 0, N_BLOCKS) - -16.0) < 0.01, "state %.0f: A runs at -16 dB: the row at -10, the default state at -6 (%.3f)",
          rate, gain_db(in, a, 0, N_BLOCKS));
    mo_init(&saved);
    CHECK(state_of(&ra) && state_of(&ra)->save(ra.plugin, &saved.stream) && saved.n > 0, "state %.0f: A saves (%zu bytes)", rate, saved.n);
    CHECK(strstr(saved.buf, "gain_db") != NULL && strstr(saved.buf, "offset_db") != NULL, "state %.0f: the save carries the row and the plugin's state", rate);

    // B: the same state loaded before the first activate, applied by that activate
    if (rig_up(&rb, g_fixture, FIXTURE_URI, 0.0, BLOCK) == 0)
    {
        mi_init(&given, saved.buf, saved.n);
        CHECK(state_of(&rb) && state_of(&rb)->load(rb.plugin, &given.stream), "state %.0f: B loads before its activate", rate);
        CHECK(rb.plugin->activate(rb.plugin, rate, 1, BLOCK) && rb.plugin->start_processing(rb.plugin), "state %.0f: B activates", rate);
        for (blk = 0; blk < N_BLOCKS; blk++)
            process(&rb, in + blk * BLOCK, NULL, b + blk * BLOCK, NULL, BLOCK, NULL);
        CHECK(bits_equal(a, b, N_BLOCKS * BLOCK), "state %.0f: B's blocks are A's, bit for bit", rate);
        CHECK(param(&rb, 3) == -10.0, "state %.0f: B reads the gain back at -10 (%.4f)", rate, param(&rb, 3));
        rig_down(&rb);
    }

    // C: the same state loaded while active: held, a restart asked for, applied by the restart's activate
    if (rig_up(&rc, g_fixture, FIXTURE_URI, rate, BLOCK) == 0)
    {
        mi_init(&given, saved.buf, saved.n);
        CHECK(state_of(&rc)->load(rc.plugin, &given.stream), "state %.0f: C loads while active", rate);
        CHECK(rc.host.restarts == 1, "state %.0f: C asks the host for one restart (%d)", rate, rc.host.restarts);
        restart(&rc, rate);
        for (blk = 0; blk < N_BLOCKS; blk++)
            process(&rc, in + blk * BLOCK, NULL, c + blk * BLOCK, NULL, BLOCK, NULL);
        CHECK(bits_equal(a, c, N_BLOCKS * BLOCK), "state %.0f: C's blocks after the restart are A's, bit for bit", rate);
        CHECK(param(&rc, 3) == -10.0, "state %.0f: C reads the gain back at -10 after the restart (%.4f)", rate, param(&rc, 3));

        // D: a text that is not a state is refused, and the held state stays C's
        mi_init(&given, "this is not a turtle state {", strlen("this is not a turtle state {"));
        CHECK(!state_of(&rc)->load(rc.plugin, &given.stream), "state %.0f: a text that does not parse is refused", rate);
        CHECK(rc.host.restarts == 1, "state %.0f: a refused load asks for no restart (%d)", rate, rc.host.restarts);

        // E: a state that runs past 1 MiB is refused, though it parses, and so is a stream that never ends
        {
            struct pad_in long_in;

            pi_init(&long_in, saved.buf, saved.n, 1048576u);
            CHECK(!state_of(&rc)->load(rc.plugin, &long_in.stream), "state %.0f: A's state padded past 1 MiB is refused", rate);
            pi_init(&long_in, saved.buf, saved.n, UINT64_MAX);
            CHECK(!state_of(&rc)->load(rc.plugin, &long_in.stream), "state %.0f: a stream that never ends is refused", rate);
            pi_init(&long_in, saved.buf, saved.n, 1048576u - saved.n);
            CHECK(state_of(&rc)->load(rc.plugin, &long_in.stream), "state %.0f: A's state padded to 1 MiB loads", rate);
        }
        rig_down(&rc);
    }

    // F: the same state loaded while the fixture's latency restart is already asked for: that restart's activate is the
    // full one, the LV2 instance activated again rather than kept, and it applies the state
    if (rig_up(&rf, g_fixture, FIXTURE_URI, rate, BLOCK) == 0)
    {
        const uint32_t hold_blocks = (uint32_t)(((uint64_t)(rate * HOLD_MS / 1000.0) + BLOCK - 1) / BLOCK);

        for (blk = 0; blk <= hold_blocks; blk++)
            process(&rf, in, NULL, c, NULL, BLOCK, NULL);
        CHECK(rf.host.restarts == 1, "state %.0f: F's latency asks for a restart (%d)", rate, rf.host.restarts);
        mi_init(&given, saved.buf, saved.n);
        CHECK(state_of(&rf)->load(rf.plugin, &given.stream), "state %.0f: F loads with that restart pending", rate);
        restart(&rf, rate);
        for (blk = 0; blk < N_BLOCKS; blk++)
            process(&rf, in + blk * BLOCK, NULL, c + blk * BLOCK, NULL, BLOCK, NULL);
        CHECK(fabs(gain_db(in, c, 0, N_BLOCKS) - -16.0) < 0.01 && param(&rf, 3) == -10.0,
              "state %.0f: F runs at -16 dB after the restart, the state applied (%.3f, the gain at %.4f)", rate,
              gain_db(in, c, 0, N_BLOCKS), param(&rf, 3));
        rig_down(&rf);
    }
    rig_down(&ra);
}

/* ---- clap.preset-load: the bundle's pset:Preset, by its URI ---- */

#define PRESET_QUIET "urn:openmixer:test:host-fixture-preset:quiet"        /* offset -3 dB, applies to the fixture */
#define PRESET_WRONG "urn:openmixer:test:host-fixture-preset:quiet-wrong"  /* applies to the CV twin: refused */
#define PRESET_NONE "urn:openmixer:test:host-fixture-preset:nothing"

static const clap_plugin_preset_load_t *preset_of(struct rig *g)
{
    return g->plugin->get_extension(g->plugin, CLAP_EXT_PRESET_LOAD);
}

/* the preset loaded while active (a restart), and before the first activate: the row's -20 with the preset's -3 */
static void t_preset(double rate)
{
    static float in[N_BLOCKS * BLOCK], out[N_BLOCKS * BLOCK];
    const clap_plugin_preset_load_t *pl;
    struct rig g;
    uint32_t blk;

    for (blk = 0; blk < N_BLOCKS; blk++)
        tone(in + blk * BLOCK, BLOCK, 0.5f, blk * BLOCK);

    if (rig_up(&g, g_fixture, FIXTURE_URI, rate, BLOCK) == 0)
    {
        pl = preset_of(&g);
        CHECK(pl && !pl->from_location(g.plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, NULL, PRESET_WRONG) && g.host.restarts == 0,
              "preset %.0f: a preset that applies to another plugin is refused, and asks for no restart", rate);
        CHECK(pl && !pl->from_location(g.plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, NULL, PRESET_NONE),
              "preset %.0f: a preset the bundle does not hold is refused", rate);
        CHECK(pl && pl->from_location(g.plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, NULL, PRESET_QUIET) && g.host.restarts == 1,
              "preset %.0f: quiet loads while active, and asks for one restart (%d)", rate, g.host.restarts);
        restart(&g, rate);
        for (blk = 0; blk < N_BLOCKS; blk++)
            process(&g, in + blk * BLOCK, NULL, out + blk * BLOCK, NULL, BLOCK, NULL);
        CHECK(fabs(gain_db(in, out, 2, N_BLOCKS) - -23.0) < 0.01, "preset %.0f: after the restart the gain reads %.3f dB: -20 plus the preset's -3",
              rate, gain_db(in, out, 2, N_BLOCKS));
        rig_down(&g);
    }

    if (rig_up(&g, g_fixture, FIXTURE_URI, 0.0, BLOCK) == 0)
    {
        pl = preset_of(&g);
        CHECK(pl && pl->from_location(g.plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, NULL, PRESET_QUIET) && g.host.restarts == 0,
              "preset %.0f: quiet loads before the first activate, and asks for no restart", rate);
        CHECK(g.plugin->activate(g.plugin, rate, 1, BLOCK) && g.plugin->start_processing(g.plugin), "preset %.0f: activates", rate);
        for (blk = 0; blk < N_BLOCKS; blk++)
            process(&g, in + blk * BLOCK, NULL, out + blk * BLOCK, NULL, BLOCK, NULL);
        CHECK(fabs(gain_db(in, out, 2, N_BLOCKS) - -23.0) < 0.01, "preset %.0f: the first activate applies it: %.3f dB", rate, gain_db(in, out, 2, N_BLOCKS));
        rig_down(&g);
    }
}

/* ---- the bundle's presets listed: the face's preset-discovery factory, through the core's listing ---- */

#define PRESET_LOUD "urn:openmixer:test:host-fixture-preset:loud"          /* offset +3 dB, applies to the fixture */
#define FIXTURE_CV_URI "urn:openmixer:test:host-fixture#cv"

struct listed_preset
{
    char name[64], key[96], plugin[96], provider[64];
    uint32_t kind, flags, plugins;
    int has_location;
};

struct listed
{
    struct listed_preset p[8];
    int n;
};

static int list_preset(void *ctx, const struct omx_clap_preset *p)
{
    struct listed *l = ctx;

    if (l->n < 8)
    {
        struct listed_preset *e = &l->p[l->n];

        snprintf(e->name, sizeof(e->name), "%s", p->name);
        snprintf(e->key, sizeof(e->key), "%s", p->load_key ? p->load_key : "");
        snprintf(e->plugin, sizeof(e->plugin), "%s", p->plugin_count ? p->plugin_ids[0] : "");
        snprintf(e->provider, sizeof(e->provider), "%s", p->provider_id);
        e->kind = p->location_kind;
        e->flags = p->flags;
        e->plugins = p->plugin_count;
        e->has_location = p->location != NULL;
    }
    l->n++;
    return 0;
}

static const struct listed_preset *listed_key(const struct listed *l, const char *key)
{
    for (int i = 0; i < l->n && i < 8; i++)
        if (!strcmp(l->p[i].key, key))
            return &l->p[i];
    return NULL;
}

/* the fixture bundle lists three presets, quiet and loud for the fixture and one for the CV twin, each by its label with
 * its URI the load key; loud then loads by that key, as a listed preset does */
static void t_preset_list(double rate)
{
    static float in[N_BLOCKS * BLOCK], out[N_BLOCKS * BLOCK];
    const struct listed_preset *quiet, *loud, *wrong;
    const clap_plugin_entry_t *entry;
    const clap_plugin_preset_load_t *pl;
    char why[OMX_CLAP_LV2_WHY_MAX];
    struct listed l;
    struct rig g;
    uint32_t blk;
    int listed;

    entry = omx_clap_lv2_entry(g_fixture, why);
    CHECK(entry != NULL, "preset list: the fixture bundle's entry (%s)", why);
    if (!entry)
        return;
    memset(&l, 0, sizeof(l));
    listed = omx_clap_host_entry_presets(entry, list_preset, &l);
    quiet = listed_key(&l, PRESET_QUIET);
    loud = listed_key(&l, PRESET_LOUD);
    wrong = listed_key(&l, PRESET_WRONG);
    CHECK(listed == 3 && l.n == 3, "preset list: the bundle lists three presets (%d)", listed);
    CHECK(quiet && !strcmp(quiet->name, "quiet") && !strcmp(quiet->plugin, FIXTURE_URI) && quiet->plugins == 1,
          "preset list: quiet, by its label, its URI the load key, for the fixture");
    CHECK(loud && !strcmp(loud->name, "loud") && !strcmp(loud->plugin, FIXTURE_URI), "preset list: loud, for the fixture");
    CHECK(wrong && !strcmp(wrong->name, "quiet, for the CV twin") && !strcmp(wrong->plugin, FIXTURE_CV_URI),
          "preset list: the CV twin's own, for the CV twin");
    CHECK(quiet && loud && wrong && quiet->kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN && !quiet->has_location &&
              loud->kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN && quiet->flags == CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT &&
              !strcmp(quiet->provider, "org.openmixer.lv2.pset"),
          "preset list: the bundle is a PLUGIN location of factory content, through the provider org.openmixer.lv2.pset");
    omx_clap_lv2_entry_release(entry);

    for (blk = 0; blk < N_BLOCKS; blk++)
        tone(in + blk * BLOCK, BLOCK, 0.5f, blk * BLOCK);
    if (rig_up(&g, g_fixture, FIXTURE_URI, 0.0, BLOCK) == 0)
    {
        pl = preset_of(&g);
        CHECK(pl && pl->from_location(g.plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, NULL, PRESET_LOUD),
              "preset list %.0f: loud loads by its listed key", rate);
        CHECK(g.plugin->activate(g.plugin, rate, 1, BLOCK) && g.plugin->start_processing(g.plugin), "preset list %.0f: activates", rate);
        for (blk = 0; blk < N_BLOCKS; blk++)
            process(&g, in + blk * BLOCK, NULL, out + blk * BLOCK, NULL, BLOCK, NULL);
        CHECK(fabs(gain_db(in, out, 2, N_BLOCKS) - -17.0) < 0.01, "preset list %.0f: the gain reads %.3f dB: -20 plus loud's +3", rate,
              gain_db(in, out, 2, N_BLOCKS));
        rig_down(&g);
    }
}

/* ---- the live twin of the worker gain: options:interface and state:threadSafeRestore ---- */

/*
 * A shorter block that fits the buffers is told through the plugin's options:interface and the plugin lives on; a longer
 * one, or a plugin with no such interface, is instantiated again. A state loaded while the plugin runs reaches it at
 * once when it declares state:threadSafeRestore: restore() gets the worker's schedule, the trim lands through the
 * worker, and no restart is asked for.
 */
static double max_abs_ratio(const float *y, const float *x);

static void t_live(double rate)
{
    static float x[BLOCK], y[BLOCK];
    struct lv2_worker_gain_probe *pr;
    struct mem_out saved;
    struct mem_in given;
    struct rig g, w;
    unsigned made;
    int b, callbacks;

    if (rig_up(&g, g_wg, LV2_WORKER_GAIN_LIVE_URI, rate, 512) != 0 || !(pr = loaded_symbol(g_wg_so, "lv2_worker_gain_probe")))
        return;
    made = atomic_load(&pr->instantiate_calls);
    g.plugin->stop_processing(g.plugin);
    g.plugin->deactivate(g.plugin);
    CHECK(g.plugin->activate(g.plugin, rate, 1, 256) && atomic_load(&pr->instantiate_calls) == made && atomic_load(&pr->options_set) == 1
              && pr->max_block_set == 256,
          "live %.0f: a shorter block is told through options:interface, the plugin kept (%u set, max %d)", rate,
          atomic_load(&pr->options_set), pr->max_block_set);
    g.plugin->deactivate(g.plugin);
    CHECK(g.plugin->activate(g.plugin, rate, 1, 256) && atomic_load(&pr->options_set) == 1, "live %.0f: the same block again tells nothing", rate);
    g.plugin->deactivate(g.plugin);
    CHECK(g.plugin->activate(g.plugin, rate, 1, 512) && atomic_load(&pr->instantiate_calls) == made && atomic_load(&pr->options_set) == 2
              && pr->max_block_set == 512,
          "live %.0f: back to the length the buffers hold, told again (%d)", rate, pr->max_block_set);
    g.plugin->deactivate(g.plugin);
    CHECK(g.plugin->activate(g.plugin, rate, 1, 1024) && atomic_load(&pr->instantiate_calls) == made + 1,
          "live %.0f: a longer block than the buffers hold is a new instantiation", rate);
    CHECK(g.plugin->start_processing(g.plugin), "live %.0f: processing", rate);

    // the plain worker gain has no options:interface: a shorter block instantiates it again
    if (rig_up(&w, g_wg, LV2_WORKER_GAIN_URI, rate, 512) == 0)
    {
        made = atomic_load(&pr->instantiate_calls);
        w.plugin->stop_processing(w.plugin);
        w.plugin->deactivate(w.plugin);
        CHECK(w.plugin->activate(w.plugin, rate, 1, 256) && atomic_load(&pr->instantiate_calls) == made + 1,
              "live %.0f: a plugin with no options:interface is instantiated again for a shorter block", rate);
        rig_down(&w);
    }

    // state:threadSafeRestore: a state with a trim of -6 dB loaded while it runs
    tone(x, BLOCK, 0.5f, 0);
    for (b = 0; b < 3; b++)
    {
        process(&g, x, NULL, y, NULL, BLOCK, NULL);
        omx_clap_lv2_worker_quiesce(g.plugin);
    }
    pr->save_trim_db = -6.0f;
    mo_init(&saved);
    CHECK(state_of(&g)->save(g.plugin, &saved.stream) && saved.n > 0, "live %.0f: the state saves", rate);
    process(&g, x, NULL, y, NULL, BLOCK, NULL);
    CHECK(fabs(20.0 * log10(max_abs_ratio(y, x))) < 0.01, "live %.0f: no trim before the load", rate);
    mi_init(&given, saved.buf, saved.n);
    callbacks = g.host.callbacks;
    CHECK(state_of(&g)->load(g.plugin, &given.stream) && g.host.restarts == 0 && atomic_load(&pr->restores) >= 1 && pr->restore_had_schedule,
          "live %.0f: loaded while running, restore() is called at once with the worker's schedule, and no restart is asked for (%d)",
          rate, g.host.restarts);
    omx_clap_lv2_worker_quiesce(g.plugin);
    process(&g, x, NULL, y, NULL, BLOCK, NULL);
    CHECK(fabs(20.0 * log10(max_abs_ratio(y, x)) - -6.0) < 0.01, "live %.0f: the next block plays the trim the worker applied (%.3f dB)", rate,
          20.0 * log10(max_abs_ratio(y, x)));
    CHECK(g.host.callbacks == callbacks, "live %.0f: restore()'s schedule is the restore's, not a stray call counted off the audio role (%d)",
          rate, g.host.callbacks - callbacks);
    rig_down(&g);
}

/* ---- the numeric patch parameters through the face: ids above the ports, a write before the block, the echo after ---- */

/* the parameter named `name`, its info filled: 1 when the plugin lists one */
static int patch_info(struct rig *g, const char *name, clap_param_info_t *info)
{
    uint32_t k;

    for (k = 0; k < g->params->count(g->plugin); k++)
        if (g->params->get_info(g->plugin, k, info) && strcmp(info->name, name) == 0)
            return 1;
    return 0;
}

static void t_patch(double rate)
{
    struct rig g;
    struct evlist e;
    clap_id gain = 0;
    clap_param_info_t i;
    float in[BLOCK], out_l[BLOCK], out_r[BLOCK];
    uint32_t k;
    int halved = 1, echoed = 0;

    if (rig_up(&g, g_patch, FIXTURE_PATCH_URI, rate, MAXB) != 0)
        return;
    CHECK(patch_info(&g, "Gain", &i) && i.id >= 4u, "patch face: Gain is a parameter above the four port indices (%u)", i.id);
    gain = i.id;
    CHECK(i.min_value == 0.0 && i.max_value == 2.0 && i.default_value == 1.0,
          "patch face: Gain's range and default are the patch:writable's, 0..2 and 1");
    CHECK(param(&g, gain) == 1.0, "patch face: the value read is the default 1 (%g)", param(&g, gain));
    for (k = 0; k < BLOCK; k++)
        in[k] = 1.0f;
    ev_init(&e);
    ev_param(&e, gain, 0.5);
    g_out_n = 0;
    process(&g, in, in, out_l, out_r, BLOCK, &e);
    for (k = 0; k < BLOCK; k++)
        halved = halved && out_l[k] == 0.5f;
    CHECK(halved, "patch face: a parameter write of 0.5 lands before the block, so the audio is halved");
    for (k = 0; k < g_out_n; k++)
        echoed += g_out_id[k] == gain && g_out_value[k] == 0.5;
    CHECK(echoed == 1, "patch face: the plugin's patch:Set comes back as one output parameter event for Gain's id (%d of %u events)", echoed, g_out_n);
    rig_down(&g);
}

/* ---- MIDI through the face ---- */

#define MIDI_EVENTS 64u
#define MIDI_SYSEX_MAX 16u
#define MIDI_CAPACITY 42u       // a 3-byte message takes 24 bytes of the 1016 the MIDI buffer holds after its header

union any_event
{
    clap_event_header_t h;
    clap_event_midi_t m;
    clap_event_midi_sysex_t x;
    clap_event_note_t note;
    clap_event_note_expression_t expr;
};

struct midi_in
{
    clap_input_events_t list;
    union any_event ev[MIDI_EVENTS];
    uint32_t n;
};

struct midi_out
{
    clap_output_events_t list;
    union any_event ev[MIDI_EVENTS];
    uint8_t bytes[MIDI_EVENTS][MIDI_SYSEX_MAX];     // a sysex's bytes, copied out of the plugin's buffer
    uint32_t n;
};

/* the fixture's symbols, read back from the loaded binary */
typedef uint32_t (*echo_seen_count_fn)(void);
typedef int (*echo_seen_fn)(uint32_t, int64_t *, uint32_t *, uint8_t *);
typedef void (*echo_extra_fn)(int);

static uint32_t mi_size(const clap_input_events_t *l)
{
    return ((const struct midi_in *)l->ctx)->n;
}

static const clap_event_header_t *mi_get(const clap_input_events_t *l, uint32_t i)
{
    const struct midi_in *m = l->ctx;

    return i < m->n ? &m->ev[i].h : NULL;
}

static void midi_in_init(struct midi_in *m)
{
    memset(m, 0, sizeof(*m));
    m->list.ctx = m;
    m->list.size = mi_size;
    m->list.get = mi_get;
}

static void mi_midi(struct midi_in *m, uint32_t time, const uint8_t *bytes, uint32_t size)
{
    clap_event_midi_t *e = &m->ev[m->n++].m;

    memset(e, 0, sizeof(*e));
    e->header.size = sizeof(*e);
    e->header.time = time;
    e->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e->header.type = CLAP_EVENT_MIDI;
    memcpy(e->data, bytes, size);
}

static void mi_sysex(struct midi_in *m, uint32_t time, const uint8_t *bytes, uint32_t size)
{
    clap_event_midi_sysex_t *e = &m->ev[m->n++].x;

    memset(e, 0, sizeof(*e));
    e->header.size = sizeof(*e);
    e->header.time = time;
    e->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e->header.type = CLAP_EVENT_MIDI_SYSEX;
    e->buffer = bytes;
    e->size = size;
}

/* a CLAP-dialect note on with a wildcard key: it names no MIDI message, so it must not reach the plugin */
static void mi_note(struct midi_in *m, uint32_t time)
{
    clap_event_note_t *e = &m->ev[m->n++].note;

    memset(e, 0, sizeof(*e));
    e->header.size = sizeof(*e);
    e->header.time = time;
    e->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e->header.type = CLAP_EVENT_NOTE_ON;
    e->note_id = -1;
    e->key = -1;
    e->velocity = 0.8;
}

static bool mo_push(const clap_output_events_t *l, const clap_event_header_t *event)
{
    struct midi_out *o = l->ctx;
    union any_event *e;

    if (o->n >= MIDI_EVENTS)
        return false;
    e = &o->ev[o->n];
    if (event->type == CLAP_EVENT_MIDI_SYSEX)
    {
        const clap_event_midi_sysex_t *x = (const clap_event_midi_sysex_t *)event;

        memcpy(o->bytes[o->n], x->buffer, x->size < MIDI_SYSEX_MAX ? x->size : MIDI_SYSEX_MAX);
        e->x = *x;
        e->x.buffer = o->bytes[o->n];
    }
    else if (event->type == CLAP_EVENT_MIDI)
        e->m = *(const clap_event_midi_t *)event;
    else
        e->h = *event;
    o->n++;
    return true;
}

static void midi_out_init(struct midi_out *o)
{
    memset(o, 0, sizeof(*o));
    o->list.ctx = o;
    o->list.try_push = mo_push;
}

/* whether output `i` is the event of `type` at `time` with these bytes: a CLAP_EVENT_MIDI compares its three bytes */
static int out_is(const struct midi_out *o, uint32_t i, uint32_t type, uint32_t time, const uint8_t *bytes, uint32_t size)
{
    const union any_event *e;

    if (i >= o->n)
        return 0;
    e = &o->ev[i];
    if (e->h.type != type || e->h.time != time)
        return 0;
    if (type == CLAP_EVENT_MIDI)
        return memcmp(e->m.data, bytes, 3) == 0;
    return type == CLAP_EVENT_MIDI_SYSEX && e->x.size == size && memcmp(o->bytes[i], bytes, size) == 0;
}

/* the note ports, the messages of every channel status and the system bytes through the fixture's echo, the frames of
 * the block, and nothing carried into the next block; a block past its capacity keeps what fits and counts the rest;
 * and a malformed output of the plugin is not passed on */
/* #132: an instrument, a MIDI input and a stereo output with no audio input, is admitted: its face has no audio input
 * port, one note input, and a note on through it sounds on both channels at the velocity's amplitude; the note off is
 * silence from its frame on */
static void t_instrument(double rate)
{
    static const uint8_t on[3] = { 0x90, 69, 100 }, off[3] = { 0x80, 69, 0 };
    static float out_l[BLOCK], out_r[BLOCK];
    float *outs[2] = { out_l, out_r };
    clap_audio_buffer_t ob = { outs, NULL, 2, 0, 0 };
    const clap_plugin_note_ports_t *np;
    clap_audio_port_info_t info;
    struct midi_in mi;
    struct midi_out mo;
    clap_process_t p;
    struct rig g;
    uint32_t i, all;
    double worst = 0.0;

    if (rig_up(&g, g_midi, MIDI_URI "#synth", rate, BLOCK) != 0)
        return;
    np = g.plugin->get_extension(g.plugin, CLAP_EXT_NOTE_PORTS);
    CHECK(g.ports && g.ports->count(g.plugin, true) == 0 && g.ports->count(g.plugin, false) == 1
              && g.ports->get(g.plugin, 0, false, &info) && info.channel_count == 2 && (info.flags & CLAP_AUDIO_PORT_IS_MAIN)
              && np && np->count(g.plugin, true) == 1,
          "instrument %.0f: no audio input port, a stereo main output and one note input", rate);
    memset(&p, 0, sizeof(p));
    p.steady_time = -1;
    p.frames_count = BLOCK;
    p.audio_outputs = &ob;
    p.audio_outputs_count = 1;
    p.out_events = &OUT_EVENTS;
    midi_in_init(&mi);
    mi_midi(&mi, 8, on, 3);
    mi_midi(&mi, 40, off, 3);
    midi_out_init(&mo);
    p.in_events = &mi.list;
    p.out_events = &mo.list;
    g_audio = 1;
    in_rt = 1;
    CHECK(g.plugin->process(g.plugin, &p) == CLAP_PROCESS_CONTINUE, "instrument %.0f: a block with no audio input runs", rate);
    in_rt = 0;
    g_audio = 0;
    for (i = 0, all = 1; i < BLOCK; i++)
    {
        const double want = i >= 8 && i < 40 ? 100.0 / 127.0 * sin(2.0 * M_PI * 440.0 * (double)(i - 8) / rate) : 0.0;
        const double d = fabs((double)out_l[i] - want);

        worst = d > worst ? d : worst;
        all &= out_l[i] == out_r[i];
    }
    CHECK(worst < 1e-6 && all, "instrument %.0f: A4 at velocity 100 from frame 8 to its note off at 40, on both channels (worst %g)",
          rate, worst);
    rig_down(&g);
}

static void t_midi(double rate)
{
    static const uint8_t note[3] = { 0x90, 0x3c, 0x64 }, pc[3] = { 0xc3, 0x05, 0x00 }, cp[3] = { 0xd5, 0x40, 0x00 };
    static const uint8_t cc[3] = { 0xb7, 0x07, 0x7f }, bend[3] = { 0xe9, 0x00, 0x40 }, noff[3] = { 0x8f, 0x3c, 0x00 };
    static const uint8_t poly[3] = { 0xa2, 0x3c, 0x50 }, rt[3] = { 0xf8, 0x00, 0x00 }, cc0[3] = { 0xb0, 0x01, 0x40 };
    static const uint8_t note91[3] = { 0x91, 0x40, 0x7f }, sysex[6] = { 0xf0, 0x7e, 0x00, 0x06, 0x01, 0xf7 };
    static const uint8_t expect_size[11] = { 3, 2, 2, 3, 3, 3, 3, 6, 1, 3, 3 };
    static const uint32_t expect_frame[11] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 63, 63 };
    static const struct { uint32_t type, time; const uint8_t *bytes; uint32_t size; } want[11] =
    {
        { CLAP_EVENT_MIDI, 0, note, 3 }, { CLAP_EVENT_MIDI, 1, pc, 3 }, { CLAP_EVENT_MIDI, 2, cp, 3 },
        { CLAP_EVENT_MIDI, 3, cc, 3 }, { CLAP_EVENT_MIDI, 4, bend, 3 }, { CLAP_EVENT_MIDI, 5, noff, 3 },
        { CLAP_EVENT_MIDI, 6, poly, 3 }, { CLAP_EVENT_MIDI_SYSEX, 7, sysex, 6 }, { CLAP_EVENT_MIDI, 8, rt, 3 },
        { CLAP_EVENT_MIDI, 63, cc0, 3 }, { CLAP_EVENT_MIDI, 63, note91, 3 },
    };
    static float in[BLOCK], out[BLOCK];
    struct rig g;
    const clap_plugin_note_ports_t *np;
    clap_note_port_info_t info;
    struct midi_in mi;
    struct midi_out mo;
    echo_seen_count_fn seen_count;
    echo_seen_fn seen;
    echo_extra_fn extra;
    uint32_t i, k, all;
    int64_t frame;
    uint32_t size;
    uint8_t bytes[8];

    if (rig_up(&g, g_midi, MIDI_URI, rate, BLOCK) != 0)
        return;
    *(void **)&seen_count = loaded_symbol(g_midi_so, "lv2_midi_echo_seen_count");
    *(void **)&seen = loaded_symbol(g_midi_so, "lv2_midi_echo_seen");
    *(void **)&extra = loaded_symbol(g_midi_so, "lv2_midi_echo_extra");
    np = g.plugin->get_extension(g.plugin, CLAP_EXT_NOTE_PORTS);
    CHECK(np && np->count(g.plugin, true) == 1 && np->count(g.plugin, false) == 1 && np->get(g.plugin, 0, true, &info)
              && info.supported_dialects == (CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_CLAP) && info.preferred_dialect == CLAP_NOTE_DIALECT_MIDI
              && np->get(g.plugin, 0, false, &info) && info.supported_dialects == CLAP_NOTE_DIALECT_MIDI,
          "midi %.0f: one MIDI input, MIDI preferred and CLAP notes taken, and one MIDI output, dialect MIDI and no other", rate);

    // block 1: every channel status, a system realtime byte, a sysex, a CLAP note of no key, not taken, one message at the
    // block's last frame and one past its end, which is held at the last frame
    midi_in_init(&mi);
    mi_midi(&mi, 0, note, 3);
    mi_midi(&mi, 1, pc, 2);
    mi_midi(&mi, 2, cp, 2);
    mi_midi(&mi, 3, cc, 3);
    mi_midi(&mi, 4, bend, 3);
    mi_midi(&mi, 5, noff, 3);
    mi_midi(&mi, 6, poly, 3);
    mi_sysex(&mi, 7, sysex, sizeof(sysex));
    mi_midi(&mi, 8, rt, 1);
    mi_note(&mi, 9);
    mi_midi(&mi, 63, cc0, 3);
    mi_midi(&mi, 200, note91, 3);
    midi_out_init(&mo);
    process_with(&g, in, NULL, out, NULL, BLOCK, &mi.list, &mo.list);
    for (i = 0, all = 1; i < 11 && i < mo.n; i++)
        all &= out_is(&mo, i, want[i].type, want[i].time, want[i].bytes, want[i].size);
    CHECK(mo.n == 11 && all, "midi %.0f: eleven messages out, each at its frame and byte for byte: channel, sysex and realtime alike, the note not taken (%u)",
          rate, mo.n);
    CHECK(seen_count && seen_count() == 11, "midi %.0f: the plugin saw eleven messages (%u)", rate, seen_count ? seen_count() : 0);
    for (k = 0, all = 1; seen && k < 11; k++)
    {
        all &= seen(k, &frame, &size, bytes) == 0 && size == expect_size[k] && frame == (int64_t)expect_frame[k];
        all &= memcmp(bytes, want[k].bytes, size < 8 ? size : 8) == 0;
    }
    CHECK(all, "midi %.0f: each arrives at its length (a program change and a channel pressure two bytes, a sysex six, a realtime byte one)", rate);

    // block 2: nothing carries over, neither the input nor the output
    midi_in_init(&mi);
    midi_out_init(&mo);
    process_with(&g, in, NULL, out, NULL, BLOCK, &mi.list, &mo.list);
    CHECK(mo.n == 0 && seen_count && seen_count() == 0, "midi %.0f: a block with no input writes and sees nothing (%u out)", rate, mo.n);

    // block 3: 50 messages into a buffer for 42: the first 42 are kept, the other 8 counted by the core
    midi_in_init(&mi);
    for (i = 0; i < MIDI_CAPACITY + 8u; i++)
        mi_midi(&mi, i, note, 3);
    midi_out_init(&mo);
    process_with(&g, in, NULL, out, NULL, BLOCK, &mi.list, &mo.list);
    g.plugin->on_main_thread(g.plugin);
    CHECK(mo.n == MIDI_CAPACITY && mo.ev[MIDI_CAPACITY - 1u].h.time == MIDI_CAPACITY - 1u && strstr(g.host.log, "midi_in_dropped 8") != NULL,
          "midi %.0f: %u messages fit, the 8 past them are dropped and logged as midi_in_dropped 8 (%u)", rate, MIDI_CAPACITY, mo.n);

    // block 4: a malformed output of the plugin, one kind at a time, is not passed on
    if (extra)
    {
        static const int kinds[4] = { 1, 2, 3, 4 };    // EXTRA_4_BYTES, EXTRA_EMPTY, EXTRA_DATA_FIRST, EXTRA_BEATS

        for (k = 0; k < 4; k++)
        {
            midi_in_init(&mi);
            midi_out_init(&mo);
            extra(kinds[k]);
            process_with(&g, in, NULL, out, NULL, BLOCK, &mi.list, &mo.list);
            CHECK(mo.n == 0, "midi %.0f: malformed output kind %d is not passed on (%u out)", rate, kinds[k], mo.n);
        }
        extra(0);
    }
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

    /* the contract the TTL states: units, the log scale, the steps */
    {
        const omx_clap_plugin_param_contract_ext_t *pc = g.plugin->get_extension(g.plugin, OMX_CLAP_EXT_PARAM_CONTRACT);
        omx_clap_param_contract_entry_t e;

        CHECK(pc && pc->version(g.plugin) == 1 && strlen(pc->fingerprint(g.plugin)) == 64 && pc->migration_count(g.plugin) == 0,
              "props: the face offers org.openmixer.param-contract/1, version 1, a fingerprint, no migration");
        CHECK(pc && pc->get(g.plugin, 8, &e) && !strcmp(e.symbol, "freq") && e.unit == 16 && e.scale == 1 && e.step == 0.0,
              "props: freq is units:hz (16) on a log scale, continuous (%s %u %u %g)", pc ? e.symbol : "", pc ? e.unit : 0, pc ? e.scale : 0,
              pc ? e.step : -1.0);
        CHECK(pc && pc->get(g.plugin, 10, &e) && e.unit == 13 && e.scale == 0 && e.step == 1.0,
              "props: nodef is units:db (13), linear, its 7 rangeSteps over -3..3 a step of 1 (%u %u %g)", pc ? e.unit : 0,
              pc ? e.scale : 0, pc ? e.step : -1.0);
        CHECK(pc && pc->get(g.plugin, 3, &e) && !strcmp(e.symbol, "steps") && e.unit == 0 && e.scale == 2 && e.step == 1.0,
              "props: an integer port is stepped, step 1, no unit");
        CHECK(pc && !pc->get(g.plugin, 2, &e), "props: the latency port is no parameter and has no entry");
    }

    /* pprops:trigger: set for one block, back at its default after it, and the host told so */
    {
        static float in[BLOCK], out[BLOCK];
        struct evlist ev;

        CHECK(g.plugin->activate(g.plugin, 48000.0, 1, MAXB) && g.plugin->start_processing(g.plugin), "props: activates");
        ev_init(&ev);
        ev_param(&ev, 9, 1.0);
        g_out_n = 0;
        process(&g, in, NULL, out, NULL, BLOCK, &ev);
        CHECK(g.params->get_value(g.plugin, 9, &v) && v == 0.0, "props: a trigger set for a block is back at its default after it (%g)", v);
        CHECK(g_out_n == 1 && g_out_id[0] == 9 && g_out_value[0] == 0.0, "props: and the host is told through one parameter event (%u)",
              g_out_n);
        g_out_n = 0;
        process(&g, in, NULL, out, NULL, BLOCK, NULL);
        CHECK(g_out_n == 0, "props: a trigger at rest tells nothing (%u)", g_out_n);
        g.plugin->stop_processing(g.plugin);
        g.plugin->deactivate(g.plugin);
    }
    rig_down(&g);

    /* a control whose symbol is longer than a contract symbol holds: the face offers no contract at all */
    if (rig_up(&g, g_fakes, FAKE_LONGSYM, 0.0, MAXB) == 0)
    {
        CHECK(g.params->count(g.plugin) == 1 && g.plugin->get_extension(g.plugin, OMX_CLAP_EXT_PARAM_CONTRACT) == NULL,
              "props: a symbol past 63 bytes leaves the face with no contract, rather than one the host refuses");
        rig_down(&g);
    }

    /* through the core: the contract holds R1 to R5, and a parameter's meaning carries the unit and the scale */
    {
        char why[OMX_CLAP_LV2_WHY_MAX], cwhy[OMX_CLAP_WHY_MAX];
        const clap_plugin_entry_t *entry = omx_clap_lv2_entry(g_fakes, why);
        struct omx_clap_instance *in = NULL;
        struct omx_clap_param_meaning m;
        const char *rule = NULL;
        uint32_t version = 0;

        CHECK(entry && omx_clap_host_open_entry(entry, FAKE_PROPS, &in, cwhy) == 0, "props: the core opens the props fake (%s)", cwhy);
        if (in)
        {
            CHECK(omx_clap_host_param_contract(in, &version, &rule) == 1 && version == 1,
                  "props: the core trusts the face's contract, version 1 (%s)", rule ? rule : "");
            CHECK(omx_clap_host_param_meaning(in, 8, &m) == 0 && m.unit == 16 && m.scale == 1 && !strcmp(omx_clap_param_unit_name(m.unit), "hz"),
                  "props: the meaning of freq through the core: hz, log");
            omx_clap_host_close(in);
        }
        if (entry)
            omx_clap_lv2_entry_release(entry);
    }
}

/* ---- the latency hold, on the pad ---- */

/* bufsz:fixedBlockLength: declared by the host, the twin requiring it is admitted, a block of the activation's length
 * runs, one of another length is refused and never reaches the plugin, and under the defaults the twin is refused */
static void t_fixed_block(double rate)
{
    struct omx_lv2_feature_options o;
    struct rig g;
    float in[BLOCK], out[BLOCK];
    char why[OMX_CLAP_LV2_WHY_MAX] = "";
    uint32_t i;

    omx_lv2_feature_options_init(&o);
    o.block.declares |= OMX_LV2_BLOCK_FIXED;
    CHECK(omx_clap_lv2_feature_options(&o, why) == 0, "fixed %.0f: the options declare a fixed block (%s)", rate, why);
    if (rig_up(&g, g_fixture, FIXTURE_URI "#fixed", rate, BLOCK) == 0)
    {
        for (i = 0; i < BLOCK; i++)
            in[i] = 0.25f, out[i] = -7.0f;
        CHECK(process_with(&g, in, NULL, out, NULL, BLOCK, NULL, NULL) == CLAP_PROCESS_CONTINUE && out[0] != -7.0f,
              "fixed %.0f: the twin requiring fixedBlockLength is admitted and a block of the activation's length runs", rate);
        for (i = 0; i < BLOCK; i++)
            out[i] = -7.0f;
        CHECK(process_with(&g, in, NULL, out, NULL, BLOCK / 2, NULL, NULL) == CLAP_PROCESS_ERROR && out[0] == -7.0f,
              "fixed %.0f: a block of another length is refused and the plugin never runs on it (out %g)", rate, out[0]);
        // the host's cycle shrank: it re-activates at the new length, and that length runs while the old one is refused
        g.plugin->stop_processing(g.plugin);
        g.plugin->deactivate(g.plugin);
        CHECK(g.plugin->activate(g.plugin, rate, 1, BLOCK / 2) && g.plugin->start_processing(g.plugin), "fixed %.0f: re-activated at half the block", rate);
        for (i = 0; i < BLOCK; i++)
            out[i] = -7.0f;
        CHECK(process_with(&g, in, NULL, out, NULL, BLOCK / 2, NULL, NULL) == CLAP_PROCESS_CONTINUE && out[0] == 0.25f,
              "fixed %.0f: after the shrink the new length runs (out %g)", rate, out[0]);
        for (i = 0; i < BLOCK; i++)
            out[i] = -7.0f;
        CHECK(process_with(&g, in, NULL, out, NULL, BLOCK, NULL, NULL) == CLAP_PROCESS_ERROR && out[0] == -7.0f,
              "fixed %.0f: and the old length is refused now (out %g)", rate, out[0]);
        rig_down(&g);
    }
    CHECK(omx_clap_lv2_feature_options(NULL, why) == 0, "fixed %.0f: the defaults again (%s)", rate, why);
    {
        const clap_plugin_entry_t *e = omx_clap_lv2_entry(g_fixture, why);
        const clap_plugin_factory_t *f = e ? e->get_factory(CLAP_PLUGIN_FACTORY_ID) : NULL;
        struct th t;

        th_init(&t);
        CHECK(f && f->create_plugin(f, &t.host, FIXTURE_URI "#fixed") == NULL
              && strcmp(omx_clap_lv2_last_refusal(), "hosting.features.missing") == 0,
              "fixed %.0f: under the defaults the twin is refused hosting.features.missing again", rate);
        omx_clap_lv2_entry_release(e);
    }
}

/* lv2:inPlaceBroken as a requirement: admitted under the defaults, and the twin's witness sees distinct buffers on
 * every run, also when the host itself processes in place (one buffer in and out): the adapter copies both ways */
static void t_inplace(double rate)
{
    struct lv2_host_fixture_inplace *w = NULL;
    struct rig g;
    float in[BLOCK], out[BLOCK];
    uint32_t i;

    if (rig_up(&g, g_fixture, FIXTURE_URI "#inplace", rate, BLOCK) != 0)
        return;
    *(void **)&w = loaded_symbol(g_fixture_so, "lv2_host_fixture_inplace");
    CHECK(w != NULL, "inplace %.0f: the twin's witness is at hand", rate);
    if (w)
        w->runs = w->aliased = 0;
    for (i = 0; i < BLOCK; i++)
        in[i] = 0.5f, out[i] = 0.0f;
    CHECK(process_with(&g, in, NULL, out, NULL, BLOCK, NULL, NULL) == CLAP_PROCESS_CONTINUE && out[0] == 0.5f,
          "inplace %.0f: the twin requiring inPlaceBroken is admitted and runs (out %g)", rate, out[0]);
    CHECK(process_with(&g, in, NULL, in, NULL, BLOCK, NULL, NULL) == CLAP_PROCESS_CONTINUE,
          "inplace %.0f: the host processing in place runs too", rate);
    CHECK(w && w->runs == 2 && w->aliased == 0,
          "inplace %.0f: the plugin saw two runs and never one buffer as input and output (runs %u aliased %u)", rate,
          w ? w->runs : 0, w ? w->aliased : 0);
    rig_down(&g);
}

/* a two-event list: one parameter value, then one MIDI message */
struct two_events { clap_input_events_t list; clap_event_param_value_t p; clap_event_midi_t m; };
static uint32_t two_size(const clap_input_events_t *l) { (void)l; return 2; }
static const clap_event_header_t *two_get(const clap_input_events_t *l, uint32_t i)
{
    const struct two_events *b = l->ctx;

    return i == 0 ? &b->p.header : i == 1 ? &b->m.header : NULL;
}

/* one atom input for MIDI and patch (the patch fixture's #midi twin): admitted, and a block carrying a parameter write
 * and a note reaches the plugin as ONE sequence, the patch:Set at frame 0 before the note at its frame, in order */
static void t_midi_patch(double rate)
{
    struct two_events both;
    struct lv2_patch_fixture_midi *w = NULL;
    clap_param_info_t info;
    struct rig g;
    float in[BLOCK], out[BLOCK];
    uint32_t i;

    if (rig_up(&g, g_patch, FIXTURE_PATCH_URI "#midi", rate, BLOCK) != 0)
        return;
    *(void **)&w = loaded_symbol(g_patch_so, "lv2_patch_fixture_midi");
    CHECK(w != NULL && patch_info(&g, "Gain", &info), "midi+patch %.0f: the twin is admitted, its witness and its Gain at hand", rate);
    if (!w)
    {
        rig_down(&g);
        return;
    }
    memset(&both, 0, sizeof(both));
    both.p.header.size = sizeof(both.p);
    both.p.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    both.p.header.type = CLAP_EVENT_PARAM_VALUE;
    both.p.param_id = info.id;
    both.p.note_id = -1;
    both.p.port_index = -1;
    both.p.channel = -1;
    both.p.key = -1;
    both.p.value = 0.5;
    both.m.header.size = sizeof(both.m);
    both.m.header.time = 10;
    both.m.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    both.m.header.type = CLAP_EVENT_MIDI;
    both.m.data[0] = 0x90;
    both.m.data[1] = 60;
    both.m.data[2] = 100;
    both.list.ctx = &both;
    both.list.size = two_size;
    both.list.get = two_get;
    for (i = 0; i < BLOCK; i++)
        in[i] = 0.25f, out[i] = 0.0f;
    // the first block carries every property's initial value as the parameter path always does; the measured block is the next
    process_with(&g, in, NULL, out, NULL, BLOCK, NULL, NULL);
    w->runs = w->out_of_order = 0;
    CHECK(process_with(&g, in, NULL, out, NULL, BLOCK, &both.list, NULL) == CLAP_PROCESS_CONTINUE && out[0] == 0.25f,
          "midi+patch %.0f: the block runs through the twin", rate);
    CHECK(w->runs == 1 && w->patch == 1 && w->midi == 1,
          "midi+patch %.0f: one run saw one patch:Set and one MIDI message in one sequence (runs %u patch %u midi %u of %u)",
          rate, w->runs, w->patch, w->midi, w->events);
    CHECK(w->out_of_order == 0 && w->first_patch_frame == 0 && w->first_midi_frame == 10,
          "midi+patch %.0f: in frame order, the patch:Set at 0 before the note at 10 (order faults %u, patch %lld, midi %lld)",
          rate, w->out_of_order, (long long)w->first_patch_frame, (long long)w->first_midi_frame);
    rig_down(&g);
}

/* more than one MIDI port (#199): the first input is the note path's and the second is fed an empty sequence; the
 * first output is the note output's and the second is drained, so an event the plugin writes to both reaches the host once */
static void t_midi_extra(double rate)
{
    struct lv2_midi_echo_two *w = NULL;
    struct midi_in mi;
    struct midi_out mo;
    struct rig g;
    float in[BLOCK], out[BLOCK];
    static const uint8_t note[3] = { 0x90, 60, 100 };
    uint32_t i;

    for (i = 0; i < BLOCK; i++)
        in[i] = 0.5f, out[i] = 0.0f;
    if (rig_up(&g, g_midi, MIDI_URI "#two-in", rate, BLOCK) == 0)
    {
        *(void **)&w = loaded_symbol(g_midi_so, "lv2_midi_echo_two");
        midi_in_init(&mi);
        mi_midi(&mi, 3, note, 3);
        midi_out_init(&mo);
        CHECK(w && process_with(&g, in, NULL, out, NULL, BLOCK, &mi.list, &mo.list) == CLAP_PROCESS_CONTINUE && out[0] == 0.5f,
              "two-in %.0f: admitted and runs", rate);
        CHECK(w && w->a == 1 && w->b == 0, "two-in %.0f: the note reached the first MIDI input, the second was empty (a %u b %u)", rate,
              w ? w->a : 0, w ? w->b : 0);
        rig_down(&g);
    }
    if (rig_up(&g, g_midi, MIDI_URI "#two-out", rate, BLOCK) == 0)
    {
        *(void **)&w = loaded_symbol(g_midi_so, "lv2_midi_echo_two");
        midi_in_init(&mi);
        mi_midi(&mi, 3, note, 3);
        midi_out_init(&mo);
        CHECK(w && process_with(&g, in, NULL, out, NULL, BLOCK, &mi.list, &mo.list) == CLAP_PROCESS_CONTINUE && w->in == 1,
              "two-out %.0f: admitted, the note reached the plugin (in %u)", rate, w ? w->in : 0);
        CHECK(mo.n == 1 && out_is(&mo, 0, CLAP_EVENT_MIDI, 3, note, 3),
              "two-out %.0f: the plugin wrote the note to both outputs and the host read it once, from the first (%u)", rate, mo.n);
        rig_down(&g);
    }
}

/* no audio output (#183): a MIDI tool (notes in and out, no audio port) and an analyser (audio in, no output) are
 * hosted with the strip's audio passing through them bit for bit, the host's placement said in the port's name, no
 * latency on that audio, the tool's notes transformed and the analyser's control output read as before */
static void t_passthrough(double rate)
{
    struct midi_in mi;
    struct midi_out mo;
    struct rig g;
    clap_audio_port_info_t info;
    float in_l[BLOCK], in_r[BLOCK], out_l[BLOCK], out_r[BLOCK];
    static const uint8_t note[3] = { 0x90, 60, 100 }, up[3] = { 0x90, 72, 100 };
    uint32_t i;

    for (i = 0; i < BLOCK; i++)
        in_l[i] = (float)i / BLOCK - 0.5f, in_r[i] = 0.5f - (float)i / BLOCK, out_l[i] = out_r[i] = 7.0f;
    if (rig_up(&g, g_midi, MIDI_URI "#tool", rate, BLOCK) == 0)
    {
        CHECK(g.ports && g.ports->count(g.plugin, true) == 1 && g.ports->count(g.plugin, false) == 1 && g.ports->get(g.plugin, 0, false, &info)
              && info.channel_count == 2 && strstr(info.name, "passthrough: host") != NULL,
              "tool %.0f: a stereo main pair in and out, named as the host's placement (%s)", rate, info.name);
        CHECK(g.latency && g.latency->get(g.plugin) == 0, "tool %.0f: no latency on the audio it does not touch", rate);
        midi_in_init(&mi);
        mi_midi(&mi, 5, note, 3);
        midi_out_init(&mo);
        CHECK(process_with(&g, in_l, in_r, out_l, out_r, BLOCK, &mi.list, &mo.list) == CLAP_PROCESS_CONTINUE
              && memcmp(out_l, in_l, sizeof out_l) == 0 && memcmp(out_r, in_r, sizeof out_r) == 0,
              "tool %.0f: the audio passes through bit for bit, both legs", rate);
        CHECK(mo.n == 1 && out_is(&mo, 0, CLAP_EVENT_MIDI, 5, up, 3), "tool %.0f: the note came out transformed, an octave up at its frame (%u)", rate, mo.n);
        rig_down(&g);
    }
    if (rig_up(&g, g_fixture, FIXTURE_URI "#no-out", rate, BLOCK) == 0)
    {
        clap_param_info_t pi;
        uint32_t k, n = g.params ? g.params->count(g.plugin) : 0;
        clap_id peak = CLAP_INVALID_ID;

        for (k = 0; k < n; k++)
            if (g.params->get_info(g.plugin, k, &pi) && strcmp(pi.name, "peak") == 0)
                peak = pi.id;
        CHECK(g.ports && g.ports->get(g.plugin, 0, false, &info) && info.channel_count == 1 && strstr(info.name, "passthrough: host") != NULL,
              "analyser %.0f: a mono main out named as the host's placement (%s)", rate, info.name);
        for (i = 0; i < BLOCK; i++)
            out_l[i] = 7.0f;
        CHECK(process_with(&g, in_l, NULL, out_l, NULL, BLOCK, NULL, NULL) == CLAP_PROCESS_CONTINUE && memcmp(out_l, in_l, sizeof out_l) == 0,
              "analyser %.0f: the audio passes through bit for bit", rate);
        CHECK(peak != CLAP_INVALID_ID && param(&g, peak) == 0.5f, "analyser %.0f: its control output reads the block's peak as before (%g)", rate,
              peak != CLAP_INVALID_ID ? param(&g, peak) : -1.0);
        rig_down(&g);
    }
}

/* #184: a generator, a stereo output and no input of any kind, is admitted as a source: its face has no audio input
 * port and no note port, one stereo main output, and a block with no input renders its signal on both channels; a
 * MIDI source, a MIDI output and no input at all, is hosted as a pass-through tool whose note output carries its notes */
static void t_generator(double rate)
{
    static const uint8_t note[3] = { 0x90, 60, 100 };
    struct midi_in mi;
    struct midi_out mo;
    struct rig g;
    clap_audio_port_info_t info;
    const clap_plugin_note_ports_t *np;
    float in_l[BLOCK], in_r[BLOCK], out_l[BLOCK], out_r[BLOCK];
    float *outs[2] = { out_l, out_r };
    clap_audio_buffer_t ob = { outs, NULL, 2, 0, 0 };
    clap_process_t p;
    uint32_t i, all;

    for (i = 0; i < BLOCK; i++)
        in_l[i] = (float)i / BLOCK - 0.5f, in_r[i] = 0.5f - (float)i / BLOCK, out_l[i] = out_r[i] = 7.0f;
    if (rig_up(&g, g_fixture, FIXTURE_URI "#gen", rate, BLOCK) == 0)
    {
        np = g.plugin->get_extension(g.plugin, CLAP_EXT_NOTE_PORTS);
        CHECK(g.ports && g.ports->count(g.plugin, true) == 0 && g.ports->count(g.plugin, false) == 1
                  && g.ports->get(g.plugin, 0, false, &info) && info.channel_count == 2 && (info.flags & CLAP_AUDIO_PORT_IS_MAIN)
                  && strstr(info.name, "passthrough") == NULL && (!np || (np->count(g.plugin, true) == 0 && np->count(g.plugin, false) == 0)),
              "generator %.0f: no audio input port, no note port, a stereo main output of its own (%s)", rate, info.name);
        memset(&p, 0, sizeof(p));
        p.steady_time = -1;
        p.frames_count = BLOCK;
        p.audio_outputs = &ob;
        p.audio_outputs_count = 1;
        midi_in_init(&mi);
        midi_out_init(&mo);
        p.in_events = &mi.list;
        p.out_events = &mo.list;
        g_audio = 1;
        in_rt = 1;
        CHECK(g.plugin->process(g.plugin, &p) == CLAP_PROCESS_CONTINUE, "generator %.0f: a block with no audio input runs", rate);
        in_rt = 0;
        g_audio = 0;
        for (i = 0, all = 1; i < BLOCK; i++)
            all &= out_l[i] == 0.5f && out_r[i] == 0.25f;
        CHECK(all, "generator %.0f: 0.5 on the left and 0.25 on the right, every frame (%g, %g)", rate, out_l[0], out_r[BLOCK - 1]);
        rig_down(&g);
    }
    if (rig_up(&g, g_midi, MIDI_URI "#source", rate, BLOCK) == 0)
    {
        np = g.plugin->get_extension(g.plugin, CLAP_EXT_NOTE_PORTS);
        CHECK(g.ports && g.ports->count(g.plugin, true) == 1 && g.ports->count(g.plugin, false) == 1 && g.ports->get(g.plugin, 0, false, &info)
                  && info.channel_count == 2 && strstr(info.name, "passthrough: host") != NULL
                  && np && np->count(g.plugin, true) == 0 && np->count(g.plugin, false) == 1,
              "midi source %.0f: the strip's pair passing through, no note input, one note output (%s)", rate, info.name);
        midi_in_init(&mi);
        midi_out_init(&mo);
        CHECK(process_with(&g, in_l, in_r, out_l, out_r, BLOCK, &mi.list, &mo.list) == CLAP_PROCESS_CONTINUE
                  && memcmp(out_l, in_l, sizeof out_l) == 0 && memcmp(out_r, in_r, sizeof out_r) == 0,
              "midi source %.0f: the audio passes through bit for bit, both legs", rate);
        CHECK(mo.n == 1 && out_is(&mo, 0, CLAP_EVENT_MIDI, 0, note, 3), "midi source %.0f: its note came out at frame 0, a note the host never sent (%u)", rate, mo.n);
        rig_down(&g);
    }
}

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
    CHECK(f->activations == 1 && f->deactivations == 0,
          "latency %.0f: the restart is CLAP's only: the LV2 instance kept active, never LV2 deactivated or activated again (%u activations, %u deactivations)",
          rate, f->activations, f->deactivations);
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
    CHECK(f->activations == 1 && f->deactivations == 0, "latency %.0f: and keeps the LV2 instance too (%u activations, %u deactivations)",
          rate, f->activations, f->deactivations);
    g.plugin->deactivate(g.plugin);
    CHECK(f->deactivations == 1, "latency %.0f: a deactivate no restart asked for is LV2's too (%u deactivations)", rate, f->deactivations);
    CHECK(g.plugin->activate(g.plugin, rate, 1, MAXB) && f->activations == 2 && f->deactivations == 1,
          "latency %.0f: and its activate LV2 activates again (%u activations)", rate, f->activations);
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

/* ---- time:Position from the host's transport (§6.6) ---- */

static void time_run(struct omx_clap_instance *in, float *l, float *r)
{
    in_rt = 1;
    omx_clap_host_run(in, l, r, BLOCK);
    in_rt = 0;
}

static void t_time(double rate)
{
    char cwhy[OMX_CLAP_WHY_MAX] = "", why[OMX_CLAP_LV2_WHY_MAX] = "";
    const clap_plugin_entry_t *entry = omx_clap_lv2_entry(g_time, why);
    struct lv2_time_probe *pr;
    struct omx_clap_instance *in = NULL;
    struct omx_clap_transport_src src;
    struct omx_clap_transport rec;
    float l[BLOCK], r[BLOCK];
    uint32_t objects;
    int64_t frame;
    uint32_t i;

    if (entry && omx_clap_host_open_entry(entry, LV2_TIME_PROBE_URI, &in, cwhy) == 0 && omx_clap_host_activate(in, rate, MAXB, cwhy) != 0)
    {
        omx_clap_host_close(in);
        in = NULL;
    }
    // the binary is loaded by the open: its symbols are read after it
    pr = in ? loaded_symbol(g_time_so, "lv2_time_probe") : NULL;
    CHECK(in && pr, "time %.0f: the time probe opens and activates (%s)", rate, cwhy);
    if (!in || !pr)
        return;
    memset(pr, 0, sizeof *pr); /* the probe is the process's own, and every rate before this one counted into it */
    // the source is set before the publish, as the host sets it
    memset(&src, 0, sizeof src);
    omx_clap_host_set_transport(in, &src);
    omx_clap_host_publish(in, pthread_self());
    for (i = 0; i < BLOCK; i++) l[i] = r[i] = 0.25f;

    time_run(in, l, r);
    time_run(in, l, r);
    objects = pr->objects;
    CHECK(pr->events == 0 && objects == 0, "time %.0f: no transport, no object (%u events, %u objects)", rate, pr->events, objects);

    /* playing at 120 bpm, 7/8, bar 3, 1.5 beats into the bar: the whole object, once */
    memset(&rec, 0, sizeof rec);
    rec.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_TIME_SIGNATURE | CLAP_TRANSPORT_IS_PLAYING;
    rec.tempo = 120.0;
    rec.song_pos_beats = 5 * (CLAP_BEATTIME_FACTOR / 2);
    rec.bar_start = CLAP_BEATTIME_FACTOR; /* the bar starts on beat 1, so the beat in the bar is 1.5 */
    rec.bar_number = 3;
    rec.tsig_num = 7;
    rec.tsig_denom = 8;
    omx_clap_transport_publish(&src, &rec);
    time_run(in, l, r);
    frame = pr->frame;
    CHECK(pr->objects == objects + 1 && pr->events == 1, "time %.0f: a new transport forges one object (%u events)", rate, pr->events);
    CHECK(pr->keys == (LV2_TIME_PROBE_FRAME | LV2_TIME_PROBE_SPEED | LV2_TIME_PROBE_BAR | LV2_TIME_PROBE_BAR_BEAT | LV2_TIME_PROBE_BEAT_UNIT
                       | LV2_TIME_PROBE_BEATS_PER_BAR | LV2_TIME_PROBE_BPM), "time %.0f: every field of the transport is in it (0x%x)", rate, pr->keys);
    CHECK(pr->speed == 1.0f && pr->bpm == 120.0f && pr->bar == 3 && pr->bar_beat == 1.5f, "time %.0f: speed 1, 120 bpm, bar 3, beat 1.5 (%g %g %lld %g)", rate,
          (double)pr->speed, (double)pr->bpm, (long long)pr->bar, (double)pr->bar_beat);
    CHECK(pr->beat_unit == 8 && pr->beats_per_bar == 7.0f, "time %.0f: the meter 7/8 (%d/%g)", rate, pr->beat_unit, (double)pr->beats_per_bar);

    /* while it plays, every block carries one object, at the block's own frame */
    time_run(in, l, r);
    CHECK(pr->objects == objects + 2 && pr->events == 1 && pr->frame > frame,
          "time %.0f: playing, the next block carries one object again, at its own frame (%u events, %lld then %lld)", rate, pr->events,
          (long long)frame, (long long)pr->frame);
    frame = pr->frame;
    objects++;
    /* stopped and one beat on: a new object, speed 0 */
    rec.flags &= ~(uint32_t)CLAP_TRANSPORT_IS_PLAYING;
    rec.song_pos_beats += CLAP_BEATTIME_FACTOR;
    omx_clap_transport_publish(&src, &rec);
    time_run(in, l, r);
    CHECK(pr->objects == objects + 2 && pr->speed == 0.0f && pr->bar_beat == 2.5f, "time %.0f: stopped at beat 2.5 of the bar forges one object (speed %g, barBeat %g)", rate,
          (double)pr->speed, (double)pr->bar_beat);
    CHECK(pr->frame > frame, "time %.0f: the frame is the block's own (%lld, then %lld)", rate, (long long)frame, (long long)pr->frame);
    /* stopped and unchanged: nothing, the sequence is empty */
    time_run(in, l, r);
    CHECK(pr->objects == objects + 2 && pr->events == 0, "time %.0f: stopped, an unchanged transport forges no object (%u events)", rate,
          pr->events);

    /* only a tempo: the keys say so, and the bar and meter are absent */
    memset(&rec, 0, sizeof rec);
    rec.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_IS_PLAYING;
    rec.tempo = 90.0;
    omx_clap_transport_publish(&src, &rec);
    time_run(in, l, r);
    CHECK(pr->keys == (LV2_TIME_PROBE_FRAME | LV2_TIME_PROBE_SPEED | LV2_TIME_PROBE_BPM), "time %.0f: a tempo-only transport carries no bar and no meter (0x%x)", rate, pr->keys);
    CHECK(pr->bpm == 90.0f, "time %.0f: 90 bpm (%g)", rate, (double)pr->bpm);

    /* withdrawn: no transport, no object; then the same transport again is new */
    memset(&rec, 0, sizeof rec);
    omx_clap_transport_publish(&src, &rec);
    time_run(in, l, r);
    CHECK(pr->events == 0, "time %.0f: a withdrawn transport forges nothing (%u events)", rate, pr->events);
    rec.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_IS_PLAYING;
    rec.tempo = 90.0;
    omx_clap_transport_publish(&src, &rec);
    time_run(in, l, r);
    CHECK(pr->events == 1, "time %.0f: the transport back after a withdrawal forges again", rate);

    omx_clap_host_set_transport(in, NULL);
    omx_clap_request_stop(&in->stage);
    time_run(in, l, r);
    CHECK(omx_clap_host_unpublish(in, 10, 1000) == 0, "time %.0f: unpublished", rate);
    omx_clap_host_close(in);
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
        fake_worker_hold(1);
        process(&g, l, NULL, l, NULL, 32, NULL);
        fake_worker_hold(0);
        omx_clap_lv2_worker_quiesce(g.plugin);
        fake_worker_hold(1);
        process(&g, l, NULL, l, NULL, 32, NULL);
        CHECK(!strcmp(fake_trace(), "run0,end0,work0,resp0,run1,end1"), "worker %.0f: the drain order through process(): %s", rate, fake_trace());
        fake_worker_hold(0);
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
            fake_worker_hold(1);
            process(&g, l, NULL, l, NULL, 32, NULL);
            fake_worker_hold(0);
            omx_clap_lv2_worker_quiesce(g.plugin);
            fake_worker_hold(1);
            process(&g, l, NULL, l, NULL, 32, NULL);
            {
                char want[64];

                snprintf(want, sizeof(want), "run%u,end%u,work%u,resp%u,run%u,end%u", seq, seq, seq, seq, seq + 1, seq + 1);
                CHECK(!strcmp(fake_trace(), want), "worker %.0f: after the reset the worker answers again: %s", rate, fake_trace());
            }
            fake_worker_hold(0);
            omx_clap_lv2_worker_quiesce(g.plugin);
        }
        rig_down(&g);
    }
}

/* a reset whose worker cannot start again: the instance is broken, process() fails and clap.log says why, until the
 * next activate brings the worker back */
static void t_reset_no_worker(double rate)
{
    float l[32] = { 0 };
    struct fake *f;
    struct rig g;
    uint32_t acts, deacts, seq;
    char want[64];

    if (rig_up(&g, g_fakes, FAKE_WORKER, rate, MAXB) != 0)
        return;
    f = fake_last();
    CHECK(f && process(&g, l, NULL, l, NULL, 32, NULL) == CLAP_PROCESS_CONTINUE, "reset %.0f: the worker fake processes", rate);
    omx_clap_lv2_worker_quiesce(g.plugin);
    acts = f ? f->activations : 0;
    deacts = f ? f->deactivations : 0;
    g_refuse_threads = 1;
    g.plugin->reset(g.plugin);
    g_refuse_threads = 0;
    CHECK(f && f->activations == acts + 1 && f->deactivations == deacts + 2,
          "reset %.0f: the worker that cannot start leaves the plugin LV2 deactivated (activate %u -> %u, deactivate %u -> %u)",
          rate, acts, f ? f->activations : 0, deacts, f ? f->deactivations : 0);
    CHECK(strstr(g.host.log, "could not start again") != NULL, "reset %.0f: clap.log says the worker could not start (%s)", rate, g.host.log);
    CHECK(process(&g, l, NULL, l, NULL, 32, NULL) == CLAP_PROCESS_ERROR, "reset %.0f: process() fails while the instance is broken", rate);
    CHECK(process(&g, l, NULL, l, NULL, 32, NULL) == CLAP_PROCESS_ERROR, "reset %.0f: and keeps failing", rate);

    g.plugin->stop_processing(g.plugin);
    g.plugin->deactivate(g.plugin);
    CHECK(g.plugin->activate(g.plugin, rate, 1, MAXB) && g.plugin->start_processing(g.plugin), "reset %.0f: the next activate succeeds", rate);
    fake_trace_clear();
    seq = f ? f->seq : 0;
    fake_worker_hold(1);
    CHECK(process(&g, l, NULL, l, NULL, 32, NULL) == CLAP_PROCESS_CONTINUE, "reset %.0f: and process() runs again", rate);
    fake_worker_hold(0);
    omx_clap_lv2_worker_quiesce(g.plugin);
    fake_worker_hold(1);
    process(&g, l, NULL, l, NULL, 32, NULL);
    snprintf(want, sizeof(want), "run%u,end%u,work%u,resp%u,run%u,end%u", seq, seq, seq, seq, seq + 1, seq + 1);
    CHECK(!strcmp(fake_trace(), want), "reset %.0f: with the worker answering again: %s", rate, fake_trace());
    fake_worker_hold(0);
    omx_clap_lv2_worker_quiesce(g.plugin);
    rig_down(&g);
}

/* ---- the side chain: one CLAP input per side chain, fed the caller's buffers through the face and through the core ---- */

#define SC_URI "urn:openmixer:test:sidechain-fixture"

/* one process() call of the side-chain plugin: `in` the main pair, `side` the side chain's pair (NULL: none handed) */
static clap_process_status sc_process(struct rig *g, float *in_l, float *in_r, float *side_l, float *side_r, float *out_l, float *out_r, uint32_t n)
{
    float *ins[2] = { in_l, in_r }, *sides[2] = { side_l, side_r }, *outs[2] = { out_l, out_r };
    clap_audio_buffer_t ib[2] = { { ins, NULL, 2, 0, 0 }, { sides, NULL, 2, 0, 0 } }, ob = { outs, NULL, 2, 0, 0 };
    struct evlist none;
    clap_process_t p;
    clap_process_status st;

    ev_init(&none);
    memset(&p, 0, sizeof(p));
    p.steady_time = -1;
    p.frames_count = n;
    p.audio_inputs = ib;
    p.audio_outputs = &ob;
    p.audio_inputs_count = side_l ? 2 : 1;
    p.audio_outputs_count = 1;
    p.in_events = &none.list;
    p.out_events = &OUT_EVENTS;
    g_audio = 1;
    in_rt = 1;
    st = g->plugin->process(g->plugin, &p);
    in_rt = 0;
    g_audio = 0;
    return st;
}

static void t_sidechain(double rate)
{
    static float in_l[BLOCK], in_r[BLOCK], out_l[BLOCK], out_r[BLOCK], side_l[BLOCK], side_r[BLOCK], none_l[BLOCK], none_r[BLOCK];
    clap_audio_port_info_t port;
    struct rig g;
    uint32_t i;
    int same;

    if (rig_up(&g, g_sc, SC_URI, rate, MAXB) != 0)
        return;
    CHECK(g.ports && g.ports->count(g.plugin, true) == 2 && g.ports->count(g.plugin, false) == 1,
          "side chain %.0f: two inputs (the main pair and the side chain), one output", rate);
    CHECK(g.ports->get(g.plugin, 0, true, &port) && port.flags == CLAP_AUDIO_PORT_IS_MAIN && port.channel_count == 2,
          "side chain %.0f: the main input is the main stereo port", rate);
    CHECK(g.ports->get(g.plugin, 1, true, &port) && port.id == 1 && port.flags == 0 && port.channel_count == 2
          && strcmp(port.name, "Sidechain") == 0 && strcmp(port.port_type, CLAP_PORT_STEREO) == 0 && port.in_place_pair == CLAP_INVALID_ID,
          "side chain %.0f: the side chain is a stereo port of its group's label, not main (%s)", rate, port.name);
    CHECK(!g.ports->get(g.plugin, 2, true, &port), "side chain %.0f: no third input", rate);

    for (i = 0; i < BLOCK; i++)
        in_l[i] = 0.5f, in_r[i] = -0.25f, side_l[i] = 0.25f, side_r[i] = 0.125f, none_l[i] = none_r[i] = 0.0f;
    sc_process(&g, in_l, in_r, side_l, side_r, out_l, out_r, BLOCK);
    for (same = 1, i = 0; i < BLOCK; i++)
        same = same && out_l[i] == 0.75f && out_r[i] == -0.125f;
    CHECK(same, "side chain %.0f: out = main + side, per channel (%g, %g)", rate, out_l[0], out_r[0]);

    sc_process(&g, in_l, in_r, NULL, NULL, out_l, out_r, BLOCK);
    for (same = 1, i = 0; i < BLOCK; i++)
        same = same && out_l[i] == 0.5f && out_r[i] == -0.25f;
    CHECK(same, "side chain %.0f: handed no side chain, the output is the main input alone", rate);

    sc_process(&g, in_l, in_r, none_l, NULL, out_l, out_r, BLOCK);
    CHECK(out_l[0] == 0.5f && out_r[0] == -0.25f, "side chain %.0f: a silent side chain adds nothing", rate);
    rig_down(&g);
}

/* ---- state:makePath: a directory of the instance's own under the state root ---- */

/* the entries of the state root */
static int root_entries(void)
{
    DIR *d = opendir(g_root);
    struct dirent *e;
    int n = 0;

    while (d && (e = readdir(d)) != NULL)
        n += strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0;
    if (d)
        closedir(d);
    return n;
}

/* the first line of the file at `path`, "" when it cannot be read */
static const char *file_line(const char *path)
{
    static char line[64];
    FILE *f = fopen(path, "r");

    line[0] = '\0';
    if (f && !fgets(line, sizeof(line), f))
        line[0] = '\0';
    if (f)
        fclose(f);
    return line;
}

static int ends_with(const char *s, const char *tail)
{
    const size_t n = strlen(s), t = strlen(tail);

    return n >= t && strcmp(s + n - t, tail) == 0;
}

#define STALE 400u      // more instance numbers than this run reaches

/* the copies an earlier process of the same pid left: lv2-<pid>-<n>.saved/takes/level.txt holding 9, for every n up to
 * STALE. An instance must take a name none of them holds, or its save would land beside a stale copy. */
static int root_entry_remove(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st;
    (void)flag;
    (void)ftw;
    remove(path);
    return 0;
}

static void stale_copies(void)
{
    char path[PATH_MAX + 160];
    uint32_t n;

    // a root of this pid an earlier run left is cleared first: a run starts from the stale copies alone
    nftw(g_root, root_entry_remove, 16, FTW_DEPTH | FTW_PHYS);
    mkdir(g_root, 0755);
    for (n = 1; n <= STALE; n++)
    {
        FILE *f;

        snprintf(path, sizeof(path), "%s/lv2-%ld-%u.saved", g_root, (long)getpid(), n);
        mkdir(path, 0755);
        snprintf(path, sizeof(path), "%s/lv2-%ld-%u.saved/takes", g_root, (long)getpid(), n);
        mkdir(path, 0755);
        snprintf(path, sizeof(path), "%s/lv2-%ld-%u.saved/takes/level.txt", g_root, (long)getpid(), n);
        if ((f = fopen(path, "w")) != NULL)
        {
            fputs("9\n", f);
            fclose(f);
        }
    }
}

static void t_make_path(double rate)
{
    static const char TAIL[] = "/takes/level.txt";
    static float in[BLOCK], out[BLOCK];
    char dir[600], copy[700], prefix[PATH_MAX + 96];
    struct lv2_path_probe *pr;
    struct mem_out saved;
    struct mem_in given;
    struct evlist ev;
    struct rig a, b;
    struct stat st;
    uint32_t i;
    int before, same;

    for (i = 0; i < BLOCK; i++)
        in[i] = 0.5f * sinf(0.0613f * (float)i);
    if (rig_up(&a, g_path, LV2_PATH_FIXTURE_URI, rate, BLOCK) != 0)
        return;
    pr = loaded_symbol(g_path_so, "lv2_path_probe");
    CHECK(pr && pr->had_make_path, "make-path %.0f: instantiate is handed state:makePath", rate);
    if (!pr)
    {
        rig_down(&a);
        return;
    }
    before = root_entries();
    ev_init(&ev);
    ev_param(&ev, 2, 2.5);
    process(&a, in, NULL, out, NULL, BLOCK, &ev);
    CHECK(root_entries() == before, "make-path %.0f: no directory is made before the plugin asks for a path (%d entries)", rate, before);

    // the save: the plugin makes takes/level.txt in its directory, lilv copies it out and the state names the copy
    mo_init(&saved);
    CHECK(state_of(&a)->save(a.plugin, &saved.stream) && saved.n > 0, "make-path %.0f: the state saves (%zu bytes)", rate, saved.n);
    snprintf(prefix, sizeof(prefix), "%s/lv2-", g_root);
    CHECK(strncmp(pr->made, prefix, strlen(prefix)) == 0 && ends_with(pr->made, TAIL) && !strcmp(file_line(pr->made), "2.5\n"),
          "make-path %.0f: the path made is <root>/lv2-<pid>-<n>/takes/level.txt, its directories made, and the plugin wrote 2.5 into it (%s)",
          rate, pr->made);
    snprintf(dir, sizeof(dir), "%.*s", (int)(strlen(pr->made) - strlen(TAIL)), pr->made);
    snprintf(copy, sizeof(copy), "%s.saved%s", dir, TAIL);
    CHECK(strtoul(strrchr(dir, '-') + 1, NULL, 10) > STALE, "make-path %.0f: the instance's name is past every stale copy an earlier process left (%s)",
          rate, strrchr(dir, '/') + 1);
    CHECK(!strcmp(pr->stored, "takes/level.txt") && strstr(saved.buf, copy) != NULL && !strcmp(file_line(copy), "2.5\n"),
          "make-path %.0f: the state names the copy in <dir>.saved, which holds 2.5 (stored %s)", rate, pr->stored);
    rig_down(&a);
    CHECK(stat(dir, &st) != 0 && errno == ENOENT, "make-path %.0f: the instance's directory is removed with the instance", rate);
    CHECK(!strcmp(file_line(copy), "2.5\n"), "make-path %.0f: the copy the save made stays", rate);

    // a second instance loads the state before its activate: the plugin reads the level from the copy
    if (rig_up(&b, g_path, LV2_PATH_FIXTURE_URI, 0.0, BLOCK) != 0)
        return;
    mi_init(&given, saved.buf, saved.n);
    CHECK(state_of(&b)->load(b.plugin, &given.stream) && b.plugin->activate(b.plugin, rate, 1, BLOCK) && b.plugin->start_processing(b.plugin),
          "make-path %.0f: a second instance loads the state and activates", rate);
    process(&b, in, NULL, out, NULL, BLOCK, NULL);
    for (same = 1, i = 0; i < BLOCK; i++)
        same = same && out[i] == in[i] * 2.5f;
    CHECK(pr->restored == 2.5f && !strcmp(pr->restored_from, copy), "make-path %.0f: its plugin read 2.5 from the copy (%g, %s)", rate,
          (double)pr->restored, pr->restored_from);
    CHECK(same, "make-path %.0f: it runs at the level read through the file: out = in x 2.5 on every frame", rate);
    {
        char bdir[PATH_MAX + 96], why[OMX_CLAP_LV2_WHY_MAX] = "", saved_a[620], other[PATH_MAX + 96], link[PATH_MAX + 96];
        const uint32_t n = omx_clap_lv2_saved_dir(b.plugin, bdir, sizeof(bdir));

        CHECK(n == strlen(bdir) && strncmp(bdir, prefix, strlen(prefix)) == 0 && ends_with(bdir, ".saved"),
              "make-path %.0f: omx_clap_lv2_saved_dir names the plugin's copy directory (%s)", rate, bdir);
        snprintf(saved_a, sizeof(saved_a), "%s.saved", dir);
        // a path that is no copy directory of the root is refused, and left as it was
        snprintf(other, sizeof(other), "%slv2-7-%.0f.saved", g_build, rate);     // named like a copy, outside the root
        mkdir(other, 0755);
        snprintf(link, sizeof(link), "%s/lv2-9-%.0f.saved", g_root, rate);
        unlink(link);
        CHECK(symlink(saved_a, link) == 0, "make-path %.0f: a link named like a copy directory, to A's copies", rate);
        CHECK(omx_clap_lv2_saved_drop("/tmp", why) == -1 && !strcmp(why, "hosting.state.not-a-copy")
                  && omx_clap_lv2_saved_drop(g_root, why) == -1 && omx_clap_lv2_saved_drop(other, why) == -1
                  && omx_clap_lv2_saved_drop(link, why) == -1 && stat(other, &st) == 0 && !strcmp(file_line(copy), "2.5\n"),
              "make-path %.0f: /tmp, the root, a .saved directory outside the root and a link to A's copies are refused (%s)", rate, why);
        unlink(link);
        rmdir(other);
        CHECK(omx_clap_lv2_saved_drop(saved_a, why) == 0 && stat(saved_a, &st) != 0 && errno == ENOENT,
              "make-path %.0f: A's copy directory is dropped when the host deletes its state (%s)", rate, why);
    }
    rig_down(&b);
}

/* one makePath request the test makes itself: answered NULL, and the block after it logs `code` as the refusal */
static int mp_refused(struct rig *g, const LV2_State_Make_Path *mp, const char *path, const char *code)
{
    static float in[BLOCK], out[BLOCK];
    char *r = mp->path(mp->handle, path);
    int ok;

    g->host.log[0] = '\0';
    process(g, in, NULL, out, NULL, BLOCK, NULL);
    g->plugin->on_main_thread(g->plugin);
    ok = r == NULL && strstr(g->host.log, code) != NULL;
    free(r);
    return ok;
}

/* a path outside the instance's directory, and an absolute one, are answered NULL and counted; nothing is written */
/*
 * state:mapPath handed at instantiate (the #mappath twin requires it, and uses the instance's own rather than save's): a
 * file under the state root is stored relative to the root and read back from the same file by a second instance; a file
 * outside it is stored whole. The abstracted paths come back through the instance's freePath.
 */
static void map_path_round(double rate, const char *file, int inside)
{
    static float in[BLOCK], out[BLOCK];
    struct lv2_path_probe *pr;
    struct mem_out saved;
    struct mem_in given;
    struct evlist ev;
    struct rig a, b;
    uint32_t i;
    int same;

    for (i = 0; i < BLOCK; i++)
        in[i] = 0.5f * sinf(0.0613f * (float)i);
    setenv("OMX_PATH_FIXTURE_FILE", file, 1);
    if (rig_up(&a, g_path, LV2_PATH_FIXTURE_URI "#mappath", rate, BLOCK) != 0)
    {
        CHECK(0, "map-path %.0f: the twin requiring state:mapPath is admitted", rate);
        return;
    }
    pr = loaded_symbol(g_path_so, "lv2_path_probe");
    if (!pr)
    {
        rig_down(&a);
        return;
    }
    memset(pr, 0, sizeof(*pr));
    CHECK(1, "map-path %.0f: the twin requiring state:mapPath is admitted", rate);
    ev_init(&ev);
    ev_param(&ev, 2, 3.0);
    process(&a, in, NULL, out, NULL, BLOCK, &ev);
    mo_init(&saved);
    CHECK(state_of(&a)->save(a.plugin, &saved.stream) && saved.n > 0, "map-path %.0f: the state saves (%zu bytes)", rate, saved.n);
    if (inside)
        CHECK(!strcmp(pr->stored, file + strlen(g_root) + 1) && pr->stored[0] != '/',
              "map-path %.0f: a file under the state root is stored relative to it (%s)", rate, pr->stored);
    else
        CHECK(!strcmp(pr->stored, file), "map-path %.0f: a file outside the root is stored whole (%s)", rate, pr->stored);
    CHECK(pr->freed_by_host == 1, "map-path %.0f: the abstracted path is handed back through the instance's freePath", rate);
    rig_down(&a);
    if (rig_up(&b, g_path, LV2_PATH_FIXTURE_URI "#mappath", 0.0, BLOCK) != 0)
        return;
    mi_init(&given, saved.buf, saved.n);
    CHECK(state_of(&b)->load(b.plugin, &given.stream) && b.plugin->activate(b.plugin, rate, 1, BLOCK) && b.plugin->start_processing(b.plugin),
          "map-path %.0f: a second instance loads the state and activates", rate);
    process(&b, in, NULL, out, NULL, BLOCK, NULL);
    for (same = 1, i = 0; i < BLOCK; i++)
        same = same && out[i] == in[i] * 3.0f;
    CHECK(pr->restored == 3.0f && !strcmp(pr->restored_from, file) && same,
          "map-path %.0f: the path made absolute again is the file, its level read back: out = in x 3 (%g, %s)", rate,
          (double)pr->restored, pr->restored_from);
    rig_down(&b);
    unsetenv("OMX_PATH_FIXTURE_FILE");
}

static void t_map_path(double rate)
{
    char inside[sizeof(g_root) + 64], outside[sizeof(g_root) + 64];

    snprintf(inside, sizeof(inside), "%s/map-path-level.txt", g_root);
    snprintf(outside, sizeof(outside), "%s.map-path-level.txt", g_root);
    map_path_round(rate, inside, 1);
    map_path_round(rate, outside, 0);
    unlink(inside);
    unlink(outside);
}

static void t_make_path_escape(double rate)
{
    static const char ABS[] = "/tmp/omx-path-fixture-absolute.txt";
    static float in[BLOCK], out[BLOCK];
    char outside[PATH_MAX + 96], made[512], dir[512], copies[600];
    size_t i;
    struct lv2_path_probe *pr;
    struct stat st;
    struct rig g;

    snprintf(outside, sizeof(outside), "%s/outside.txt", g_root);
    unlink(ABS);
    if (rig_up(&g, g_path, LV2_PATH_FIXTURE_URI "#escape", rate, BLOCK) != 0)
        return;
    pr = loaded_symbol(g_path_so, "lv2_path_probe");
    CHECK(pr && pr->escape_null && pr->absolute_null, "make-path %.0f: ../outside.txt and an absolute path are answered NULL", rate);
    CHECK(stat(outside, &st) != 0 && stat(ABS, &st) != 0, "make-path %.0f: nothing is written outside the instance's directory", rate);
    memset(in, 0, sizeof(in));
    process(&g, in, NULL, out, NULL, BLOCK, NULL);
    g.plugin->on_main_thread(g.plugin);
    CHECK(strstr(g.host.log, "make_path_refused 2") != NULL && strstr(g.host.log, "hosting.state.path-outside") != NULL,
          "make-path %.0f: both are counted, make_path_refused 2 and the code hosting.state.path-outside on clap.log", rate);
    snprintf(made, sizeof(made), "%s", pr ? pr->made : "");
    CHECK(made[0] && !strcmp(file_line(made), "unsaved\n"), "make-path %.0f: a path inside is made and written (%s)", rate, made);
    snprintf(dir, sizeof(dir), "%.*s", (int)(strlen(made) > 16 ? strlen(made) - 16 : 0), made);     // less /never/saved.txt
    if (pr && pr->make_path)
    {
        const LV2_State_Make_Path *mp = pr->make_path;
        static char longest[5000], component[300];
        char lnk[600], fin[600], blk[600], esc[PATH_MAX + 96], *ok;
        FILE *bf;

        // a link inside the directory, to the root: a path through it would leave the directory
        snprintf(lnk, sizeof(lnk), "%s/lnk", dir);
        snprintf(fin, sizeof(fin), "%s/fin", dir);
        snprintf(esc, sizeof(esc), "%s/esc", g_root);
        CHECK(symlink(g_root, lnk) == 0 && symlink(outside, fin) == 0, "make-path %.0f: links lnk -> the root and fin -> outside.txt", rate);
        CHECK(mp_refused(&g, mp, "lnk/esc/f.txt", "hosting.state.path-symlink") && stat(esc, &st) != 0,
              "make-path %.0f: a link as a directory component is refused, hosting.state.path-symlink, and nothing is made through it", rate);
        CHECK(mp_refused(&g, mp, "fin", "hosting.state.path-symlink"), "make-path %.0f: a link as the last component is refused too", rate);
        CHECK(mp_refused(&g, mp, "a/../b", "hosting.state.path-outside"),
              "make-path %.0f: a/../b, though it comes back inside, is refused, hosting.state.path-outside", rate);
        memset(longest, 0, sizeof(longest));
        for (i = 0; i + 2 < sizeof(longest) - 1; i += 2)
            longest[i] = 'a', longest[i + 1] = '/';
        longest[i] = 'f';
        memset(component, 'c', sizeof(component) - 1);
        CHECK(mp_refused(&g, mp, longest, "hosting.state.path-too-long") && mp_refused(&g, mp, component, "hosting.state.path-too-long"),
              "make-path %.0f: a path past PATH_MAX and a component past NAME_MAX are refused, hosting.state.path-too-long", rate);
        // a request on the audio role: refused before anything is made
        g_audio = 1;
        ok = mp->path(mp->handle, "rt/f.txt");
        g_audio = 0;
        g.host.log[0] = '\0';
        process(&g, in, NULL, out, NULL, BLOCK, NULL);
        g.plugin->on_main_thread(g.plugin);
        snprintf(blk, sizeof(blk), "%s/rt", dir);
        CHECK(ok == NULL && strstr(g.host.log, "hosting.state.path-on-audio") != NULL && stat(blk, &st) != 0,
              "make-path %.0f: a request on the audio role is refused, hosting.state.path-on-audio, and nothing is made", rate);
        free(ok);
        // a file where the path needs a directory: the directory cannot be made
        snprintf(blk, sizeof(blk), "%s/blk", dir);
        CHECK((bf = fopen(blk, "w")) != NULL && fclose(bf) == 0 && mp_refused(&g, mp, "blk/x.txt", "hosting.state.path-unmakeable"),
              "make-path %.0f: a path through a file is refused, hosting.state.path-unmakeable", rate);
        ok = mp->path(mp->handle, "./ok/inside.txt");
        CHECK(ok && strncmp(ok, dir, strlen(dir)) == 0, "make-path %.0f: ./ok/inside.txt is still made (%s)", rate, ok ? ok : "NULL");
        free(ok);
    }
    rig_down(&g);
    CHECK(stat(g_root, &st) == 0 && S_ISDIR(st.st_mode), "make-path %.0f: removing the directory removed the link, not the root it named", rate);
    // never saved: neither the instance's directory nor a copy directory outlives it
    snprintf(copies, sizeof(copies), "%s.saved", dir);
    CHECK(dir[0] && stat(dir, &st) != 0 && stat(copies, &st) != 0, "make-path %.0f: an instance never saved leaves nothing under the root (%s)", rate, dir);
}

/* with no state root, state:makePath is not offered: a plugin that requires it is refused */
static void make_path_no_root(void)
{
    omx_clap_lv2_config_t config = { omx_clap_lv2_provided, 1000u, 4096u, 1000u, HOLD_MS, NULL, NULL };
    char why[OMX_CLAP_LV2_WHY_MAX] = "";
    const clap_plugin_entry_t *e;
    const clap_plugin_factory_t *f;
    struct th t;

    CHECK(omx_clap_lv2_configure(&config, why) == 0, "no root: the provided list with no state root configures (%s)", why);
    e = omx_clap_lv2_entry(g_path, why);
    f = e ? e->get_factory(CLAP_PLUGIN_FACTORY_ID) : NULL;
    th_init(&t);
    CHECK(f && f->create_plugin(f, &t.host, LV2_PATH_FIXTURE_URI) == NULL && !strcmp(t.log, "hosting.features.missing\n"),
          "no root: a plugin that requires state:makePath is refused, hosting.features.missing (%.*s)", (int)strcspn(t.log, "\n"), t.log);
    omx_clap_lv2_entry_release(e);
    e = omx_clap_lv2_entry(g_fixture, why);
    f = e ? e->get_factory(CLAP_PLUGIN_FACTORY_ID) : NULL;
    th_init(&t);
    CHECK(f && f->create_plugin(f, &t.host, FIXTURE_URI "#makepath") == NULL && !strcmp(t.log, "hosting.features.missing\n"),
          "no root: so is the host fixture's makePath twin");
    omx_clap_lv2_entry_release(e);
}

/* a state root that is not absolute refuses the configure */
static void make_path_relative_root(void)
{
    omx_clap_lv2_config_t config = { omx_clap_lv2_provided, 1000u, 4096u, 1000u, HOLD_MS, NULL, "relative/state" };
    char why[OMX_CLAP_LV2_WHY_MAX] = "";

    CHECK(omx_clap_lv2_configure(&config, why) == -1 && !strcmp(why, "lv2.configure.state-root"),
          "a relative state root refuses the configure (%s)", why);
}

/* ---- the atom messages beside MIDI: text properties, atom:Blank, patch:Put bodies, notify, time and notes on MIDI ---- */

#define ATOM_NAME LV2_ATOM_FIXTURE_URI "#name"
#define ATOM_FILE LV2_ATOM_FIXTURE_URI "#file"
#define ATOM_GAIN_ID 6u                 /* five ports, and the file before the gain in the URIs' order */
#define ATOM_NOTIFY_CAPACITY (1024u - 8u)   /* the smallest atom buffer, less its atom header */

static struct lv2_atom_probe *atom_probe(void)
{
    return loaded_symbol(g_atom_so, "lv2_atom_probe");
}

static int text_is(struct rig *g, const char *uri, const char *want)
{
    char buf[4200];
    const uint32_t n = omx_clap_lv2_patch_text_get(g->plugin, uri, buf, sizeof(buf));

    return n == strlen(want) && strcmp(buf, want) == 0;
}

static void t_atom_text(double rate)
{
    static float in[BLOCK], out[BLOCK];
    static char big[4097], got_y[4 + 63 + 1];
    char why[OMX_CLAP_LV2_WHY_MAX] = "";
    omx_clap_lv2_text_info_t info, name, file;
    struct lv2_atom_probe *pr;
    struct evlist ev;
    struct rig g;
    uint32_t i, blocks = 0;
    int same;

    if (rig_up(&g, g_atom, LV2_ATOM_FIXTURE_URI, rate, BLOCK) != 0)
        return;
    pr = atom_probe();
    CHECK(pr != NULL, "atom %.0f: the probe of the atom fixture", rate);
    if (!pr)
    {
        rig_down(&g);
        return;
    }
    memset(pr, 0, sizeof(*pr));
    for (i = 0; i < BLOCK; i++)
        in[i] = 0.5f * sinf(0.0613f * (float)i);

    CHECK(omx_clap_lv2_patch_text_count(g.plugin) == 2 && omx_clap_lv2_patch_text_info(g.plugin, 0, &file)
              && omx_clap_lv2_patch_text_info(g.plugin, 1, &name) && !omx_clap_lv2_patch_text_info(g.plugin, 2, &info),
          "atom %.0f: two text properties", rate);
    CHECK(!strcmp(name.uri, ATOM_NAME) && !strcmp(name.name, "Name") && !name.is_path && name.max_bytes == 4095u
              && !strcmp(file.uri, ATOM_FILE) && !strcmp(file.name, "File") && file.is_path && file.max_bytes == 4095u,
          "atom %.0f: the name a string and the file a path, each up to 4095 bytes (%u, %u)", rate, name.max_bytes, file.max_bytes);
    CHECK(omx_clap_lv2_patch_text_count(NULL) == 0 && omx_clap_lv2_patch_text_set(NULL, ATOM_NAME, "x", why) == -1
              && !strcmp(why, "hosting.patch.not-writable"),
          "atom %.0f: no plugin of the adapter's, no text property (%s)", rate, why);

    // a string: written, busy until its block, the plugin's patch:Put body read back
    CHECK(omx_clap_lv2_patch_text_set(g.plugin, ATOM_NAME, "hello", why) == 0, "atom %.0f: write the name (%s)", rate, why);
    CHECK(omx_clap_lv2_patch_text_set(g.plugin, ATOM_NAME, "again", why) == -1 && !strcmp(why, "hosting.patch.busy"),
          "atom %.0f: a second write before the block that takes the first is refused (%s)", rate, why);
    CHECK(text_is(&g, ATOM_NAME, "hello"), "atom %.0f: before the plugin reports one, the value read is the host's write", rate);
    process(&g, in, NULL, out, NULL, BLOCK, NULL), blocks++;
    CHECK(!strcmp(pr->name, "hello"), "atom %.0f: the plugin got the name as a patch:Set (%s)", rate, pr->name);
    CHECK(text_is(&g, ATOM_NAME, "got:hello"), "atom %.0f: its reply, a patch:Put whose patch:body holds the name, is read back", rate);

    // a path: the plugin's patch:Set of the same atom:Path read back
    CHECK(omx_clap_lv2_patch_text_set(g.plugin, ATOM_FILE, "/srv/take 1.wav", why) == 0, "atom %.0f: write the file (%s)", rate, why);
    process(&g, in, NULL, out, NULL, BLOCK, NULL), blocks++;
    CHECK(!strcmp(pr->file, "/srv/take 1.wav") && text_is(&g, ATOM_FILE, "/srv/take 1.wav"),
          "atom %.0f: the plugin got the path, and its patch:Set of it is read back (%s)", rate, pr->file);

    // the limits: 4095 bytes are carried, 4096 refused, a property the plugin does not list refused
    memset(big, 'y', 4096);
    big[4096] = '\0';
    CHECK(omx_clap_lv2_patch_text_set(g.plugin, ATOM_NAME, big, why) == -1 && !strcmp(why, "hosting.patch.too-large"),
          "atom %.0f: a value of 4096 bytes is refused (%s)", rate, why);
    big[4095] = '\0';
    CHECK(omx_clap_lv2_patch_text_set(g.plugin, ATOM_NAME, big, why) == 0, "atom %.0f: a value of 4095 bytes is taken (%s)", rate, why);
    process(&g, in, NULL, out, NULL, BLOCK, NULL), blocks++;
    snprintf(got_y, sizeof(got_y), "got:%.63s", big);
    CHECK(strlen(pr->name) == 63 && pr->name[0] == 'y' && text_is(&g, ATOM_NAME, got_y),
          "atom %.0f: the plugin got it whole (it keeps 63 bytes of it) and replied", rate);
    CHECK(omx_clap_lv2_patch_text_set(g.plugin, LV2_ATOM_FIXTURE_URI "#nothing", "x", why) == -1 && !strcmp(why, "hosting.patch.not-writable"),
          "atom %.0f: a property the plugin does not list is refused (%s)", rate, why);

    // a reply past 4095 bytes is dropped and counted; the value read stays the last one carried
    CHECK(omx_clap_lv2_patch_text_set(g.plugin, ATOM_NAME, "big", why) == 0, "atom %.0f: write big (%s)", rate, why);
    process(&g, in, NULL, out, NULL, BLOCK, NULL), blocks++;
    g.plugin->on_main_thread(g.plugin);
    CHECK(strstr(g.host.log, "patch_dropped 1") != NULL && text_is(&g, ATOM_NAME, got_y),
          "atom %.0f: the plugin's 5000-byte reply is dropped, logged as patch_dropped 1, and the value read is the last one carried", rate);

    // a reply of 4096 bytes and no NUL is dropped too, and nothing is written past the slot; 4095 and the NUL are read whole
    CHECK(omx_clap_lv2_patch_text_set(g.plugin, ATOM_NAME, "nonul", why) == 0, "atom %.0f: write nonul (%s)", rate, why);
    process(&g, in, NULL, out, NULL, BLOCK, NULL), blocks++;
    g.plugin->on_main_thread(g.plugin);
    CHECK(strstr(g.host.log, "patch_dropped 2") != NULL && text_is(&g, ATOM_NAME, got_y) && text_is(&g, ATOM_FILE, "/srv/take 1.wav"),
          "atom %.0f: a 4096-byte reply with no NUL is dropped, patch_dropped 2, and neither text is touched", rate);
    CHECK(omx_clap_lv2_patch_text_set(g.plugin, ATOM_NAME, "max", why) == 0, "atom %.0f: write max (%s)", rate, why);
    process(&g, in, NULL, out, NULL, BLOCK, NULL), blocks++;
    {
        char buf[4200];
        const uint32_t n = omx_clap_lv2_patch_text_get(g.plugin, ATOM_NAME, buf, sizeof(buf));

        CHECK(n == 4095u && strlen(buf) == 4095u && buf[0] == 'x' && buf[4094] == 'x',
              "atom %.0f: a reply of 4095 bytes and its NUL is read whole (%u)", rate, n);
    }
    CHECK(omx_clap_lv2_patch_text_set(g.plugin, ATOM_NAME, "big", why) == 0, "atom %.0f: write big again", rate);
    process(&g, in, NULL, out, NULL, BLOCK, NULL), blocks++;

    // a numeric write: the reply forged as an atom:Blank is the output event, the other object at 99 ignored
    ev_init(&ev);
    ev_param(&ev, ATOM_GAIN_ID, 0.5);
    g_out_n = 0;
    process(&g, in, NULL, out, NULL, BLOCK, &ev), blocks++;
    for (same = 1, i = 0; i < BLOCK; i++)
        same = same && out[i] == in[i] * 0.5f;
    CHECK(same && pr->gain == 0.5f, "atom %.0f: the gain reaches the plugin as a patch:Set: out = in x 0.5 on every frame", rate);
    CHECK(g_out_n == 1 && g_out_id[0] == ATOM_GAIN_ID && g_out_value[0] == 0.5,
          "atom %.0f: the plugin's patch:Set forged as an atom:Blank comes back as the gain's output event (%u events)", rate, g_out_n);
    CHECK(param(&g, ATOM_GAIN_ID) == 0.5, "atom %.0f: the object that is no patch:Set or patch:Put, naming the gain at 99, is ignored (%g)",
          rate, param(&g, ATOM_GAIN_ID));
    CHECK(pr->notify_capacity == ATOM_NOTIFY_CAPACITY && pr->notify_written == blocks,
          "atom %.0f: the notify output is offered %u bytes at every block, and what the plugin writes there goes nowhere (%u, %u blocks)",
          rate, ATOM_NOTIFY_CAPACITY, pr->notify_capacity, pr->notify_written);

    // a new LV2 instance (another rate) is sent every text the host wrote, again
    g.plugin->stop_processing(g.plugin);
    g.plugin->deactivate(g.plugin);
    CHECK(g.plugin->activate(g.plugin, rate * 2.0, 1, BLOCK) && g.plugin->start_processing(g.plugin), "atom %.0f: activate at twice the rate", rate);
    memset(pr->name, 0, sizeof(pr->name));
    memset(pr->file, 0, sizeof(pr->file));
    process(&g, in, NULL, out, NULL, BLOCK, NULL);
    CHECK(!strcmp(pr->name, "big") && !strcmp(pr->file, "/srv/take 1.wav"),
          "atom %.0f: the new instance gets the host's last name and file on its first block (%s, %s)", rate, pr->name, pr->file);
    rig_down(&g);
}

/* one process() call of the MIDI-and-time plugin: `tr` its transport (NULL: none), `events` its input */
static clap_process_status mt_process(struct rig *g, float *in, float *out, const clap_event_transport_t *tr, const clap_input_events_t *events)
{
    float *ins[1] = { in }, *outs[1] = { out };
    clap_audio_buffer_t ib = { ins, NULL, 1, 0, 0 }, ob = { outs, NULL, 1, 0, 0 };
    struct midi_out mo;
    clap_process_t p;
    clap_process_status st;

    midi_out_init(&mo);
    memset(&p, 0, sizeof(p));
    p.steady_time = 0;
    p.frames_count = BLOCK;
    p.transport = tr;
    p.audio_inputs = &ib;
    p.audio_outputs = &ob;
    p.audio_inputs_count = 1;
    p.audio_outputs_count = 1;
    p.in_events = events;
    p.out_events = &mo.list;
    g_audio = 1;
    in_rt = 1;
    st = g->plugin->process(g->plugin, &p);
    in_rt = 0;
    g_audio = 0;
    return st;
}

/* a CLAP note event of `type` */
static void mi_clap_note(struct midi_in *m, uint16_t type, uint32_t time, int16_t port, int16_t channel, int16_t key, double velocity)
{
    clap_event_note_t *e = &m->ev[m->n++].note;

    memset(e, 0, sizeof(*e));
    e->header.size = sizeof(*e);
    e->header.time = time;
    e->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e->header.type = type;
    e->note_id = -1;
    e->port_index = port;
    e->channel = channel;
    e->key = key;
    e->velocity = velocity;
}

static int probe_midi_is(const struct lv2_atom_probe *pr, uint32_t k, int64_t frame, uint8_t a, uint8_t b, uint8_t c)
{
    return k < pr->n_midi && pr->midi_frame[k] == frame && pr->midi_size[k] == 3 && pr->midi[k][0] == a && pr->midi[k][1] == b
           && pr->midi[k][2] == c;
}

static void t_atom_midi_time(double rate)
{
    static const uint8_t note[3] = { 0x90, 0x3c, 0x64 };
    static float in[BLOCK], out[BLOCK];
    const clap_plugin_note_ports_t *np;
    clap_note_port_info_t info;
    clap_event_transport_t tr;
    struct lv2_atom_probe *pr;
    struct midi_in mi;
    struct rig g;
    uint32_t objects;

    if (rig_up(&g, g_atom, LV2_ATOM_FIXTURE_URI "#midi-time", rate, BLOCK) != 0)
        return;
    pr = atom_probe();
    if (!pr)
    {
        CHECK(0, "midi-time %.0f: the probe of the atom fixture", rate);
        rig_down(&g);
        return;
    }
    memset(pr, 0, sizeof(*pr));
    np = g.plugin->get_extension(g.plugin, CLAP_EXT_NOTE_PORTS);
    CHECK(np && np->count(g.plugin, true) == 1 && np->count(g.plugin, false) == 0 && np->get(g.plugin, 0, true, &info)
              && info.supported_dialects == (CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_CLAP) && info.preferred_dialect == CLAP_NOTE_DIALECT_MIDI,
          "midi-time %.0f: one note input, MIDI preferred, CLAP notes taken", rate);

    // block 1: a transport, one MIDI note on, and CLAP notes of every kind
    memset(&tr, 0, sizeof(tr));
    tr.header.size = sizeof(tr);
    tr.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    tr.header.type = CLAP_EVENT_TRANSPORT;
    tr.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_IS_PLAYING;
    tr.tempo = 120.0;
    midi_in_init(&mi);
    mi_midi(&mi, 5, note, 3);
    mi_clap_note(&mi, CLAP_EVENT_NOTE_ON, 10, 0, 2, 60, 0.5);       // 63.5 rounds to 64
    mi_clap_note(&mi, CLAP_EVENT_NOTE_ON, 11, -1, 0, 61, 0.0);      // a note on never at velocity 0
    mi_clap_note(&mi, CLAP_EVENT_NOTE_OFF, 12, 0, 2, 60, 0.25);     // 31.75 rounds to 32
    mi_clap_note(&mi, CLAP_EVENT_NOTE_CHOKE, 13, 0, 3, 62, 1.0);    // a choke is a note off at 0
    mi_clap_note(&mi, CLAP_EVENT_NOTE_ON, 14, 0, 0, -1, 1.0);       // a wildcard key: not taken
    mi_clap_note(&mi, CLAP_EVENT_NOTE_ON, 15, 0, -1, 64, 1.0);      // a wildcard channel: not taken
    mi_clap_note(&mi, CLAP_EVENT_NOTE_ON, 16, 1, 0, 65, 1.0);       // another port: not taken
    {
        clap_event_note_expression_t *x = &mi.ev[mi.n++].expr;     // a note expression: not taken

        memset(x, 0, sizeof(*x));
        x->header.size = sizeof(*x);
        x->header.time = 17;
        x->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        x->header.type = CLAP_EVENT_NOTE_EXPRESSION;
        x->expression_id = CLAP_NOTE_EXPRESSION_PRESSURE;
        x->note_id = -1;
        x->channel = 2;
        x->key = 60;
        x->value = 0.5;
    }
    mt_process(&g, in, out, &tr, &mi.list);
    CHECK(pr->events == 6 && pr->time_first && pr->time_event_frame == 0 && pr->speed == 1.0f && pr->bpm == 120.0f,
          "midi-time %.0f: the transport's object first, at frame 0, playing at 120 bpm, then five MIDI messages (%u events)", rate, pr->events);
    CHECK(pr->n_midi == 5 && probe_midi_is(pr, 0, 5, 0x90, 0x3c, 0x64) && probe_midi_is(pr, 1, 10, 0x92, 60, 64)
              && probe_midi_is(pr, 2, 11, 0x90, 61, 1) && probe_midi_is(pr, 3, 12, 0x82, 60, 32) && probe_midi_is(pr, 4, 13, 0x83, 62, 0),
          "midi-time %.0f: the MIDI note, then the CLAP notes as MIDI: on 92 3c 40, on at velocity 1, off 82 3c 20, choke 83 3e 00 (%u)",
          rate, pr->n_midi);
    objects = pr->objects;

    // block 2: still playing, one object again, at this block's frame
    midi_in_init(&mi);
    mt_process(&g, in, out, &tr, &mi.list);
    CHECK(pr->events == 1 && pr->time_first && pr->objects == objects + 1,
          "midi-time %.0f: playing, the next block carries one object again (%u events)", rate, pr->events);
    objects = pr->objects;

    // block 3: stopped: one object, speed 0, before the MIDI
    tr.flags &= ~(uint32_t)CLAP_TRANSPORT_IS_PLAYING;
    midi_in_init(&mi);
    mi_midi(&mi, 0, note, 3);
    mt_process(&g, in, out, &tr, &mi.list);
    CHECK(pr->events == 2 && pr->time_first && pr->speed == 0.0f && pr->objects == objects + 1 && probe_midi_is(pr, 0, 0, 0x90, 0x3c, 0x64),
          "midi-time %.0f: stopped, one object at speed 0, still ahead of a note at the same frame (%u events)", rate, pr->events);

    // block 4: no transport: nothing forged
    midi_in_init(&mi);
    mt_process(&g, in, out, NULL, &mi.list);
    CHECK(pr->events == 0, "midi-time %.0f: no transport, no object (%u events)", rate, pr->events);
    rig_down(&g);
}

/* LSP's UI ports and a sequence that takes time:Position, through the CLAP face: created, no note port (the input is no
 * MIDI port), the input a valid sequence at every process(), the transport's object there once, and out = in x 0.5 */
static void t_atom_ui(double rate)
{
    static const char *const frags[] = { "#ui-lsp", "#ui-seq-time", "#ui-none" };
    static float in[BLOCK], out[BLOCK];
    clap_event_transport_t tr;
    size_t s;
    uint32_t i, b;

    for (i = 0; i < BLOCK; i++)
        in[i] = 0.5f * sinf(0.0613f * (float)i);
    memset(&tr, 0, sizeof(tr));
    tr.header.size = sizeof(tr);
    tr.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    tr.header.type = CLAP_EVENT_TRANSPORT;
    tr.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_IS_PLAYING;
    tr.tempo = 120.0;
    for (s = 0; s < sizeof(frags) / sizeof(frags[0]); s++)
    {
        char uri[128];
        const clap_plugin_note_ports_t *np;
        struct lv2_atom_probe *pr;
        struct midi_in mi;
        struct rig g;
        int same = 1;

        snprintf(uri, sizeof(uri), "%s%s", LV2_ATOM_FIXTURE_URI, frags[s]);
        if (rig_up(&g, g_atom, uri, rate, BLOCK) != 0)
            continue;
        pr = atom_probe();
        if (!pr)
        {
            CHECK(0, "atom ui %s %.0f: the probe of the atom fixture", frags[s], rate);
            rig_down(&g);
            continue;
        }
        memset(pr, 0, sizeof(*pr));
        np = g.plugin->get_extension(g.plugin, CLAP_EXT_NOTE_PORTS);
        CHECK(!np || (np->count(g.plugin, true) == 0 && np->count(g.plugin, false) == 0),
              "atom ui %s %.0f: no note port: the atom input is not reported as MIDI", frags[s], rate);
        for (b = 0; b < 4; b++)
        {
            midi_in_init(&mi);
            mt_process(&g, in, out, &tr, &mi.list);
            for (i = 0; i < BLOCK; i++)
                same = same && out[i] == in[i] * 0.5f;
        }
        CHECK(pr->ui_blocks == 4 && pr->ui_valid == 4, "atom ui %s %.0f: the input is a valid sequence at every process() (%u of %u)",
              frags[s], rate, pr->ui_valid, pr->ui_blocks);
        CHECK(pr->ui_time == (strcmp(frags[s], "#ui-none") != 0 ? 4u : 0u) && pr->ui_other == 0,
              "atom ui %s %.0f: a playing transport's object read there at every block when the port takes time:Position, nothing else (%u, %u)",
              frags[s], rate, pr->ui_time, pr->ui_other);
        CHECK(same, "atom ui %s %.0f: out = in x 0.5 on every frame of four blocks", frags[s], rate);
        rig_down(&g);
    }
}

/* ---- the CLAP body of libomx-clap-core around the adapter ---- */

static struct omx_clap_instance *core_open_at(const clap_plugin_entry_t *entry, const char *uri, double rate, uint32_t max_block,
                                              char why[OMX_CLAP_WHY_MAX])
{
    struct omx_clap_instance *in = NULL;

    if (omx_clap_host_open_entry(entry, uri, &in, why) != 0)
        return NULL;
    if (omx_clap_host_activate(in, rate, max_block, why) != 0)
    {
        omx_clap_host_close(in);
        return NULL;
    }
    omx_clap_host_publish(in, pthread_self());
    return in;
}

static struct omx_clap_instance *core_open(const clap_plugin_entry_t *entry, const char *uri, double rate, char why[OMX_CLAP_WHY_MAX])
{
    return core_open_at(entry, uri, rate, MAXB, why);
}

static void core_run(struct omx_clap_instance *in, float *l, float *r, uint32_t n)
{
    in_rt = 1;
    omx_clap_run(&in->stage, l, r, n);
    in_rt = 0;
}

static void t_sidechain_core(double rate)
{
    char why[OMX_CLAP_LV2_WHY_MAX] = "", cwhy[OMX_CLAP_WHY_MAX] = "";
    const clap_plugin_entry_t *entry = omx_clap_lv2_entry(g_sc, why);
    struct omx_clap_instance *in = NULL;
    static float l[BLOCK], r[BLOCK], sl[MAXB], sr[MAXB];
    uint32_t i;
    int same;

    CHECK(entry && omx_clap_host_open_entry(entry, SC_URI, &in, cwhy) == 0, "core %.0f: the side-chain plugin opens through the core (%s)", rate, cwhy);
    if (!in)
    {
        omx_clap_lv2_entry_release(entry);
        return;
    }
    CHECK(in->aux_inputs == 1 && in->aux_in_channels[0] == 2 && in->in_channels == 2, "core %.0f: one stereo auxiliary input beside the stereo pair", rate);
    for (i = 0; i < BLOCK; i++)
        l[i] = 0.5f, r[i] = -0.25f, sl[i] = 0.25f, sr[i] = 0.125f;
    CHECK(omx_clap_host_bind_aux_input(in, 0, sl, sr, MAXB) == 0, "core %.0f: bind the side chain before activation", rate);
    CHECK(omx_clap_host_activate(in, rate, MAXB, cwhy) == 0, "core %.0f: activate (%s)", rate, cwhy);
    omx_clap_host_publish(in, pthread_self());
    CHECK(omx_clap_host_bind_aux_input(in, 0, NULL, NULL, 0) == -1, "core %.0f: a binding while the instance runs is refused", rate);
    core_run(in, l, r, BLOCK);          /* the first cycle after arming is the warm-up's: its output is not checked */
    for (i = 0; i < BLOCK; i++)
        l[i] = 0.5f, r[i] = -0.25f;
    core_run(in, l, r, BLOCK);
    for (same = 1, i = 0; i < BLOCK; i++)
        same = same && l[i] == 0.75f && r[i] == -0.125f;
    CHECK(same, "core %.0f: the plugin reads the bound pair through the core (%g, %g)", rate, l[0], r[0]);

    omx_clap_request_stop(&in->stage);
    for (i = 0; i < BLOCK; i++)
        l[i] = 0.5f, r[i] = -0.25f;
    core_run(in, l, r, BLOCK);
    CHECK(omx_clap_host_unpublish(in, 10, 1000000) == 0, "core %.0f: unpublish", rate);
    CHECK(omx_clap_host_bind_aux_input(in, 0, NULL, NULL, 0) == 0, "core %.0f: unbind the side chain", rate);
    omx_clap_host_publish(in, pthread_self());
    for (i = 0; i < BLOCK; i++)
        l[i] = 0.5f, r[i] = -0.25f;
    core_run(in, l, r, BLOCK);
    for (i = 0; i < BLOCK; i++)
        l[i] = 0.5f, r[i] = -0.25f;
    core_run(in, l, r, BLOCK);
    for (same = 1, i = 0; i < BLOCK; i++)
        same = same && l[i] == 0.5f && r[i] == -0.25f;
    CHECK(same, "core %.0f: unbound, the plugin reads silence again (%g, %g)", rate, l[0], r[0]);
    omx_clap_request_stop(&in->stage);
    core_run(in, l, r, BLOCK);
    CHECK(omx_clap_host_unpublish(in, 10, 1000000) == 0, "core %.0f: unpublish again", rate);
    omx_clap_host_close(in);
    omx_clap_lv2_entry_release(entry);
}

/* ---- the CV ports: mono audio ports, bit-identical with the plugin run directly ---- */

#define CV_URI "urn:openmixer:test:cv-fixture"

/* one process() call of the CV fixture: `cv` the CV input (NULL: no buffer handed), `env` the CV output (NULL: none) */
static clap_process_status cv_process(struct rig *g, float *in, float *cv, float *out, float *env, uint32_t n)
{
    float *ins[1] = { in }, *cvs[1] = { cv }, *outs[1] = { out }, *envs[1] = { env };
    clap_audio_buffer_t ib[2] = { { ins, NULL, 1, 0, 0 }, { cvs, NULL, 1, 0, 0 } };
    clap_audio_buffer_t ob[2] = { { outs, NULL, 1, 0, 0 }, { envs, NULL, 1, 0, 0 } };
    struct evlist none;
    clap_process_t p;
    clap_process_status st;

    ev_init(&none);
    memset(&p, 0, sizeof(p));
    p.steady_time = -1;
    p.frames_count = n;
    p.audio_inputs = ib;
    p.audio_outputs = ob;
    p.audio_inputs_count = cv ? 2 : 1;
    p.audio_outputs_count = env ? 2 : 1;
    p.in_events = &none.list;
    p.out_events = &OUT_EVENTS;
    g_audio = 1;
    in_rt = 1;
    st = g->plugin->process(g->plugin, &p);
    in_rt = 0;
    g_audio = 0;
    return st;
}

/* the fixture's binary run directly at `rate` over the same blocks and buffers: what the adapter must give, bit for bit */
static int cv_direct(double rate, const uint32_t *lens, uint32_t n_lens, const float *x, const float *m, float *out, float *env)
{
    void *so = dlopen(g_cv_so, RTLD_NOW);
    const LV2_Descriptor *(*descriptor)(uint32_t);
    const LV2_Descriptor *d = NULL;
    static float bi[MAXB], bm[MAXB], bo[MAXB], be[MAXB];
    LV2_Handle h;
    uint32_t b, at = 0;

    if (!so)
        return -1;
    *(void **)&descriptor = dlsym(so, "lv2_descriptor");
    d = descriptor ? descriptor(0) : NULL;
    h = d ? d->instantiate(d, rate, g_cv, NULL) : NULL;
    if (!h)
    {
        dlclose(so);
        return -1;
    }
    d->connect_port(h, 0, bi);
    d->connect_port(h, 1, bo);
    d->connect_port(h, 2, bm);
    d->connect_port(h, 3, be);
    d->activate(h);
    for (b = 0; b < n_lens; b++)
    {
        memcpy(bi, x + at, lens[b] * sizeof(float));
        memcpy(bm, m + at, lens[b] * sizeof(float));
        d->run(h, lens[b]);
        memcpy(out + at, bo, lens[b] * sizeof(float));
        memcpy(env + at, be, lens[b] * sizeof(float));
        at += lens[b];
    }
    d->deactivate(h);
    d->cleanup(h);
    dlclose(so);
    return 0;
}

static void t_cv(double rate)
{
    static const uint32_t lens[] = { BLOCK, 17u, 1u, MAXB, 40u, BLOCK, 3u };
    enum { N_LENS = sizeof(lens) / sizeof(lens[0]), TOTAL = 64 + 17 + 1 + 256 + 40 + 64 + 3 };
    static float x[TOTAL], m[TOTAL], zero[TOTAL], out[TOTAL], env[TOTAL], want_out[TOTAL], want_env[TOTAL];
    clap_audio_port_info_t port;
    struct rig g;
    uint32_t b, i, at;

    for (i = 0; i < TOTAL; i++)
    {
        x[i] = 0.5f * sinf(0.0613f * (float)i);
        m[i] = 0.75f * sinf(0.0071f * (float)i + 0.3f);      // a signal of its own, not a copy of the audio
        zero[i] = 0.0f;
    }
    if (rig_up(&g, g_cv, CV_URI, rate, MAXB) != 0)
        return;
    CHECK(g.ports && g.ports->count(g.plugin, true) == 2 && g.ports->count(g.plugin, false) == 2,
          "cv %.0f: two inputs (the main and the CV input) and two outputs (the main and the CV output)", rate);
    CHECK(g.ports->get(g.plugin, 0, true, &port) && port.flags == CLAP_AUDIO_PORT_IS_MAIN && port.channel_count == 1,
          "cv %.0f: the main input is the main mono port", rate);
    CHECK(g.ports->get(g.plugin, 1, true, &port) && port.id == 1 && port.flags == 0 && port.channel_count == 1
          && strcmp(port.name, "Mod") == 0 && strcmp(port.port_type, CLAP_PORT_MONO) == 0 && port.in_place_pair == CLAP_INVALID_ID,
          "cv %.0f: the CV input is a mono port named Mod, not main (%s)", rate, port.name);
    CHECK(g.ports->get(g.plugin, 1, false, &port) && port.id == 1 && port.flags == 0 && port.channel_count == 1
          && strcmp(port.name, "Env") == 0 && strcmp(port.port_type, CLAP_PORT_MONO) == 0,
          "cv %.0f: the CV output is a mono port named Env, not main (%s)", rate, port.name);
    CHECK(!g.ports->get(g.plugin, 2, true, &port) && !g.ports->get(g.plugin, 2, false, &port), "cv %.0f: no third port either way", rate);

    for (b = 0, at = 0; b < N_LENS; at += lens[b], b++)
        cv_process(&g, x + at, m + at, out + at, env + at, lens[b]);
    CHECK(cv_direct(rate, lens, N_LENS, x, m, want_out, want_env) == 0, "cv %.0f: the fixture's binary runs directly", rate);
    CHECK(bits_equal(out, want_out, TOTAL), "cv %.0f: %u frames in blocks of 64, 17, 1, 256, 40, 64, 3: the audio output is the direct run's, bit for bit (peak %g)",
          rate, (unsigned)TOTAL, (double)peak_diff(out, want_out, TOTAL));
    CHECK(bits_equal(env, want_env, TOTAL), "cv %.0f: the CV output is the direct run's, bit for bit (peak %g)", rate,
          (double)peak_diff(env, want_env, TOTAL));
    rig_down(&g);

    // handed no CV buffer, the input reads silence: the direct run with a zero CV input, after a fresh activate
    if (rig_up(&g, g_cv, CV_URI, rate, MAXB) != 0)
        return;
    for (b = 0, at = 0; b < N_LENS; at += lens[b], b++)
        cv_process(&g, x + at, NULL, out + at, NULL, lens[b]);
    cv_direct(rate, lens, N_LENS, x, zero, want_out, want_env);
    CHECK(bits_equal(out, want_out, TOTAL), "cv %.0f: an input handed no buffer reads silence, as the direct run with a zero CV input", rate);
    rig_down(&g);
}

/* the CV fixture through libomx-clap-core: the CV input an auxiliary input the caller binds, the CV output an auxiliary
 * output; the bound CV reaches the audio exactly */
static void t_cv_core(double rate)
{
    char why[OMX_CLAP_LV2_WHY_MAX] = "", cwhy[OMX_CLAP_WHY_MAX] = "";
    const clap_plugin_entry_t *entry = omx_clap_lv2_entry(g_cv, why);
    struct omx_clap_instance *in = NULL;
    static float l[BLOCK], r[BLOCK], cv[MAXB];
    uint32_t i;
    int same;

    CHECK(entry && omx_clap_host_open_entry(entry, CV_URI, &in, cwhy) == 0, "cv core %.0f: the CV fixture opens through the core (%s)", rate, cwhy);
    if (!in)
    {
        omx_clap_lv2_entry_release(entry);
        return;
    }
    CHECK(in->aux_inputs == 1 && in->aux_in_channels[0] == 1 && in->aux_outputs == 1 && in->aux_channels[0] == 1,
          "cv core %.0f: one mono auxiliary input and one mono auxiliary output (%u, %u)", rate, in->aux_inputs, in->aux_outputs);
    for (i = 0; i < MAXB; i++)
        cv[i] = 0.25f;
    CHECK(omx_clap_host_bind_aux_input(in, 0, cv, NULL, MAXB) == 0, "cv core %.0f: bind the CV input", rate);
    CHECK(omx_clap_host_activate(in, rate, MAXB, cwhy) == 0, "cv core %.0f: activate (%s)", rate, cwhy);
    omx_clap_host_publish(in, pthread_self());
    for (i = 0; i < BLOCK; i++)
        l[i] = 0.5f;
    core_run(in, l, r, BLOCK);          /* the first cycle after arming is the warm-up's: its output is not checked */
    for (i = 0; i < BLOCK; i++)
        l[i] = 0.5f;
    core_run(in, l, r, BLOCK);
    for (same = 1, i = 0; i < BLOCK; i++)
        same = same && l[i] == 0.125f;
    CHECK(same, "cv core %.0f: the plugin reads the bound CV through the core: 0.5 x 0.25 (%g)", rate, l[0]);
    omx_clap_request_stop(&in->stage);
    core_run(in, l, r, BLOCK);
    CHECK(omx_clap_host_unpublish(in, 10, 1000000) == 0, "cv core %.0f: unpublish", rate);
    omx_clap_host_close(in);
    omx_clap_lv2_entry_release(entry);
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
            all &= l[i] == ((i & 1) ? -omx_db_to_lin(CLAP_HOST_CLAMP_DBFS) : omx_db_to_lin(CLAP_HOST_CLAMP_DBFS));
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

/* ---- the warm-up at the host's block size ---- */

/* A plain LV2 run loop, the way a host with no CLAP body runs a plugin: instantiate, activate, the warm-up in blocks of
 * `warm` frames (the activated size), deactivate and activate, then `live` blocks of `n` frames of the tone into `out`. */
static int plain_lv2_run(const char *uri, double rate, uint32_t warm, uint32_t n, uint32_t live, float *out)
{
    void *so = dlopen(g_fakes_so, RTLD_NOW);
    const LV2_Descriptor *(*descriptor)(uint32_t);
    const LV2_Descriptor *d = NULL;
    LV2_Handle h;
    static float in[WARMB], o[WARMB];
    float lat = 0.0f;
    uint32_t i, b;

    if (!so || warm > WARMB || n > warm)
        return -1;
    *(void **)&descriptor = dlsym(so, "lv2_descriptor");
    for (i = 0; descriptor && (d = descriptor(i)) && strcmp(d->URI, uri); i++)
        ;
    h = d ? d->instantiate(d, rate, g_fakes, NULL) : NULL;
    if (!h)
    {
        dlclose(so);
        return -1;
    }
    d->connect_port(h, 0, in);
    d->connect_port(h, 2, &lat);
    d->activate(h);
    for (b = 0; b < CLAP_HOST_WARMUP_BLOCKS; b++)
    {
        const float v = b < CLAP_HOST_WARMUP_BLOCKS / 2u ? omx_db_to_lin(CLAP_HOST_WARMUP_LEVEL_DBFS) : 0.0f;

        for (i = 0; i < warm; i++)
            in[i] = v;
        d->connect_port(h, 1, o);
        d->run(h, warm);
    }
    d->deactivate(h);
    d->activate(h);
    for (b = 0; b < live; b++)
    {
        tone(in, n, 0.5f, b * n);
        d->connect_port(h, 1, out + b * n);
        d->run(h, n);
    }
    d->deactivate(h);
    d->cleanup(h);
    dlclose(so);
    return 0;
}

/*
 * A plugin whose state is counted in frames (here a sample-and-hold whose phase runs on across run() calls) comes out of
 * the warm-up in the state the warm-up's block size left it in. The CLAP body activated at `act` must warm it up at
 * `act`, so that from the first live block of `n` frames on it is sample for sample what a plain LV2 loop primed at
 * `act` gives: with `act` the live block size, and with `act` larger (a host that activates at its largest quantum and
 * runs smaller periods).
 */
static void t_warmup_block(double rate, uint32_t act, uint32_t n)
{
    enum { LIVE = 4 };
    char why[OMX_CLAP_LV2_WHY_MAX] = "", cwhy[OMX_CLAP_WHY_MAX] = "";
    const clap_plugin_entry_t *entry = omx_clap_lv2_entry(g_fakes, why);
    struct omx_clap_instance *in = entry ? core_open_at(entry, FAKE_HOLD, rate, act, cwhy) : NULL;
    static float want[LIVE * MAXB], got[LIVE * MAXB];
    uint32_t b, first = UINT32_MAX;

    CHECK(in != NULL, "warm-up %.0f: the sample-and-hold through the CLAP body activated at %u frames (%s)", rate, act, cwhy);
    if (!in)
    {
        if (entry)
            omx_clap_lv2_entry_release(entry);
        return;
    }
    for (b = 0; b < LIVE; b++)
    {
        tone(got + b * n, n, 0.5f, b * n);
        core_run(in, got + b * n, NULL, n);
    }
    omx_clap_host_unpublish(in, CLAP_HOST_UNPUBLISH_POLL_US, CLAP_HOST_UNPUBLISH_TIMEOUT_US);
    omx_clap_host_close(in);
    omx_clap_lv2_entry_release(entry);
    CHECK(plain_lv2_run(FAKE_HOLD, rate, act, n, LIVE, want) == 0, "warm-up %.0f: the plain LV2 loop ran the sample-and-hold", rate);
    // the first live block fades in from dry: compare from the second on
    for (b = n; b < LIVE * n && first == UINT32_MAX; b++)
        if (memcmp(&got[b], &want[b], sizeof(float)) != 0)
            first = b;
    CHECK(first == UINT32_MAX,
          "warm-up %.0f: activated at %u, blocks 2..%d of %u frames bit-identical with a plain LV2 loop warmed at %u (first difference at frame %d: %.9g vs %.9g)",
          rate, act, LIVE, n, act, first == UINT32_MAX ? -1 : (int)first, first == UINT32_MAX ? 0.0 : (double)got[first],
          first == UINT32_MAX ? 0.0 : (double)want[first]);
}

int main(int argc, char **argv)
{
    omx_clap_lv2_config_t config = { omx_clap_lv2_provided, 1000u, 4096u, 1000u, HOLD_MS, NULL, NULL };
    char dir[PATH_MAX + 32], build[PATH_MAX], why[OMX_CLAP_LV2_WHY_MAX];
    void *volatile probe;
    size_t r;

    // one line at a time, also into a pipe: a run that stops shows the last check it passed
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 2 || abs_dir(argv[1], build) != 0)
    {
        printf("FAIL usage: lv2_clap_test <build/lv2>\n");
        return 1;
    }
    snprintf(dir, sizeof(dir), "%somx-host-fixture.lv2", build);
    abs_dir(dir, g_fixture);
    snprintf(g_fixture_so, sizeof(g_fixture_so), "%somx-host-fixture.so", g_fixture);
    snprintf(dir, sizeof(dir), "%somx-worker-gain.lv2", build);
    abs_dir(dir, g_wg);
    snprintf(dir, sizeof(dir), "%somx-lv2-fakes.lv2", build);
    abs_dir(dir, g_fakes);
    snprintf(dir, sizeof(dir), "%somx-patch-fixture.lv2", build);
    abs_dir(dir, g_patch);
    snprintf(g_patch_so, sizeof(g_patch_so), "%somx-patch-fixture.so", g_patch);
    snprintf(dir, sizeof(dir), "%somx-time-probe.lv2", build);
    abs_dir(dir, g_time);
    snprintf(g_time_so, sizeof(g_time_so), "%somx-time-probe.so", g_time);
    snprintf(dir, sizeof(dir), "%somx-sidechain-fixture.lv2", build);
    abs_dir(dir, g_sc);
    snprintf(dir, sizeof(dir), "%somx-path-fixture.lv2", build);
    abs_dir(dir, g_path);
    snprintf(g_path_so, sizeof(g_path_so), "%somx-path-fixture.so", g_path);
    // a state root of this run's own: the directories it leaves are build/'s
    snprintf(g_root, sizeof(g_root), "%sstate-root-%ld", build, (long)getpid());
    snprintf(g_build, sizeof(g_build), "%s", build);
    config.state_root = g_root;
    stale_copies();
    snprintf(g_fakes_so, sizeof(g_fakes_so), "%somx-lv2-fakes.so", g_fakes);
    snprintf(g_wg_so, sizeof(g_wg_so), "%somx-worker-gain.so", g_wg);
    snprintf(dir, sizeof(dir), "%somx-midi-echo.lv2", build);
    abs_dir(dir, g_midi);
    snprintf(dir, sizeof(dir), "%somx-atom-fixture.lv2", build);
    abs_dir(dir, g_atom);
    snprintf(g_atom_so, sizeof(g_atom_so), "%somx-atom-fixture.so", g_atom);
    snprintf(dir, sizeof(dir), "%somx-cv-fixture.lv2", build);
    abs_dir(dir, g_cv);
    snprintf(g_cv_so, sizeof(g_cv_so), "%somx-cv-fixture.so", g_cv);
    snprintf(g_midi_so, sizeof(g_midi_so), "%somx-midi-echo.so", g_midi);
    in_rt = 1;
    probe = malloc(16);
    free(probe);
    in_rt = 0;
    CHECK(rt_allocs == 2, "allocation witness sees malloc+free (%d)", rt_allocs);
    rt_allocs = 0;

    in_child("configure", unconfigured);
    in_child("make path with no root", make_path_no_root);
    in_child("make path with a relative root", make_path_relative_root);
    CHECK(omx_clap_lv2_configure(&config, why) == 0, "configure with omx_clap_lv2_provided, a hold of %u ms (%s)", HOLD_MS, why);
    t_entry();
    t_refusals();
    t_midi_refusal();
    t_props();
    for (r = 0; r < N_RATES; r++)
    {
        const int before = g_failures;

        t_fixture(RATES[r]);
        t_state(RATES[r]);
        t_preset(RATES[r]);
        t_preset_list(RATES[r]);
        t_live(RATES[r]);
        t_patch(RATES[r]);
        t_latency_hold(RATES[r]);
        t_fixed_block(RATES[r]);
        t_inplace(RATES[r]);
        t_midi_patch(RATES[r]);
        t_midi_extra(RATES[r]);
        t_passthrough(RATES[r]);
        t_pad(RATES[r]);
        t_worker(RATES[r]);
        t_reset_no_worker(RATES[r]);
        t_midi(RATES[r]);
        t_instrument(RATES[r]);
        t_generator(RATES[r]);
        t_time(RATES[r]);
        t_atom_text(RATES[r]);
        t_atom_midi_time(RATES[r]);
        t_atom_ui(RATES[r]);
        t_sidechain(RATES[r]);
        t_sidechain_core(RATES[r]);
        t_make_path(RATES[r]);
        t_make_path_escape(RATES[r]);
        t_map_path(RATES[r]);
        t_cv(RATES[r]);
        t_cv_core(RATES[r]);
        t_core(RATES[r]);
        t_warmup_block(RATES[r], MAXB, MAXB);
        t_warmup_block(RATES[r], WARMB, MAXB);
        printf("lv2 clap @ %.0f: %d failure(s)\n", RATES[r], g_failures - before);
    }
    CHECK(rt_allocs == 0, "no allocation inside process() or the CLAP body's run (%d)", rt_allocs);
    printf("%s\n", g_failures == 0 ? "lv2 clap test ok" : "lv2 clap test FAILED");
    return g_failures == 0 ? 0 : 1;
}
