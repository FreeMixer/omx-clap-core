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
 * lv2_host_test.c — the bundle half and the feature providers of the LV2 host against REAL bundles, offline.
 * This binary links NO lilv: the host dlopens it.
 *
 *   lazy      lilv is not in the process until the first bundle, and is after it; a library that cannot load refuses
 *             hosting.no-realisation and does not latch (a child: the library is named once per process).
 *   bundle    one bundle is loaded, never load_all: the fixture bundle does not know the worker gain, even with the
 *             worker gain's own bundle loaded beside it.
 *   fixture   a plugin that dereferences every port, needs urid:map and loadDefaultState and its own bundle path:
 *             -20 dB row + -6 dB default state reads -26.00 dB, its latency port reads 7, a -10 dB row write reads
 *             -16.00 dB, its meter output is read back.
 *   refusals  one twin per refusal code of the port reader, from the TTL alone, before the binary is opened.
 *   atom      the atom fixture's ports: the patch pair, a notify output given a buffer of its own, the string and path
 *             properties beside the numeric one; a MIDI input that takes time:Position too; an input of the plugin's own
 *             objects hosted; a fifth notify output refused hosting.features.midi-in-fed-empty.
 *   atom ui   an atom control input that takes no MIDI, in each shape the desk's effects declare (LSP's in_ui, a sequence
 *             with time:Position, nothing declared, patch:Message with time:Position, atom:String with patch:Message,
 *             the plugin's own objects): admitted, never a MIDI input, fed a valid sequence at every block, the
 *             transport's object when it takes time:Position, and the audio bit for bit; an atom input that is no
 *             sequence and a fifth control input refused hosting.features.atom-in-unfed.
 *   cv        the CV fixture's CV input and output read in index order with their names, nine CV inputs and nine
 *             CV outputs refused hosting.features.cv-ports; at every declared rate the CV input reaches run() and the CV output comes back,
 *             frame for frame, against the closed form computed here.
 *   worker    the worker gain: options carry the rate and the block lengths, ONE worker thread per instance, the gain
 *             reaches the audio only through the round trip, work() on that thread, the thread joined at deactivate.
 *   role      the host's audio-role predicate classifies urid:map and schedule_work.
 *   features  the configured list, provided whole or refused: a list without worker:schedule (or none) refuses a plugin
 *             requiring it before instantiate; a URI no provider serves refuses the configure.
 *   options   the feature options as the feature probe sees them through this host: the defaults are what the host
 *             did before it had options; each kind withheld, strict and not, the block declarations and lengths, the
 *             log routed and captured, all within the configured list.
 *
 *   lv2_host_test <build/lv2 directory> <path of liblilv-0.so.0>
 */
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#include <lv2/buf-size/buf-size.h>
#include <lv2/options/options.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#include "lv2_core.h"
#include "fixtures/lv2_atom_probe.h"
#include "fixtures/lv2_feature_probe.h"
#include "fixtures/lv2_worker_gain.h"
#include "lv2_test_util.h"

#define BLOCK 64u
#define N_BLOCKS 16u
#define FIXTURE_URI "urn:openmixer:test:host-fixture"

static char g_fixture[PATH_MAX], g_wg[PATH_MAX], g_wg_so[PATH_MAX + 32], g_build[PATH_MAX], g_midi[PATH_MAX], g_sc[PATH_MAX];
static char g_atom[PATH_MAX], g_atom_so[PATH_MAX + 32];
static char g_cv[PATH_MAX];
static char g_fp[PATH_MAX], g_fp_so[PATH_MAX + 32];
static const char *g_lilv;
static pthread_t g_main;
static volatile int g_in_run;      // the main thread holds the audio role only inside a block

/* A failing allocator for the strings the core duplicates (the Makefile wraps strdup and free for this test): strdup of
 * the text in g_strdup_fail returns NULL, and every pointer strdup hands out is held until free takes it back, so the
 * pointers still held are the strings a path leaked. */
#define LIVE_MAX 256
static const char *g_strdup_fail;
static void *g_live[LIVE_MAX];
static int g_live_n;
char *__real_strdup(const char *);
void __real_free(void *);

char *__wrap_strdup(const char *s)
{
    char *r;

    if (g_strdup_fail && strcmp(s, g_strdup_fail) == 0)
        return NULL;
    r = __real_strdup(s);
    if (r && g_live_n < LIVE_MAX)
        g_live[g_live_n++] = r;
    return r;
}

void __wrap_free(void *p)
{
    for (int i = 0; p && i < g_live_n; i++)
        if (g_live[i] == p)
        {
            g_live[i] = g_live[--g_live_n];
            break;
        }
    __real_free(p);
}

static int thread_count(void)
{
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    int n = 0;

    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.')
            n++;
    closedir(d);
    return n;
}

/* the count once it reaches `want`, or after 1 s: a joined thread can stay listed a moment */
static int thread_count_reaching(int want)
{
    int n = thread_count(), i;

    for (i = 0; i < 1000 && n != want; i++)
    {
        usleep(1000);
        n = thread_count();
    }
    return n;
}

static double rms(const float *x, uint32_t n)
{
    double a = 0;
    uint32_t i;

    for (i = 0; i < n; i++)
        a += (double)x[i] * x[i];
    return sqrt(a / n);
}

/* dB of out over in across blocks [from, to) */
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

static void run_block(struct lv2_instance *in, const float *x, float *y)
{
    memcpy(lv2_instance_audio_in(in, 0), x, BLOCK * sizeof(float));
    g_in_run = 1;
    lv2_instance_run(in, BLOCK, NULL, NULL);
    g_in_run = 0;
    memcpy(y, lv2_instance_audio_out(in, 0), BLOCK * sizeof(float));
}

static int control_of(const struct lv2_plugin *p, const char *symbol)
{
    uint32_t k;

    for (k = 0; k < p->n_controls; k++)
        if (strcmp(p->controls[k].symbol, symbol) == 0)
            return (int)k;
    return -1;
}

static struct lv2_plugin *open_plugin(const char *dir, const char *uri, char why[LV2_CORE_WHY_MAX])
{
    struct lv2_bundle *b = lv2_bundle_ref(dir, why);
    struct lv2_plugin *p = b ? lv2_plugin_open(b, uri, why) : NULL;

    lv2_bundle_unref(b);        // the plugin holds its own reference
    return p;
}

/* ---- lazy ---- */

static void lazy(void)
{
    char late[PATH_MAX + 32], why[LV2_CORE_WHY_MAX] = "";
    struct lv2_core_config config = { lv2_core_provided, 1000u, 4096u, 0u, NULL, NULL };
    struct lv2_bundle *b;

    snprintf(late, sizeof(late), "%slilv-late.so.0", g_build);
    unlink(late);
    config.lilv_soname = late;
    CHECK(lv2_core_configure(&config, why) == 0, "lazy: configured to load %s, which does not exist yet", late);
    CHECK(!lilv_mapped() && !lv2_core_lilv_loaded(), "lazy: no lilv in the process before the first bundle");
    b = lv2_bundle_ref(g_fixture, why);
    CHECK(b == NULL && strcmp(why, "hosting.no-realisation") == 0, "lazy: no lilv refuses the bundle with hosting.no-realisation (got '%s')", why);
    CHECK(!lilv_mapped() && !lv2_core_lilv_loaded(), "lazy: the refusal mapped nothing");
    CHECK(symlink(g_lilv, late) == 0, "lazy: the library appears at %s", late);
    b = lv2_bundle_ref(g_fixture, why);
    CHECK(b != NULL, "lazy: the failed load did not latch; the next bundle loads lilv (%s)", why);
    CHECK(lilv_mapped() && lv2_core_lilv_loaded(), "lazy: lilv is in the process after the first bundle");
    lv2_bundle_unref(b);
    unlink(late);
}

/* ---- one bundle ---- */

