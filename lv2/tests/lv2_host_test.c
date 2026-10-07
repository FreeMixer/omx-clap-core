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
 * lv2_host_test.c — the bundle half and the feature providers of the LV2 host against REAL bundles, offline. Moved from
 * the console's mix_lv2_host.test.c and the LV2 arms of mix_host_backend.test.c, every number they assert unchanged.
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
 *   worker    the worker gain: options carry the rate and the block lengths, ONE worker thread per instance, the gain
 *             reaches the audio only through the round trip, work() on that thread, the thread joined at deactivate.
 *   role      the host's audio-role predicate classifies urid:map and schedule_work.
 *   features  the configured list, provided whole or refused: a list without worker:schedule (or none) refuses a plugin
 *             requiring it before instantiate; a URI no provider serves refuses the configure.
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
#include "fixtures/lv2_worker_gain.h"
#include "lv2_test_util.h"

#define BLOCK 64u
#define N_BLOCKS 16u
#define FIXTURE_URI "urn:openmixer:test:host-fixture"

static char g_fixture[PATH_MAX], g_wg[PATH_MAX], g_wg_so[PATH_MAX + 32], g_build[PATH_MAX];
static const char *g_lilv;
static pthread_t g_main;
static volatile int g_in_run;      // the main thread holds the audio role only inside a block

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
    struct lv2_core_config config = { lv2_core_provided, 1000u, 4096u, 0u, NULL };
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

    CHECK(f && lv2_bundle_count(f) == 8, "bundle: the fixture bundle lists its 8 plugins (%u)", lv2_bundle_count(f));
    p = f ? lv2_plugin_open(f, LV2_WORKER_GAIN_URI, why) : NULL;
    CHECK(!p && strcmp(why, "hosting.no-realisation") == 0, "bundle: LV2_PATH holds the worker gain, the fixture bundle does not know it: one bundle, never load_all (%s)", why);
    w = lv2_bundle_ref(g_wg, why);
    CHECK(w && lv2_bundle_count(w) == 1 && strcmp(lv2_bundle_uri(w, 0), LV2_WORKER_GAIN_URI) == 0, "bundle: the worker gain's own bundle lists it");
    p = w ? lv2_plugin_open(w, LV2_WORKER_GAIN_URI, why) : NULL;
    CHECK(p != NULL, "bundle: the worker gain's own bundle opens it (%s)", why);
    lv2_plugin_close(p);
    p = f ? lv2_plugin_open(f, LV2_WORKER_GAIN_URI, why) : NULL;
    CHECK(!p, "bundle: with the worker gain's bundle loaded too, the fixture bundle still does not hold it: the plugin must be THAT bundle's (%s)", why);
    CHECK(lv2_bundle_count(f) == 8, "bundle: and does not list it");
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

/* ---- the refusals ---- */

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
    threads0 = thread_count_reaching(thread_count());
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
    const struct lv2_core_config config = { list, 1000u, 4096u, 0u, NULL };
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

static void unknown_feature(void)
{
    struct lv2_core_config config = { list_plus("urn:openmixer:test:no-such-feature"), 1000u, 4096u, 0u, NULL };
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
    static const struct lv2_core_config config = { lv2_core_provided, 1000u, 4096u, 0u, NULL };
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
    snprintf(g_wg_so, sizeof(g_wg_so), "%somx-worker-gain.so", g_wg);
    setenv("LV2_PATH", g_build, 1);     // what load_all would find: every bundle is there

    in_child("lazy", lazy);
    in_child("without worker:schedule", without_worker);
    in_child("with no list", without_any);
    in_child("without loadDefaultState", without_default_state);
    in_child("the configure", unknown_feature);
    CHECK(!lilv_mapped(), "no lilv in this process before its first bundle");
    CHECK(lv2_core_configure(&config, why) == 0, "configure every provider (%s)", why);
    t_one_bundle();
    t_fixture();
    t_refusals();
    for (r = 0; r < N_RATES; r++)
        t_worker(RATES[r]);
    printf("%s\n", g_failures == 0 ? "lv2 host test ok" : "lv2 host test FAILED");
    return g_failures == 0 ? 0 : 1;
}