static void t_one_bundle(void)
{
    char why[LV2_CORE_WHY_MAX] = "";
    struct lv2_bundle *f = lv2_bundle_ref(g_fixture, why), *w;
    struct lv2_plugin *p;

    CHECK(f && lv2_bundle_count(f) == 12, "bundle: the fixture bundle lists its 12 plugins (%u)", lv2_bundle_count(f));
    p = f ? lv2_plugin_open(f, LV2_WORKER_GAIN_URI, why) : NULL;
    CHECK(!p && strcmp(why, "hosting.no-realisation") == 0, "bundle: LV2_PATH holds the worker gain, the fixture bundle does not know it: one bundle, never load_all (%s)", why);
    w = lv2_bundle_ref(g_wg, why);
    CHECK(w && lv2_bundle_count(w) == 1 && strcmp(lv2_bundle_uri(w, 0), LV2_WORKER_GAIN_URI) == 0, "bundle: the worker gain's own bundle lists it");
    p = w ? lv2_plugin_open(w, LV2_WORKER_GAIN_URI, why) : NULL;
    CHECK(p != NULL, "bundle: the worker gain's own bundle opens it (%s)", why);
    lv2_plugin_close(p);
    p = f ? lv2_plugin_open(f, LV2_WORKER_GAIN_URI, why) : NULL;
    CHECK(!p, "bundle: with the worker gain's bundle loaded too, the fixture bundle still does not hold it: the plugin must be THAT bundle's (%s)", why);
    CHECK(lv2_bundle_count(f) == 12, "bundle: and does not list it");
    lv2_bundle_unref(w);
    lv2_bundle_unref(f);
}

/* ---- the fixture ---- */

static void t_fixture(void)
{
    static float in[N_BLOCKS * BLOCK], out[N_BLOCKS * BLOCK];
    char why[LV2_CORE_WHY_MAX] = "";
    struct lv2_plugin *p = open_plugin(g_fixture, FIXTURE_URI, why);
    int needs_map = 0, needs_state = 0, gain, meter;
    uint32_t i, r, b;

    CHECK(p != NULL, "fixture: open (%s)", why);
    if (!p)
        return;
    for (i = 0; p->required[i]; i++)
    {
        needs_map |= strcmp(p->required[i], LV2_URID__map) == 0;
        needs_state |= strcmp(p->required[i], LV2_STATE__loadDefaultState) == 0;
    }
    CHECK(p->legs == 1 && p->in_ports[0] == 0 && p->out_ports[0] == 1 && p->latency_port == 5, "fixture: one leg 0->1, latency port 5");
    CHECK(p->n_controls == 3 && p->controls[0].port == 2 && p->controls[0].kind == LV2_CONTROL_BYPASS && p->controls[0].def == 1.0f,
          "fixture: lv2:enabled is its own bypass, held at 1");
    gain = control_of(p, "gain_db");
    meter = control_of(p, "meter");
    CHECK(gain == 1 && p->controls[1].port == 3 && p->controls[1].kind == LV2_CONTROL_INPUT && p->controls[1].def == -20.0f
          && p->controls[1].min == -60.0f && p->controls[1].max == 0.0f, "fixture: one row, gain_db at port 3, default -20 in -60..0");
    CHECK(meter == 2 && p->controls[2].port == 4 && p->controls[2].kind == LV2_CONTROL_OUTPUT, "fixture: the meter is an output the host reads");
    CHECK(needs_map && needs_state, "fixture: requires urid:map and loadDefaultState");
    CHECK(strlen(p->bundle_path) > 21 && strcmp(p->bundle_path + strlen(p->bundle_path) - 21, "omx-host-fixture.lv2/") == 0, "fixture: the bundle path instantiate is handed (%s)", p->bundle_path);
    CHECK(lv2_plugin_load(p, why) == 0 && p->desc && strcmp(p->desc->URI, FIXTURE_URI) == 0, "fixture: the descriptor walked to the URI");
    for (r = 0; r < N_RATES; r++)
    {
        struct lv2_instance *inst = lv2_instance_new(p);
        double g;

        CHECK(inst && lv2_instance_activate(inst, RATES[r], 1, BLOCK, why) == 0, "fixture %.0f Hz: instantiated and activated (%s)", RATES[r], why);
        if (!inst)
            continue;
        for (b = 0; b < N_BLOCKS; b++)
        {
            tone(in + b * BLOCK, BLOCK, 0.5f, b * BLOCK);
            run_block(inst, in + b * BLOCK, out + b * BLOCK);
        }
        g = gain_db(in, out, 2, N_BLOCKS);
        CHECK(fabs(g - -26.0) < 0.01, "fixture %.0f Hz: reads %.3f dB: the -20 dB row plus the -6 dB default state", RATES[r], g);
        CHECK(lv2_instance_latency(inst) == 7u, "fixture %.0f Hz: the latency port reads %u frames", RATES[r], lv2_instance_latency(inst));
        CHECK(fabsf(lv2_instance_control_get(inst, (uint32_t)meter) - 0.5f * powf(10.0f, -26.0f / 20.0f)) < 0.01f,
              "fixture %.0f Hz: the meter output is read back (%.4f)", RATES[r], (double)lv2_instance_control_get(inst, (uint32_t)meter));
        lv2_instance_control_set(inst, (uint32_t)gain, -10.0f);
        lv2_instance_control_set(inst, 0, 0.0f);       // the held bypass is not writable
        CHECK(lv2_instance_control_get(inst, 0) == 1.0f, "fixture %.0f Hz: the bypass stays held at 1", RATES[r]);
        for (b = 0; b < N_BLOCKS; b++)
            run_block(inst, in + b * BLOCK, out + b * BLOCK);
        g = gain_db(in, out, 1, N_BLOCKS);
        CHECK(fabs(g - -16.0) < 0.01, "fixture %.0f Hz: reads %.3f dB after the write", RATES[r], g);
        lv2_instance_free(inst);
    }
    lv2_plugin_close(p);
}

/* ---- the side chains: the main input, and one CLAP input per side chain ---- */

#define SC_URI "urn:openmixer:test:sidechain-fixture"

static void t_sidechains(void)
{
    char why[LV2_CORE_WHY_MAX] = "";
    struct lv2_plugin *p = open_plugin(g_sc, SC_URI, why);
    static const struct { const char *uri, *code; } refused[] = {
        { SC_URI "#wide", "hosting.topology.wider-than-strip" },
        { SC_URI "#extra", "hosting.topology.extra-inputs-fed-silence" },
    };
    size_t i;

    CHECK(p != NULL, "side chain: the grouped stereo effect is admitted (%s)", why);
    if (p)
    {
        CHECK(p->legs == 2 && p->in_ports[0] == 0 && p->in_ports[1] == 1 && p->out_ports[0] == 2 && p->out_ports[1] == 3,
              "side chain: the main pair is the pg:mainInput group, 0 and 1 (legs %u)", p->legs);
        CHECK(p->n_side == 1 && p->n_side_legs == 2 && p->side_channels[0] == 2 && p->side_base[0] == 0,
              "side chain: one side chain of two legs (%u chains, %u legs)", p->n_side, p->n_side_legs);
        CHECK(p->side_in[0] == 4 && p->side_in[1] == 5, "side chain: its legs are ports 4 and 5, in order (%u, %u)", p->side_in[0], p->side_in[1]);
        CHECK(p->side_name[0] && strcmp(p->side_name[0], "Sidechain") == 0, "side chain: named by its group's label (%s)", p->side_name[0] ? p->side_name[0] : "-");
    }
    lv2_plugin_close(p);

    p = open_plugin(g_sc, SC_URI "#mono", why);
    CHECK(p != NULL, "side chain: a mono side chain by lv2:isSideChain alone is admitted (%s)", why);
    if (p)
    {
        CHECK(p->legs == 2 && p->n_side == 1 && p->n_side_legs == 1 && p->side_channels[0] == 1 && p->side_in[0] == 4,
              "side chain: one leg, port 4, beside the ungrouped main pair (%u chains, %u legs)", p->n_side, p->n_side_legs);
        CHECK(p->side_name[0] && strcmp(p->side_name[0], "Side") == 0, "side chain: an ungrouped one is named by its port (%s)", p->side_name[0] ? p->side_name[0] : "-");
    }
    lv2_plugin_close(p);

    p = open_plugin(g_sc, SC_URI "#port-of", why);
    CHECK(p != NULL && p->n_side == 1 && p->side_in[0] == 4 && p->legs == 2,
          "side chain: pg:sideChainOf on the port alone makes it a side chain (%s)", why);
    lv2_plugin_close(p);

    p = open_plugin(g_fixture, FIXTURE_URI, why);
    CHECK(p != NULL && p->n_side == 0 && p->n_side_legs == 0, "side chain: a plugin with no side chain has none");
    lv2_plugin_close(p);

    for (i = 0; i < sizeof(refused) / sizeof(refused[0]); i++)
    {
        char rwhy[LV2_CORE_WHY_MAX] = "";
        struct lv2_plugin *q = open_plugin(g_sc, refused[i].uri, rwhy);

        CHECK(!q && strcmp(rwhy, refused[i].code) == 0, "side chain refusal: %s is refused %s (got '%s')", refused[i].uri, refused[i].code, rwhy);
        lv2_plugin_close(q);
    }
}

/* The open of a plugin with two side chains when the allocator refuses the name of the second: the open is refused as
 * no realisation and the name of the first, already duplicated, is freed with the rest. Unrefused, the same open
 * holds both names and the close frees them. */
static void t_sidechain_names_oom(void)
{
    char why[LV2_CORE_WHY_MAX] = "";
    struct lv2_plugin *p;
    int base = g_live_n;

    p = open_plugin(g_sc, SC_URI "#two", why);
    CHECK(p != NULL && p->n_side == 2 && strcmp(p->side_name[0], "Side A") == 0 && strcmp(p->side_name[1], "Side B") == 0,
          "side chain names: two side chains are named by their ports (%s)", why);
    lv2_plugin_close(p);
    CHECK(g_live_n == base, "side chain names: a closed plugin holds no duplicated string (%d held)", g_live_n - base);

    g_strdup_fail = "Side B";
    p = open_plugin(g_sc, SC_URI "#two", why);
    g_strdup_fail = NULL;
    CHECK(!p && strcmp(why, LV2_CODE_NO_REALISATION) == 0, "side chain names: a refused second name refuses the open (%s)", why);
    lv2_plugin_close(p);
    CHECK(g_live_n == base, "side chain names: a refused open frees the first name (%d strings held)", g_live_n - base);
}

/* ---- the refusals ---- */

static void t_refusals(void)
{
    static const struct { const char *uri, *code; } cases[] =
    {
        { FIXTURE_URI "#atom", "hosting.features.atom-in-unfed" },
        { FIXTURE_URI "#no-in", "hosting.topology.no-audio-input" },
        { FIXTURE_URI "#no-out", "hosting.topology.no-audio-output" },
        { FIXTURE_URI "#three-in", "hosting.topology.extra-inputs-fed-silence" },
        { FIXTURE_URI "#wide", "hosting.topology.wider-than-strip" },
        { FIXTURE_URI "#fixed", "hosting.features.missing" },
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        char why[LV2_CORE_WHY_MAX] = "";
        struct lv2_plugin *p = open_plugin(g_fixture, cases[i].uri, why);

        CHECK(!p && strcmp(why, cases[i].code) == 0, "refusal: %s is refused %s from the TTL (got '%s'; the binary has no descriptor for it)", cases[i].uri, cases[i].code, why);
        lv2_plugin_close(p);
    }
}

/* ---- the atom ports beside MIDI ---- */

#define ATOM_URI "urn:openmixer:test:atom-fixture"

static void t_atom_ports(void)
{
    char why[LV2_CORE_WHY_MAX] = "";
    struct lv2_plugin *p = open_plugin(g_atom, ATOM_URI, why);

    CHECK(p != NULL, "atom: the atom fixture is admitted (%s)", why);
    if (p)
    {
        CHECK(p->patch_in == 2 && p->patch_out == 3 && p->n_notify == 1 && p->notify_out[0] == 4 && p->atom_min == 8192u,
              "atom: patch ports 2 and 3 of 8192 bytes, port 4 a notify output (%d, %d, %u notify)", p->patch_in, p->patch_out, p->n_notify);
        // in the URIs' order: file, gain, name; the gain's id counts the file before it
        CHECK(p->n_patches == 1 && strcmp(p->patches[0].symbol, "gain") == 0 && p->patches[0].id == 6u,
              "atom: the gain the one numeric parameter, id 6: five ports, and the file listed before it (%u, %u)", p->n_patches,
              p->n_patches ? p->patches[0].id : 0);
        CHECK(p->n_texts == 2 && strcmp(p->texts[0].uri, ATOM_URI "#file") == 0 && p->texts[0].is_path && strcmp(p->texts[0].name, "File") == 0
                  && strcmp(p->texts[1].uri, ATOM_URI "#name") == 0 && !p->texts[1].is_path && strcmp(p->texts[1].name, "Name") == 0,
              "atom: the file a path property and the name a string property, in their URIs' order (%u)", p->n_texts);
        CHECK(lv2_plugin_text_limit(p) == 4095u, "atom: a text value up to 4095 bytes (%u)", lv2_plugin_text_limit(p));
    }
    lv2_plugin_close(p);
    // the ids are the URIs' order, not lilv's: the same at every open
    {
        uint32_t k, same = 0;

        for (k = 0; k < 16; k++)
        {
            p = open_plugin(g_atom, ATOM_URI, why);
            same += p && p->n_patches == 1 && p->patches[0].id == 6u && p->n_texts == 2 && strcmp(p->texts[0].uri, ATOM_URI "#file") == 0;
            lv2_plugin_close(p);
        }
        CHECK(same == 16, "atom: sixteen opens give the gain id 6 and the file first every time (%u of 16)", same);
    }
    p = open_plugin(g_atom, ATOM_URI "#midi-time", why);
    CHECK(p && p->midi_in_port == 2 && p->midi_time && p->time_port < 0,
          "atom: a MIDI input that takes time:Position too is the MIDI input, the transport forged into it (%s)", why);
    lv2_plugin_close(p);
    p = open_plugin(g_midi, "urn:openmixer:test:midi-echo", why);
    CHECK(p && !p->midi_time && p->n_texts == 0 && lv2_plugin_text_limit(p) == 0, "atom: the MIDI echo's input takes no time:Position, and it has no text");
    lv2_plugin_close(p);
    p = open_plugin(g_atom, ATOM_URI "#notify-many", why);
    CHECK(p && p->n_notify == 5, "atom: a fifth notify output is admitted, up to %d are (%s, %u)", LV2_CORE_NOTIFY_PORTS, why, p ? p->n_notify : 0);
    lv2_plugin_close(p);
}

/* ---- the atom control inputs that take no MIDI ---- */

#define ATOM_CODE "hosting.features.atom-in-unfed"

/* each shape of a non-MIDI atom control input: admitted, no MIDI input, and over blocks of several lengths (a transport
 * on the first) the input a valid sequence at every run, the transport's object read there when the port takes
 * time:Position, and the audio out = in x 0.5 on every frame */
static void t_atom_ui(void)
{
    // where each input lands: the patch input (and whether the transport is forged into it), the time port, or fed empty
    static const struct { const char *frag; int time, patch, time_port, fed_empty; } shapes[] =
    {
        { "#ui-lsp", 1, 1, 0, 0 }, { "#ui-seq-time", 1, 0, 1, 0 }, { "#ui-none", 0, 0, 0, 1 },
        { "#ui-patch-time", 1, 1, 0, 0 }, { "#ui-string", 0, 1, 0, 0 }, { "#object-in", 0, 0, 0, 1 },
    };
    static const uint32_t lens[] = { BLOCK, 17u, 1u, BLOCK, 40u, BLOCK };
    struct lv2_atom_probe *pr;
    size_t s;

    for (s = 0; s < sizeof(shapes) / sizeof(shapes[0]); s++)
    {
        char why[LV2_CORE_WHY_MAX] = "", uri[128];
        struct lv2_plugin *p;
        struct lv2_instance *in = NULL;
        struct lv2_time t;
        uint32_t b, i, at = 0, bad = 0;

        snprintf(uri, sizeof(uri), "%s%s", ATOM_URI, shapes[s].frag);
        p = open_plugin(g_atom, uri, why);
        CHECK(p != NULL && p->midi_in_port < 0, "atom ui %s: admitted, and its atom input is no MIDI input (%s)", shapes[s].frag, why);
        CHECK(p && (p->patch_in == 2) == shapes[s].patch && p->patch_time == (shapes[s].patch && shapes[s].time)
                  && (p->time_port == 2) == shapes[s].time_port && p->n_atom_in == (uint32_t)shapes[s].fed_empty
                  && (!shapes[s].fed_empty || p->atom_in[0] == 2),
              "atom ui %s: the input is the %s (patch %d, time %d, fed empty %u)", shapes[s].frag,
              shapes[s].patch ? "patch input" : shapes[s].time_port ? "time port" : "input fed empty",
              p ? p->patch_in : -1, p ? p->time_port : -1, p ? p->n_atom_in : 0);
        if (!p || lv2_plugin_load(p, why) != 0 || !(in = lv2_instance_new(p)) || lv2_instance_activate(in, 48000.0, 1, BLOCK, why) != 0)
        {
            CHECK(0, "atom ui %s: runs (%s)", shapes[s].frag, why);
            lv2_instance_free(in);
            lv2_plugin_close(p);
            continue;
        }
        pr = loaded_symbol(g_atom_so, "lv2_atom_probe");
        CHECK(pr != NULL, "atom ui %s: the probe of the atom fixture", shapes[s].frag);
        if (!pr)
        {
            lv2_instance_free(in);
            lv2_plugin_close(p);
            continue;
        }
        memset(pr, 0, sizeof(*pr));
        memset(&t, 0, sizeof(t));
        t.playing = 1;
        t.has = LV2_TIME_HAS_BPM;
        t.bpm = 120.0;
        for (b = 0; b < sizeof(lens) / sizeof(lens[0]); b++)
        {
            const uint32_t n = lens[b];
            float x[BLOCK];

            for (i = 0; i < n; i++)
                x[i] = 0.5f * sinf(0.0613f * (float)(at + i));
            memcpy(lv2_instance_audio_in(in, 0), x, n * sizeof(float));
            t.frame = at;
            lv2_instance_time(in, b == 0 ? &t : NULL);
            g_in_run = 1;
            lv2_instance_run(in, n, NULL, NULL);
            g_in_run = 0;
            for (i = 0; i < n; i++)
            {
                const float y = x[i] * 0.5f;

                bad += memcmp(&lv2_instance_audio_out(in, 0)[i], &y, sizeof(float)) != 0;
            }
            at += n;
        }
        CHECK(pr->ui_blocks == 6 && pr->ui_valid == 6, "atom ui %s: the input is a valid sequence at every run (%u of %u)",
              shapes[s].frag, pr->ui_valid, pr->ui_blocks);
        CHECK(pr->ui_time == (uint32_t)shapes[s].time && pr->ui_other == 0,
              "atom ui %s: %s transport object read there, and nothing else (%u objects, %u other)", shapes[s].frag,
              shapes[s].time ? "the first block's" : "no", pr->ui_time, pr->ui_other);
        CHECK(bad == 0, "atom ui %s: %u frames in blocks of 64, 17, 1, 64, 40, 64: out = in x 0.5 on every frame (%u differ)",
              shapes[s].frag, at, bad);
        lv2_instance_free(in);
        lv2_plugin_close(p);
    }
    {
        char why[LV2_CORE_WHY_MAX] = "";
        struct lv2_plugin *p = open_plugin(g_atom, ATOM_URI "#value-in", why);

        CHECK(!p && strcmp(why, ATOM_CODE) == 0, "atom ui: an atom input that is no sequence is refused " ATOM_CODE ", not as MIDI (%s)", why);
        lv2_plugin_close(p);
        p = open_plugin(g_atom, ATOM_URI "#ui-many", why);
        CHECK(p && p->n_atom_in == 5, "atom ui: a fifth control input is admitted, fed empty, up to %d are (%s, %u)", LV2_CORE_ATOM_IN_PORTS, why,
              p ? p->n_atom_in : 0);
        lv2_plugin_close(p);
    }
}

/* ---- the CV ports ---- */

#define CV_URI "urn:openmixer:test:cv-fixture"

static void t_cv(void)
{
    char why[LV2_CORE_WHY_MAX] = "";
    struct lv2_plugin *p = open_plugin(g_cv, CV_URI, why);

    CHECK(p != NULL, "cv: the CV fixture is admitted (%s)", why);
    if (p)
    {
        CHECK(p->legs == 1 && p->in_ports[0] == 0 && p->out_ports[0] == 1 && p->n_side == 0,
              "cv: its audio is one mono pair, 0 -> 1, and no CV port is read as a side chain (%u side chains)", p->n_side);
        CHECK(p->n_cv_in == 1 && p->cv_in[0] == 2 && p->cv_in_name[0] && strcmp(p->cv_in_name[0], "Mod") == 0,
              "cv: one CV input, port 2, named Mod (%u, %s)", p->n_cv_in, p->cv_in_name[0] ? p->cv_in_name[0] : "-");
        CHECK(p->n_cv_out == 1 && p->cv_out[0] == 3 && p->cv_out_name[0] && strcmp(p->cv_out_name[0], "Env") == 0,
              "cv: one CV output, port 3, named Env (%u, %s)", p->n_cv_out, p->cv_out_name[0] ? p->cv_out_name[0] : "-");
        CHECK(p->n_controls == 0, "cv: a CV port is not a control (%u controls)", p->n_controls);
    }
    lv2_plugin_close(p);
    p = open_plugin(g_cv, CV_URI "#many", why);
    CHECK(!p && strcmp(why, "hosting.features.cv-ports") == 0, "cv: nine CV inputs, one past the limit, are refused from the TTL (%s)", why);
    lv2_plugin_close(p);
    p = open_plugin(g_cv, CV_URI "#many-out", why);
    CHECK(!p && strcmp(why, "hosting.features.cv-ports") == 0, "cv: nine CV outputs, one past the limit, are refused from the TTL (%s)", why);
    lv2_plugin_close(p);
    p = open_plugin(g_fixture, FIXTURE_URI "#cv", why);
    CHECK(p && p->n_cv_in == 1 && p->cv_in[0] == 2 && p->n_cv_out == 0, "cv: the host fixture's CV twin is admitted, its CV input port 2 (%s)", why);
    lv2_plugin_close(p);
}

/* the CV fixture at `rate`: blocks of several lengths, the CV input a ramp the audio does not share; the audio output is
 * in x mod and the CV output the one-pole of its magnitude, both computed here and compared bit for bit */
static void t_cv_run(double rate)
{
    static const uint32_t lens[] = { BLOCK, 17u, 1u, BLOCK, 40u, BLOCK };
    char why[LV2_CORE_WHY_MAX] = "";
    struct lv2_plugin *p = open_plugin(g_cv, CV_URI, why);
    struct lv2_instance *in = NULL;
    uint32_t b, i, at = 0, out_bad = 0, env_bad = 0;
    float e = 0.0f;

    if (!p || lv2_plugin_load(p, why) != 0 || !(in = lv2_instance_new(p)) || lv2_instance_activate(in, rate, 1, BLOCK, why) != 0)
    {
        CHECK(0, "cv %.0f: the CV fixture runs (%s)", rate, why);
        lv2_instance_free(in);
        lv2_plugin_close(p);
        return;
    }
    for (b = 0; b < sizeof(lens) / sizeof(lens[0]); b++)
    {
        const uint32_t n = lens[b];
        float x[BLOCK], m[BLOCK];

        for (i = 0; i < n; i++)
        {
            x[i] = 0.5f * sinf(0.0613f * (float)(at + i));
            m[i] = (float)(at + i) / 256.0f - 0.25f;
        }
        memcpy(lv2_instance_audio_in(in, 0), x, n * sizeof(float));
        memcpy(lv2_instance_cv_in(in, 0), m, n * sizeof(float));
        g_in_run = 1;
        lv2_instance_run(in, n, NULL, NULL);
        g_in_run = 0;
        for (i = 0; i < n; i++)
        {
            const float y = x[i] * m[i];

            e = e + 0.125f * (fabsf(y) - e);
            out_bad += memcmp(&lv2_instance_audio_out(in, 0)[i], &y, sizeof(float)) != 0;
            env_bad += memcmp(&lv2_instance_cv_out(in, 0)[i], &e, sizeof(float)) != 0;
        }
        at += n;
    }
    CHECK(out_bad == 0, "cv %.0f: %u frames in blocks of 64, 17, 1, 64, 40, 64: the audio output is the input times the CV input on every frame (%u differ)",
          rate, at, out_bad);
    CHECK(env_bad == 0, "cv %.0f: the CV output is the one-pole of |out| on every frame, across the blocks (%u differ)", rate, env_bad);
    lv2_instance_free(in);
    lv2_plugin_close(p);
}

/* ---- the MIDI ports ---- */

/* the echo's MIDI input and output are its two atom ports, found by what they support; a twin with two MIDI inputs is
 * refused from the TTL alone, and the fixture's atom input that is no sequence is refused, but not as MIDI */
static void t_midi_ports(void)
{
    char why[LV2_CORE_WHY_MAX] = "";
    struct lv2_plugin *p = open_plugin(g_midi, "urn:openmixer:test:midi-echo", why);

    CHECK(p && p->midi_in_port == 2 && p->midi_out_port == 3 && p->legs == 1,
          "midi: the echo's atom ports are its MIDI input (2) and MIDI output (3), its audio one mono pair (%d, %d)",
          p ? p->midi_in_port : -1, p ? p->midi_out_port : -1);
    lv2_plugin_close(p);
    p = open_plugin(g_fixture, FIXTURE_URI "#atom", why);
    CHECK(!p && strcmp(why, "hosting.features.atom-in-unfed") == 0, "midi: an atom input that is no MIDI port is not refused as MIDI (%s)", why);
    lv2_plugin_close(p);
    p = open_plugin(g_midi, "urn:openmixer:test:midi-echo#synth", why);
    CHECK(p && p->midi_in_port == 0 && p->in_legs == 0 && p->legs == 2 && p->out_ports[0] == 1 && p->out_ports[1] == 2,
          "midi: an instrument, a MIDI input and a stereo output with no audio input, is admitted as a source (%s)", why);
    lv2_plugin_close(p);
    p = open_plugin(g_midi, "urn:openmixer:test:midi-echo#two-in", why);
    CHECK(p && p->midi_in_port == 2 && p->n_atom_in == 1 && p->atom_in[0] == 3,
          "midi: a second MIDI input is admitted: the first is the MIDI input, the second an input fed empty (%s)", why);
    lv2_plugin_close(p);
    p = open_plugin(g_midi, "urn:openmixer:test:midi-echo#two-out", why);
    CHECK(p && p->midi_out_port == 3 && p->n_notify == 1 && p->notify_out[0] == 4,
          "midi: a second MIDI output is admitted: the first is the MIDI output, the second an output drained (%s)", why);
    lv2_plugin_close(p);
}

/* ---- the MIDI run ---- */

#define MIDI_URI "urn:openmixer:test:midi-echo"
#define MIDI_CAPACITY 42u       // a 3-byte message takes 24 bytes of a 1016-byte body after its 8-byte sequence header

struct got
{
    uint32_t n;
    int64_t frame[64];
    uint8_t data[64][8];
    uint32_t size[64];
};

static void collect(void *ctx, int64_t frame, const uint8_t *data, uint32_t size)
{
    struct got *g = ctx;

    if (g->n < 64 && size <= 8)
    {
        g->frame[g->n] = frame;
        g->size[g->n] = size;
        memcpy(g->data[g->n], data, size);
    }
    g->n++;
}

static int same_message(const struct got *g, uint32_t i, int64_t frame, const uint8_t *data, uint32_t size)
{
    return i < g->n && i < 64 && g->frame[i] == frame && g->size[i] == size && memcmp(g->data[i], data, size) == 0;
}

/* the echo's MIDI input and output: what goes in comes out at the same frame, byte for byte, and nothing carries over */
static void t_midi_run(void)
{
    static const uint8_t note[3] = { 0x90, 0x3c, 0x64 }, pc[2] = { 0xc3, 0x05 }, sysex[6] = { 0xf0, 0x7e, 0x00, 0x06, 0x01, 0xf7 };
    static const uint8_t cc[3] = { 0xb1, 0x07, 0x7f }, rt[1] = { 0xf8 };
    char why[LV2_CORE_WHY_MAX] = "";
    struct lv2_plugin *p = open_plugin(g_midi, MIDI_URI, why);
    struct lv2_instance *in = p && lv2_plugin_load(p, why) == 0 ? lv2_instance_new(p) : NULL;
    struct lv2_counters before, after;
    struct got g;
    uint32_t i;

    CHECK(in && lv2_instance_activate(in, 48000.0, 1, BLOCK, why) == 0, "midi run: the echo instantiates and activates (%s)", why);
    if (!in)
    {
        lv2_plugin_close(p);
        return;
    }
    lv2_instance_midi_in_add(in, 0, note, 3);
    lv2_instance_midi_in_add(in, 5, pc, 2);
    lv2_instance_midi_in_add(in, 9, sysex, 6);
    lv2_instance_midi_in_add(in, 17, rt, 1);
    lv2_instance_midi_in_add(in, 63, cc, 3);
    memset(&g, 0, sizeof(g));
    lv2_instance_run(in, BLOCK, NULL, NULL);
    lv2_instance_midi_out_each(in, collect, &g);
    CHECK(g.n == 5 && same_message(&g, 0, 0, note, 3) && same_message(&g, 1, 5, pc, 2) && same_message(&g, 2, 9, sysex, 6)
              && same_message(&g, 3, 17, rt, 1) && same_message(&g, 4, 63, cc, 3),
          "midi run: five messages in, the same five out at their frames: note, a 2-byte program change, sysex, a realtime byte, a CC (%u)", g.n);

    memset(&g, 0, sizeof(g));
    lv2_instance_run(in, BLOCK, NULL, NULL);
    lv2_instance_midi_out_each(in, collect, &g);
    CHECK(g.n == 0, "midi run: a block with no input writes no output: nothing from the last block carries over (%u)", g.n);

    lv2_instance_counters(in, &before);
    for (i = 0; i < MIDI_CAPACITY + 8u; i++)
        lv2_instance_midi_in_add(in, i, note, 3);
    lv2_instance_counters(in, &after);
    memset(&g, 0, sizeof(g));
    lv2_instance_run(in, BLOCK, NULL, NULL);
    lv2_instance_midi_out_each(in, collect, &g);
    CHECK(g.n == MIDI_CAPACITY && after.midi_in_dropped - before.midi_in_dropped == 8,
          "midi run: a block that does not fit keeps the first %u messages and counts the 8 it dropped (%u, %u)",
          MIDI_CAPACITY, g.n, after.midi_in_dropped - before.midi_in_dropped);
    CHECK(same_message(&g, MIDI_CAPACITY - 1u, MIDI_CAPACITY - 1u, note, 3), "midi run: the last one that fits is at its own frame");

    CHECK(lv2_instance_midi_in_add(in, 0, note, 0) == -1, "midi run: a zero-byte message is no message, and counts nothing");

    // a size whose padding wraps to zero is refused before anything is copied: the 3-byte buffer is never read
    lv2_instance_counters(in, &before);
    CHECK(lv2_instance_midi_in_add(in, 0, note, 0xFFFFFFF9u) == -1, "midi run: a 0xFFFFFFF9-byte message is refused, not copied");
    lv2_instance_counters(in, &after);
    CHECK(after.midi_in_dropped - before.midi_in_dropped == 1, "midi run: the oversize message is counted as dropped (%u)",
          after.midi_in_dropped - before.midi_in_dropped);
    memset(&g, 0, sizeof(g));
    lv2_instance_run(in, BLOCK, NULL, NULL);
    lv2_instance_midi_out_each(in, collect, &g);
    CHECK(g.n == 0, "midi run: nothing from the refused message reaches the next block (%u)", g.n);
    lv2_instance_deactivate(in);
    lv2_instance_free(in);
    lv2_plugin_close(p);
}

/* ---- the worker gain ---- */

static struct lv2_worker_gain_probe *probe(void)
{
    return loaded_symbol(g_wg_so, "lv2_worker_gain_probe");
}

/* render tone blocks until the output sits `want_db` under the input (or `max_blocks` pass), sleeping between blocks so
 * the worker thread gets the CPU the way it would between quanta: the blocks it took, or -1 */
static int render_until(struct lv2_instance *in, double want_db, int max_blocks, double *level_db)
{
    float x[BLOCK], y[BLOCK];
    int b;

    for (b = 0; b < max_blocks; b++)
    {
        tone(x, BLOCK, 0.5f, (uint32_t)b * BLOCK);
        run_block(in, x, y);
        *level_db = 20.0 * log10(rms(y, BLOCK) / rms(x, BLOCK));
        if (fabs(*level_db - want_db) < 0.01)
            return b + 1;
        usleep(200);
    }
    return -1;
}

static int is_main(void *ctx)
{
    (void)ctx;
    return g_in_run && pthread_equal(pthread_self(), g_main);
}

static int always(void *ctx)
{
    return *(const int *)ctx;
}

static void t_worker(double rate)
{
    char why[LV2_CORE_WHY_MAX] = "";
    struct lv2_plugin *p = open_plugin(g_wg, LV2_WORKER_GAIN_URI, why);
    struct lv2_worker_gain_probe *pr;
    struct lv2_instance *in;
    struct lv2_counters c;
    int threads0, threads1, blocks, gain, yes = 1, no = 0;
    unsigned works0;
    double level = 0;

    CHECK(p && lv2_plugin_load(p, why) == 0, "worker %.0f: open and load (%s)", rate, why);
    pr = p ? probe() : NULL;
    if (!pr)
    {
        lv2_plugin_close(p);
        return;
    }
    memset(pr, 0, sizeof(*pr));
    gain = control_of(p, "gain_db");
    in = lv2_instance_new(p);
    lv2_instance_set_role(in, is_main, NULL);
    // the baseline once the last rate's worker has left /proc: a joined thread can stay listed a moment
    threads0 = thread_count_reaching(1);
    CHECK(lv2_instance_activate(in, rate, 1, BLOCK, why) == 0, "worker %.0f: a plugin requiring urid:map, worker:schedule and options activates (%s)", rate, why);
    CHECK(pr->rate_option == rate, "worker %.0f: options carried param:sampleRate %.0f", rate, pr->rate_option);
    CHECK(pr->max_block_option == (int32_t)BLOCK && pr->min_block_option == 1,
          "worker %.0f: options carried bufsz:minBlockLength %d, maxBlockLength %d (the activation's)", rate, pr->min_block_option, pr->max_block_option);
    CHECK(pr->nominal_block_option == (int32_t)BLOCK, "worker %.0f: options carried bufsz:nominalBlockLength %d, the activation's max", rate, pr->nominal_block_option);
    CHECK(pr->saw_bounded, "worker %.0f: bufsz:boundedBlockLength was in the features", rate);
    CHECK(pr->unmap_round_trip, "worker %.0f: urid:unmap gave back the URI urid:map was handed", rate);
    CHECK(pr->log_answered, "worker %.0f: log:log answered the plugin's printf", rate);
    CHECK(thread_count() == threads0 + 1, "worker %.0f: the instance owns ONE worker thread (%d -> %d threads)", rate, threads0, thread_count());
    CHECK(render_until(in, 0.0, 200, &level) > 0, "worker %.0f: at the default the output is the input (%.3f dB)", rate, level);
    works0 = atomic_load(&pr->works);
    lv2_instance_control_set(in, (uint32_t)gain, -20.0f);
    blocks = render_until(in, -20.0, 20000, &level);
    CHECK(blocks > 0, "worker %.0f: the gain reached the audio through the round trip after %d blocks: %.3f dB", rate, blocks, level);
    CHECK(atomic_load(&pr->works) > works0 && atomic_load(&pr->responses) == atomic_load(&pr->works),
          "worker %.0f: work() ran %u times, every response drained (%u)", rate, atomic_load(&pr->works), atomic_load(&pr->responses));
    CHECK(!pthread_equal(pr->work_thread, g_main) && pthread_equal(pr->run_thread, g_main),
          "worker %.0f: work() ran on the instance's own thread, not the thread that runs the plugin", rate);
    CHECK(atomic_load(&pr->mapped_in_work) > 0, "worker %.0f: urid:map answers from the worker thread", rate);
    lv2_instance_counters(in, &c);
    CHECK(atomic_load(&pr->over_block) == 0 && c.schedule_refused == 0 && c.respond_strikes == 0 && c.responses_refused == 0,
          "worker %.0f: no block over maxBlockLength, no NO_SPACE, no stray respond", rate);
    CHECK(c.map_on_audio == 0 && c.schedule_off_audio == 0, "worker %.0f: the role: map off the audio role, schedule on it (%u, %u)", rate, c.map_on_audio, c.schedule_off_audio);

    // the predicate decides: told every thread is the audio role, the worker's map is counted; told none is, the
    // schedule from run() is
    lv2_instance_set_role(in, always, &yes);
    lv2_instance_control_set(in, (uint32_t)gain, -6.0f);
    render_until(in, -6.0, 20000, &level);
    lv2_instance_set_role(in, always, &no);
    lv2_instance_control_set(in, (uint32_t)gain, -12.0f);
    render_until(in, -12.0, 20000, &level);
    lv2_instance_counters(in, &c);
    CHECK(c.map_on_audio > 0 && c.schedule_off_audio > 0, "worker %.0f: the host's predicate classifies them (map on audio %u, schedule off audio %u)", rate, c.map_on_audio, c.schedule_off_audio);

    lv2_instance_deactivate(in);
    threads1 = thread_count_reaching(threads0);
    CHECK(threads1 == threads0, "worker %.0f: the worker thread was joined at deactivate (%d threads, %d before)", rate, threads1, threads0);
    CHECK(lv2_instance_activate(in, rate, 1, BLOCK, why) == 0 && atomic_load(&pr->instantiated) == 1 && atomic_load(&pr->activated) == 2,
          "worker %.0f: the same rate activates again without instantiating (%u, %u)", rate, atomic_load(&pr->instantiated), atomic_load(&pr->activated));
    CHECK(lv2_instance_activate(in, rate * 2.0, 1, BLOCK, why) == 0 && atomic_load(&pr->instantiated) == 2 && atomic_load(&pr->cleaned_up) == 1,
          "worker %.0f: a rate change instantiates anew", rate);
    lv2_instance_free(in);
    CHECK(atomic_load(&pr->cleaned_up) == 2, "worker %.0f: cleanup ran once per instance", rate);
    lv2_plugin_close(p);
}

/* ---- the configured list ---- */

static const char *g_list[16];

static const char *const *list_without(const char *uri)
{
    uint32_t k = 0, i;

    for (i = 0; lv2_core_provided[i]; i++)
        if (strcmp(lv2_core_provided[i], uri) != 0)
            g_list[k++] = lv2_core_provided[i];
    g_list[k] = NULL;
    return g_list;
}

static const char *const *list_plus(const char *uri)
{
    uint32_t k = 0, i;

    for (i = 0; lv2_core_provided[i]; i++)
        g_list[k++] = lv2_core_provided[i];
    g_list[k++] = uri;
    g_list[k] = NULL;
    return g_list;
}

static void refused_with(const char *const *list, const char *dir, const char *uri, const char *label)
{
    const struct lv2_core_config config = { list, 1000u, 4096u, 0u, NULL, NULL };
    char why[LV2_CORE_WHY_MAX] = "";
    struct lv2_plugin *p;

    CHECK(lv2_core_configure(&config, why) == 0, "%s: configured", label);
    p = open_plugin(dir, uri, why);
    CHECK(!p && strcmp(why, "hosting.features.missing") == 0, "%s: %s refused hosting.features.missing (%s)", label, uri, why);
    CHECK(loaded_symbol(g_wg_so, "lv2_worker_gain_probe") == NULL, "%s: before its binary was opened, so before instantiate", label);
    lv2_plugin_close(p);
}

static void without_worker(void)
{
    refused_with(list_without(LV2_WORKER__schedule), g_wg, LV2_WORKER_GAIN_URI, "without worker:schedule");
}

static void without_any(void)
{
    refused_with(NULL, g_wg, LV2_WORKER_GAIN_URI, "with no list");
}

static void without_default_state(void)
{
    refused_with(list_without(LV2_STATE__loadDefaultState), g_fixture, FIXTURE_URI, "without loadDefaultState");
}

/* ---- the feature options ---- */

#define PROBE_SEVEN (LV2_FEATURE_PROBE_MAP | LV2_FEATURE_PROBE_UNMAP | LV2_FEATURE_PROBE_OPTIONS \
                     | LV2_FEATURE_PROBE_BOUNDED | LV2_FEATURE_PROBE_WORKER | LV2_FEATURE_PROBE_DEFAULT_STATE \
                     | LV2_FEATURE_PROBE_LOG)

static const uint32_t g_probe_bit[OMX_LV2_FEATURE_KINDS] =
{
    [OMX_LV2_F_MAP] = LV2_FEATURE_PROBE_MAP,
    [OMX_LV2_F_UNMAP] = LV2_FEATURE_PROBE_UNMAP,
    [OMX_LV2_F_OPTIONS] = LV2_FEATURE_PROBE_OPTIONS,
    [OMX_LV2_F_BOUNDED] = LV2_FEATURE_PROBE_BOUNDED,
    [OMX_LV2_F_WORKER] = LV2_FEATURE_PROBE_WORKER,
    [OMX_LV2_F_DEFAULT_STATE] = LV2_FEATURE_PROBE_DEFAULT_STATE,
    [OMX_LV2_F_LOG] = LV2_FEATURE_PROBE_LOG,
};

/* the probe `uri` opened, instantiated by an activation of `min` to `max` under the options now, and freed: a copy of
 * what it saw (the binary is closed with the plugin), NULL when it was refused, with `why` */
static const struct lv2_feature_probe *probe_sees(const char *uri, uint32_t min, uint32_t max, char why[LV2_CORE_WHY_MAX])
{
    static struct lv2_feature_probe copy;
    struct lv2_plugin *p = open_plugin(g_fp, uri, why);
    struct lv2_feature_probe *seen;
    struct lv2_instance *in;
    int ok;

    if (!p || lv2_plugin_load(p, why) != 0)
    {
        lv2_plugin_close(p);
        return NULL;
    }
    seen = loaded_symbol(g_fp_so, "lv2_feature_probe");
    if (seen)
        memset(seen, 0, sizeof(*seen));
    in = lv2_instance_new(p);
    ok = in && lv2_instance_activate(in, 48000.0, min, max, why) == 0 && seen;
    if (ok)
        copy = *seen;
    lv2_instance_free(in);
    lv2_plugin_close(p);
    return ok ? &copy : NULL;
}

static void t_options(void)
{
    struct omx_lv2_feature_options o;
    const struct lv2_feature_probe *seen;
    char why[LV2_CORE_WHY_MAX] = "", buf[256] = "", want[96];
    int k, in_order = 1;

    // the defaults: what this host handed a plugin before it had options
    CHECK(lv2_core_feature_options()->features == OMX_LV2_FEATURES_ALL && lv2_core_feature_options()->strict,
          "options: the host starts at the defaults");
    seen = probe_sees(LV2_FEATURE_PROBE_URI, 16, BLOCK, why);
    CHECK(seen && seen->seen == PROBE_SEVEN && seen->n_features == OMX_LV2_FEATURE_KINDS,
          "options: by default the probe sees the seven features and nothing else (%s)", why);
    for (k = 0; seen && k < OMX_LV2_FEATURE_KINDS; k++)
        in_order &= strcmp(seen->order[k], lv2_core_provided[k]) == 0;
    CHECK(seen && in_order, "options: by default in the providers' order");
    CHECK(seen && seen->min_block == 16 && seen->max_block == (int32_t)BLOCK && seen->nominal_block == (int32_t)BLOCK,
          "options: by default the activation's lengths, the nominal the maximum");
    CHECK(seen && seen->log_answer == 0, "options: by default the log formats nothing");

    // each kind withheld alone
    for (k = 0; k < OMX_LV2_FEATURE_KINDS; k++)
    {
        omx_lv2_feature_options_init(&o);
        o.features &= ~OMX_LV2_FEATURE_BIT(k);
        CHECK(lv2_core_feature_options_set(&o, why) == 0, "options: %s withheld is taken", omx_lv2_feature_uris[k]);
        seen = probe_sees(LV2_FEATURE_PROBE_URI, 16, BLOCK, why);
        CHECK(seen && seen->seen == (PROBE_SEVEN & ~g_probe_bit[k]), "options: %s withheld, the probe sees the other six (0x%x)",
              omx_lv2_feature_uris[k], seen ? seen->seen : 0u);
    }

    // strict: the twin requiring the worker, with the worker withheld
    omx_lv2_feature_options_init(&o);
    o.features &= ~OMX_LV2_FEATURE_BIT(OMX_LV2_F_WORKER);
    lv2_core_feature_options_set(&o, why);
    seen = probe_sees(LV2_FEATURE_PROBE_WORKER_URI, 16, BLOCK, why);
    CHECK(!seen && strcmp(why, LV2_CODE_FEATURES_MISSING) == 0, "options: strict, the twin requiring the worker is refused (%s)", why);
    o.strict = 0;
    lv2_core_feature_options_set(&o, why);
    seen = probe_sees(LV2_FEATURE_PROBE_WORKER_URI, 16, BLOCK, why);
    CHECK(seen && !strcmp(seen->uri, LV2_FEATURE_PROBE_WORKER_URI) && !(seen->seen & LV2_FEATURE_PROBE_WORKER),
          "options: not strict, the twin is loaded and instantiates without the worker (%s)", why);

    // the block: fixed declared admits the fixture's fixed twin, and the probe sees every declaration
    omx_lv2_feature_options_init(&o);
    lv2_core_feature_options_set(&o, why);
    CHECK(!open_plugin(g_fixture, FIXTURE_URI "#fixed", why), "options: by default the fixed twin is refused (%s)", why);
    o.block.declares = OMX_LV2_BLOCK_ALL;
    o.block.min = 1;
    o.block.nominal = 32;
    lv2_core_feature_options_set(&o, why);
    {
        struct lv2_plugin *fixed = open_plugin(g_fixture, FIXTURE_URI "#fixed", why);

        CHECK(fixed != NULL, "options: fixed declared, the fixed twin is admitted (%s)", why);
        lv2_plugin_close(fixed);
    }
    seen = probe_sees(LV2_FEATURE_PROBE_URI, 16, BLOCK, why);
    CHECK(seen && (seen->seen & (LV2_FEATURE_PROBE_FIXED | LV2_FEATURE_PROBE_POW2 | LV2_FEATURE_PROBE_BOUNDED))
                  == (LV2_FEATURE_PROBE_FIXED | LV2_FEATURE_PROBE_POW2 | LV2_FEATURE_PROBE_BOUNDED),
          "options: the probe sees bounded, fixed and power of two");
    // fixed declared, a fixed length is one number: the minimum and nominal named are overruled by the maximum
    CHECK(seen && seen->min_block == (int32_t)BLOCK && seen->max_block == (int32_t)BLOCK && seen->nominal_block == (int32_t)BLOCK,
          "options: fixed declared, the lengths are the maximum, over the minimum and nominal named (%d %d %d)",
          seen ? seen->min_block : -1, seen ? seen->max_block : -1, seen ? seen->nominal_block : -1);
    o.block.declares = OMX_LV2_BLOCK_BOUNDED | OMX_LV2_BLOCK_POW2;
    lv2_core_feature_options_set(&o, why);
    seen = probe_sees(LV2_FEATURE_PROBE_URI, 16, BLOCK, why);
    CHECK(seen && seen->min_block == 1 && seen->max_block == (int32_t)BLOCK && seen->nominal_block == 32,
          "options: not fixed, the lengths named stand over the activation's (%d %d %d)", seen ? seen->min_block : -1,
          seen ? seen->max_block : -1, seen ? seen->nominal_block : -1);

    // the log, captured
    snprintf(want, sizeof(want), LV2_FEATURE_PROBE_LINE, 48000);
    omx_lv2_feature_options_init(&o);
    o.log.mode = OMX_LV2_LOG_CAPTURE;
    o.log.capture = buf;
    o.log.capture_size = sizeof(buf);
    lv2_core_feature_options_set(&o, why);
    seen = probe_sees(LV2_FEATURE_PROBE_URI, 16, BLOCK, why);
    CHECK(seen && !strcmp(buf, want) && seen->log_answer == (int)strlen(want), "options: captured, the plugin's line is in the buffer");

    // options that do not check change nothing
    o.log.capture = NULL;
    CHECK(lv2_core_feature_options_set(&o, why) == -1 && strcmp(why, "lv2.configure.options") == 0,
          "options: a capture with no buffer is refused (%s)", why);
    CHECK(lv2_core_feature_options()->log.capture == buf, "options: and the options are unchanged");
    CHECK(lv2_core_feature_options_set(NULL, why) == 0 && lv2_core_feature_options()->log.mode == OMX_LV2_LOG_SILENT,
          "options: NULL restores the defaults");
}

/* the configured list bounds the options: a list without log:log hands no log, whatever the options offer */
static void options_within_list(void)
{
    const struct lv2_core_config config = { list_without(LV2_LOG__log), 1000u, 4096u, 0u, NULL, NULL };
    char why[LV2_CORE_WHY_MAX] = "";
    const struct lv2_feature_probe *seen;

    CHECK(lv2_core_configure(&config, why) == 0, "within the list: configured without log:log");
    CHECK(omx_lv2_feature_offered(lv2_core_feature_options(), LV2_LOG__log), "within the list: the options offer the log");
    seen = probe_sees(LV2_FEATURE_PROBE_URI, 16, BLOCK, why);
    CHECK(seen && seen->seen == (PROBE_SEVEN & ~LV2_FEATURE_PROBE_LOG), "within the list: the probe sees no log (0x%x)",
          seen ? seen->seen : 0u);
}

static void unknown_feature(void)
{
    struct lv2_core_config config = { list_plus("urn:openmixer:test:no-such-feature"), 1000u, 4096u, 0u, NULL, NULL };
    char why[LV2_CORE_WHY_MAX] = "";

    CHECK(lv2_core_configure(&config, why) == -1, "configure refuses a listed URI no provider serves (%s)", why);
    config.features = list_plus(LV2_BUF_SIZE__fixedBlockLength);
    CHECK(lv2_core_configure(&config, why) == -1, "configure refuses fixedBlockLength (the quantum is not pinned)");
    config.features = lv2_core_provided;
    config.worker_ring_bytes = 48u;
    CHECK(lv2_core_configure(&config, why) == -1, "configure refuses a ring that is not a power of two");
    config.worker_ring_bytes = 4096u;
    CHECK(lv2_core_configure(&config, why) == 0, "configure takes the provided list");
    CHECK(lv2_core_configure(&config, why) == 0, "a second configure that says the same is taken");
    config.worker_ring_bytes = 8192u;
    CHECK(lv2_core_configure(&config, why) == -1, "a second configure that says otherwise is refused (%s)", why);
}

int main(int argc, char **argv)
{
    static const struct lv2_core_config config = { lv2_core_provided, 1000u, 4096u, 0u, NULL, NULL };
    char dir[PATH_MAX + 32], why[LV2_CORE_WHY_MAX];
    size_t r;

    if (argc < 3 || abs_dir(argv[1], g_build) != 0)
    {
        printf("FAIL usage: lv2_host_test <build/lv2> <liblilv-0.so.0>\n");
        return 1;
    }
    g_lilv = argv[2];
    g_main = pthread_self();
    snprintf(dir, sizeof(dir), "%somx-host-fixture.lv2", g_build);
    if (abs_dir(dir, g_fixture) != 0)
        return 1;
    snprintf(dir, sizeof(dir), "%somx-worker-gain.lv2", g_build);
    if (abs_dir(dir, g_wg) != 0)
        return 1;
    snprintf(dir, sizeof(dir), "%somx-sidechain-fixture.lv2", g_build);
    if (abs_dir(dir, g_sc) != 0)
        return 1;
    snprintf(g_wg_so, sizeof(g_wg_so), "%somx-worker-gain.so", g_wg);
    snprintf(dir, sizeof(dir), "%somx-feature-probe.lv2", g_build);
    if (abs_dir(dir, g_fp) != 0)
        return 1;
    snprintf(g_fp_so, sizeof(g_fp_so), "%somx-feature-probe.so", g_fp);
    snprintf(dir, sizeof(dir), "%somx-midi-echo.lv2", g_build);
    if (abs_dir(dir, g_midi) != 0)
        return 1;
    snprintf(dir, sizeof(dir), "%somx-atom-fixture.lv2", g_build);
    if (abs_dir(dir, g_atom) != 0)
        return 1;
    snprintf(g_atom_so, sizeof(g_atom_so), "%somx-atom-fixture.so", g_atom);
    snprintf(dir, sizeof(dir), "%somx-cv-fixture.lv2", g_build);
    if (abs_dir(dir, g_cv) != 0)
        return 1;
    setenv("LV2_PATH", g_build, 1);     // what load_all would find: every bundle is there

    in_child("lazy", lazy);
    in_child("without worker:schedule", without_worker);
    in_child("with no list", without_any);
    in_child("without loadDefaultState", without_default_state);
    in_child("the configure", unknown_feature);
    in_child("options within the list", options_within_list);
    CHECK(!lilv_mapped(), "no lilv in this process before its first bundle");
    CHECK(lv2_core_configure(&config, why) == 0, "configure every provider (%s)", why);
    t_one_bundle();
    t_fixture();
    t_refusals();
    t_midi_ports();
    t_atom_ports();
    t_atom_ui();
    t_midi_run();
    t_sidechains();
    t_sidechain_names_oom();
    t_cv();
    for (r = 0; r < N_RATES; r++)
    {
        t_cv_run(RATES[r]);
        t_worker(RATES[r]);
    }
    t_options();
    printf("%s\n", g_failures == 0 ? "lv2 host test ok" : "lv2 host test FAILED");
    return g_failures == 0 ? 0 : 1;
}
