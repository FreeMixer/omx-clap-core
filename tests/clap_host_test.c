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

/* The core in the isolated host's configuration against a real plugin, on this thread, no jack:
 * open, activate, parameters by id, a tone through process, state to a buffer and back into a second instance, close.
 * Written for omx-delay.clap (parameter 0 = time in ms, 2 = mix, 5 = its own bypass). A second argument names
 * tests/fake.clap, whose layouts the core must refuse with the hosting code, and whose passthrough reports a latency.
 * A third argument names tests/fake_synth.clap: the instrument layouts the core admits and refuses, and the notes it
 * makes from MIDI, to the sample. */

#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#include "../src/clap_host.h"
#include "test_util.h"

#define PLUGIN_ID       "org.freemixer.openmixer.delay"
#define BLOCKS          64
#define PARAM_TIME_MS   0
#define PARAM_MIX       2
#define PARAM_BYPASS    5
#define FAKE_LATENCY        64
#define FAKE_LATENCY_NEXT   128
#define SETTLE_US       50000

typedef struct MIDI_AT_T {
    uint32_t time;
    uint8_t data[3];
    uint8_t size;
} midi_at_t;

/* what omx-clap-host sets, as effects.c does */
static int configure(void)
{
    struct omx_clap_host_config config;

    omx_clap_host_config_default(&config);
    config.clamp = 0;
    config.nonfinite = 0;
    config.warmup = 0;
    config.note_inputs = 1;
    config.preset_load = 1;
    config.name = "omx-clap-host";
    config.vendor = "Pau Aliagas";
    config.url = "";
    return omx_clap_host_configure(&config);
}

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* the writes and the tick the way the host's verbs make them: a write no cycle drains yet is delivered by the control thread */
static int param_set(struct omx_clap_instance *instance, clap_id id, double value)
{
    uint32_t state;

    if (omx_clap_host_param_write(instance, id, value) != 0)
        return -1;
    state = atomic_load(&instance->stage.state);
    if (state == OMX_CLAP_IDLE || state == OMX_CLAP_ARMED)
        omx_clap_host_param_deliver(instance);
    return 0;
}

static int param_get(struct omx_clap_instance *instance, clap_id id, double *value)
{
    omx_clap_host_settle(instance, SETTLE_US);
    return omx_clap_host_param_read(instance, id, value);
}

static void idle(struct omx_clap_instance *instance)
{
    const int restart = omx_clap_host_tick(instance);

    if (restart && instance->active)
        omx_clap_host_restart(instance);
    omx_clap_host_settle(instance, SETTLE_US);
}

static void check_refused(const char *path, const char *id, const char *code)
{
    struct omx_clap_instance *instance = NULL;
    char why[OMX_CLAP_WHY_MAX];
    int ret = omx_clap_host_open(path, id, &instance, why);

    CHECK(ret == -1 && instance == NULL, "%s refused (%i)", id, ret);
    CHECK(strcmp(why, code) == 0, "%s names the code %s (%s)", id, code, why);
    if (instance)
        omx_clap_host_close(instance);
}

static int open_ok(const char *path, const char *id, struct omx_clap_instance **instance)
{
    char why[OMX_CLAP_WHY_MAX];

    return omx_clap_host_open(path, id, instance, why) == 0 && *instance != NULL;
}

static int activate(struct omx_clap_instance *instance)
{
    char why[OMX_CLAP_WHY_MAX];

    return omx_clap_host_activate(instance, SAMPLE_RATE, BLOCK, why) == 0;
}

static void guard_checks(struct omx_clap_instance *instance)
{
    CHECK(activate(instance), "activate for the guard pages");
    CHECK(!dies_writing(&instance->stage.h.out_r[BLOCK - 1]), "the last sample of the bounce is writable");
    CHECK(dies_writing(&instance->stage.h.out_r[BLOCK]), "the first sample past the bounce faults on the guard page");
    CHECK(dies_writing(instance->bounce_map), "the guard page below the bounce faults");
}

/* three threads of their own, each asking the plugin's clap.thread-check once the test lets them go */
struct role_probes
{
    struct omx_clap_instance *instance;
    pthread_barrier_t go;
    pthread_t threads[3];
    int audio[3];
};

struct role_probe
{
    struct role_probes *all;
    int index;
};

static void *probe_role(void *arg)
{
    const struct role_probe *probe = arg;
    struct omx_clap_instance *instance = probe->all->instance;
    const clap_host_thread_check_t *check = instance->host.get_extension(&instance->host, CLAP_EXT_THREAD_CHECK);

    pthread_barrier_wait(&probe->all->go);
    probe->all->audio[probe->index] = check->is_audio_thread(&instance->host);
    return NULL;
}

/* the walk split across a driver and two workers: the first two probes are the workers */
static int workers_role(void *ctx, pthread_t self)
{
    const struct role_probes *probes = ctx;

    return pthread_equal(self, probes->threads[0]) || pthread_equal(self, probes->threads[1]);
}

/* a running instance whose workers changed: the third probe alone */
static int third_role(void *ctx, pthread_t self)
{
    const struct role_probes *probes = ctx;

    return pthread_equal(self, probes->threads[2]);
}

enum role_call { ROLE_PUBLISH, ROLE_SET_THREAD, ROLE_PUBLISH_PREDICATE, ROLE_SET_PREDICATE };

/* start the probes, name the audio role with `call`, let them ask: their answers in `probes->audio`, this thread's returned */
static int ask_role(struct omx_clap_instance *instance, enum role_call call, struct role_probes *probes)
{
    const clap_host_thread_check_t *check = instance->host.get_extension(&instance->host, CLAP_EXT_THREAD_CHECK);
    struct role_probe probe[3];
    int i;

    probes->instance = instance;
    pthread_barrier_init(&probes->go, NULL, 4);
    for (i = 0; i < 3; i++)
    {
        probe[i].all = probes;
        probe[i].index = i;
        probes->audio[i] = -1;
        pthread_create(&probes->threads[i], NULL, probe_role, &probe[i]);
    }
    switch (call)
    {
    case ROLE_PUBLISH:
        omx_clap_host_publish(instance, probes->threads[0]);
        break;
    case ROLE_SET_THREAD:
        omx_clap_host_set_audio_thread(instance, probes->threads[0]);
        break;
    case ROLE_PUBLISH_PREDICATE:
        omx_clap_host_publish_role(instance, pthread_self(), workers_role, probes);
        break;
    case ROLE_SET_PREDICATE:
        omx_clap_host_set_audio_role(instance, pthread_self(), third_role, probes);
        break;
    }
    pthread_barrier_wait(&probes->go);
    for (i = 0; i < 3; i++)
        pthread_join(probes->threads[i], NULL);
    pthread_barrier_destroy(&probes->go);
    return check->is_audio_thread(&instance->host);
}

/* the audio role as the plugin reads it: one thread named, or a driver and the workers a predicate names */
static void role_checks(const char *path)
{
    struct omx_clap_instance *instance = NULL;
    struct role_probes probes;
    int self;

    CHECK(open_ok(path, FAKE_PASSTHROUGH, &instance), "open %s for the audio role", FAKE_PASSTHROUGH);
    if (!instance)
        return;
    CHECK(activate(instance), "activate for the audio role");

    // the one-thread calls: the thread named reads true, every other false
    self = ask_role(instance, ROLE_PUBLISH, &probes);
    CHECK(probes.audio[0] == 1 && probes.audio[1] == 0 && probes.audio[2] == 0 && self == 0,
          "publish names one thread: %i %i %i, this one %i", probes.audio[0], probes.audio[1], probes.audio[2], self);
    CHECK(omx_clap_host_unpublish(instance, 100, 100000) == 0, "unpublished");
    self = ask_role(instance, ROLE_SET_THREAD, &probes);
    CHECK(probes.audio[0] == 1 && probes.audio[1] == 0 && probes.audio[2] == 0 && self == 0,
          "set_audio_thread names one thread: %i %i %i, this one %i", probes.audio[0], probes.audio[1], probes.audio[2], self);
    omx_clap_host_arm(instance);
    CHECK(omx_clap_host_unpublish(instance, 100, 100000) == 0, "unpublished");

    // the split walk: the driver and the two workers the predicate names read true, the third thread false
    self = ask_role(instance, ROLE_PUBLISH_PREDICATE, &probes);
    CHECK(probes.audio[0] == 1 && probes.audio[1] == 1 && probes.audio[2] == 0 && self == 1,
          "publish_role: both workers %i %i, the third %i, the driver %i", probes.audio[0], probes.audio[1], probes.audio[2], self);
    self = ask_role(instance, ROLE_SET_PREDICATE, &probes);
    CHECK(probes.audio[0] == 0 && probes.audio[1] == 0 && probes.audio[2] == 1 && self == 1,
          "set_audio_role on a running instance: %i %i %i, the driver %i", probes.audio[0], probes.audio[1], probes.audio[2], self);
    CHECK(omx_clap_host_unpublish(instance, 100, 100000) == 0 && atomic_load(&instance->audio_role_is) == NULL,
          "unpublish clears the predicate");

    // a one-thread publish after a predicate names that thread alone
    self = ask_role(instance, ROLE_PUBLISH_PREDICATE, &probes);
    CHECK(omx_clap_host_unpublish(instance, 100, 100000) == 0, "unpublished");
    omx_clap_host_publish_role(instance, pthread_self(), workers_role, &probes);
    omx_clap_host_publish(instance, probes.threads[2]);
    CHECK(atomic_load(&instance->audio_role_is) == NULL, "publish clears the predicate");
    omx_clap_host_set_audio_role(instance, pthread_self(), workers_role, &probes);
    omx_clap_host_set_audio_thread(instance, probes.threads[2]);
    CHECK(atomic_load(&instance->audio_role_is) == NULL, "set_audio_thread clears the predicate");
    CHECK(omx_clap_host_unpublish(instance, 100, 100000) == 0, "unpublished");
    CHECK(atomic_load(&instance->thread_violations) == 0, "%u thread-check violations", atomic_load(&instance->thread_violations));
    omx_clap_host_close(instance);
}

/* the configuration is the isolated host's: nothing is clamped, nothing scanned, nothing warmed up */
static void configuration_checks(const char *path)
{
    struct omx_clap_instance *instance = NULL;
    float in_l[BLOCK], in_r[BLOCK], out_l[BLOCK], out_r[BLOCK];
    const float *inputs[2] = { in_l, in_r };
    float *outputs[2] = { out_l, out_r };
    double value;
    uint32_t b;

    CHECK(open_ok(path, FAKE_PASSTHROUGH, &instance), "open %s", FAKE_PASSTHROUGH);
    if (!instance)
        return;
    CHECK(strcmp(instance->host.name, "omx-clap-host") == 0 && strcmp(instance->host.vendor, "Pau Aliagas") == 0,
          "the plugin is told which host it is in (%s, %s)", instance->host.name, instance->host.vendor);
    CHECK(activate(instance), "activate");
    CHECK(omx_clap_host_param_read(instance, FAKE_PARAM_ACTIVATIONS, &value) == 0 && value == 1.0, "no warm-up restart: activated %g time", value);
    CHECK(omx_clap_host_param_read(instance, FAKE_PARAM_PROCESS_CALLS, &value) == 0 && value == 0.0, "no warm-up: %g process calls before the first cycle", value);
    CHECK(omx_clap_host_param_read(instance, FAKE_PARAM_HOST_PRESET, &value) == 0 && value == 1.0, "the preset-load host extension is offered");
    omx_clap_host_publish(instance, pthread_self());

    // +40 dBFS and then a non-finite block: the isolated host passes both, as mod-host does for an LV2 plugin
    fill_tone(in_l, BLOCK, 0, 100.0f);
    memcpy(in_r, in_l, sizeof(in_l));
    for (b = 0; b < 3; b++)
        omx_clap_run_io(&instance->stage, inputs, outputs, BLOCK);
    CHECK(max_abs(out_l, BLOCK) > 90.0f && memcmp(out_l, in_l, sizeof(in_l)) == 0 && atomic_load(&instance->stage.h.clamped_samples) == 0,
          "no clamp: +40 dBFS passes unchanged (peak %g)", (double)max_abs(out_l, BLOCK));
    in_l[3] = NAN;
    in_r[3] = INFINITY;
    for (b = 0; b < 4; b++)
        omx_clap_run_io(&instance->stage, inputs, outputs, BLOCK);
    CHECK(isnan(out_l[3]) && atomic_load(&instance->stage.h.fault) == OMX_HOSTED_FAULT_NONE && atomic_load(&instance->stage.h.nonfinite_blocks) == 0,
          "no scan: a non-finite block passes and strikes nothing");
    omx_clap_host_close(instance);

    // once set, and after a binary was opened, a second configuration is refused
    CHECK(configure() == -1, "the process is configured once");
}

/* open must fail with the hosting code */
/* the fake's org.openmixer.param-contract/1 as FAKE_CONTRACT arms it: trusted whole, or not at all, naming the rule */
static void contract_open(const char *path, const char *arm, struct omx_clap_instance **instance)
{
    if (arm)
        setenv("FAKE_CONTRACT", arm, 1);
    else
        unsetenv("FAKE_CONTRACT");
    *instance = NULL;
    CHECK(open_ok(path, FAKE_PASSTHROUGH, instance), "open %s with FAKE_CONTRACT %s", FAKE_PASSTHROUGH, arm ? arm : "unset");
}

static void contract_refused(const char *path, const char *arm, const char *rule)
{
    struct omx_clap_instance *instance;
    struct omx_clap_param_meaning m;
    const char *why = NULL;
    uint32_t version = 99;

    contract_open(path, arm, &instance);
    if (!instance)
        return;
    CHECK(omx_clap_host_param_contract(instance, &version, &why) == 0 && version == 0, "%s: uncontracted", arm);
    CHECK(why && strncmp(why, rule, strlen(rule)) == 0, "%s: the reason names %s (%s)", arm, rule, why ? why : "NULL");
    CHECK(omx_clap_host_param_meaning(instance, 0, &m) == -1, "%s: no meaning for parameter 0, not even its sound half", arm);
    omx_clap_host_tick(instance);
    why = omx_clap_host_log_take(instance);
    CHECK(why && strstr(why, "parameter contract is not trusted") && strstr(why, rule), "%s: said in the log (%s)", arm, why ? why : "NULL");
    omx_clap_host_close(instance);
}

static void contract_checks(const char *path)
{
    struct omx_clap_instance *instance;
    struct omx_clap_param_meaning m;
    const char *why = NULL;
    uint32_t version = 0;
    char text[64];

    contract_open(path, NULL, &instance);
    if (instance)
    {
        CHECK(omx_clap_host_param_contract(instance, &version, &why) == 0 && why && strcmp(why, "absent") == 0,
              "no extension: uncontracted, \"absent\" (%s)", why ? why : "NULL");
        CHECK(omx_clap_host_param_meaning(instance, 0, &m) == -1, "no extension: no meaning");
        omx_clap_host_tick(instance);
        CHECK(omx_clap_host_log_take(instance) == NULL, "no extension: nothing said in the log");
        omx_clap_host_close(instance);
    }

    contract_open(path, "good", &instance);
    if (instance)
    {
        CHECK(omx_clap_host_param_contract(instance, &version, &why) == 1 && version == 1 && why == NULL,
              "good: contracted, version %u", version);
        CHECK(omx_clap_host_param_meaning(instance, 0, &m) == 0 && m.id == 0 && strcmp(m.symbol, "latency") == 0
              && m.unit == 6 && m.scale == 2 && m.step == 1.0 && m.min == 0.0 && m.max == 4096.0 && m.def == 64.0
              && (m.flags & CLAP_PARAM_IS_STEPPED),
              "good: parameter 0 is latency, frame, stepped, step 1, 0..4096, default 64");
        CHECK(strcmp(omx_clap_param_unit_name(m.unit), "frame") == 0 && strcmp(omx_clap_param_scale_name(m.scale), "stepped") == 0,
              "good: named frame and stepped");
        CHECK(omx_clap_host_param_meaning(instance, FAKE_PARAM_PROCESS_CALLS, &m) == 0 && strcmp(m.symbol, "process_calls") == 0
              && m.unit == 0 && (m.flags & CLAP_PARAM_IS_READONLY), "good: a read-only parameter has its meaning too");
        CHECK(omx_clap_host_param_meaning(instance, 99, &m) == -1, "good: no meaning for an id clap.params does not list");
        omx_clap_host_close(instance);
    }

    contract_open(path, "migrated", &instance);
    if (instance)
    {
        CHECK(omx_clap_host_param_contract(instance, &version, &why) == 1 && version == 2,
              "migrated: a rename step at version 2 is contracted (version %u)", version);
        omx_clap_host_close(instance);
    }

    contract_refused(path, "fingerprint", "R4");
    contract_refused(path, "missing", "R1");
    contract_refused(path, "duplicate", "R1");
    contract_refused(path, "symbol", "R2");
    contract_refused(path, "step", "R2");
    contract_refused(path, "noop", "R5");
    unsetenv("FAKE_CONTRACT");

    CHECK(omx_clap_param_unit_name(0x103) && strcmp(omx_clap_param_unit_name(0x103), "lufs") == 0
          && omx_clap_param_unit_name(25) == NULL && omx_clap_param_scale_name(3) == NULL, "the unit and scale tables");
    CHECK(omx_clap_param_number(0.10000000149011612, text, sizeof(text)) > 0 && strcmp(text, "0.10000000149011612") == 0,
          "a float's travel in the shortest decimal that reads back (%s)", text);
    CHECK(omx_clap_param_number(NAN, text, sizeof(text)) == -1 && omx_clap_param_number(1.0, text, 1) == -1,
          "no number for NaN, nor in a buffer too small");
}

static void fake_plugin_checks(const char *path)
{
    struct omx_clap_instance *instance = NULL;
    float in_l[BLOCK], in_r[BLOCK], out_l[BLOCK], out_r[BLOCK];
    const float *inputs[2] = { in_l, in_r };
    float *outputs[2] = { out_l, out_r };
    double value;

    CHECK(CLAP_HOST_MAIN_PORT_CHANNELS == 2, "CLAP_HOST_MAIN_PORT_CHANNELS is %u", CLAP_HOST_MAIN_PORT_CHANNELS);
    check_refused(path, FAKE_WIDE, CLAP_HOST_CODE_WIDER_THAN_STRIP);
    check_refused(path, FAKE_WIDEN, CLAP_HOST_CODE_WIDER_THAN_STRIP);
    CHECK(omx_clap_host_binaries_open() == 0, "the fake binary is closed after the refusals (%u open)", omx_clap_host_binaries_open());

    CHECK(open_ok(path, FAKE_SIDECHAIN, &instance), "open %s: a side chain is admitted", FAKE_SIDECHAIN);
    if (instance)
    {
        CHECK(instance->aux_inputs == 1 && instance->aux_in_channels[0] == 1, "one mono side chain (%u inputs, %u channels)",
              instance->aux_inputs, instance->aux_in_channels[0]);
        CHECK(instance->in_channels == 2 && instance->channels == 2, "beside the effect's audio pair (%u in, %u out)", instance->in_channels, instance->channels);
        omx_clap_host_close(instance);
        instance = NULL;
    }
    CHECK(omx_clap_host_binaries_open() == 0, "the side chain's binary is closed again (%u open)", omx_clap_host_binaries_open());

    CHECK(open_ok(path, FAKE_NOTES, &instance), "open %s: a note input is admitted", FAKE_NOTES);
    if (instance)
    {
        CHECK(instance->note_inputs == 1 && instance->note_dialect == CLAP_NOTE_DIALECT_CLAP, "one note input, fed the CLAP dialect");
        CHECK(instance->in_channels == 2 && instance->channels == 2, "and the effect's audio pair is kept (%u in, %u out)",
              instance->in_channels, instance->channels);
        omx_clap_host_close(instance);
    }

    role_checks(path);

    CHECK(open_ok(path, FAKE_PASSTHROUGH, &instance), "open %s for the guard pages", FAKE_PASSTHROUGH);
    if (instance)
    {
        guard_checks(instance);
        omx_clap_host_close(instance);
    }

    CHECK(open_ok(path, FAKE_AUXOUT, &instance), "open %s: an auxiliary output is admitted", FAKE_AUXOUT);
    if (instance)
    {
        fill_tone(in_l, BLOCK, 0, 0.5f);
        memcpy(in_r, in_l, sizeof(in_l));
        CHECK(instance->aux_outputs == 1 && instance->aux_channels[0] == 2, "one auxiliary output of two channels (%u, %u)", instance->aux_outputs, instance->aux_channels[0]);
        CHECK(activate(instance), "activate with the auxiliary output");
        omx_clap_host_publish(instance, pthread_self());
        omx_clap_run_io(&instance->stage, inputs, outputs, BLOCK);
        omx_clap_run_io(&instance->stage, inputs, outputs, BLOCK);
        CHECK(atomic_load(&instance->stage.process_errors) == 0 && memcmp(out_l, in_l, sizeof(in_l)) == 0,
              "the plugin was handed the auxiliary buffers (%u process errors) and the main output is its input", atomic_load(&instance->stage.process_errors));
        omx_clap_host_close(instance);
    }

    CHECK(open_ok(path, FAKE_PASSTHROUGH, &instance), "open %s", FAKE_PASSTHROUGH);
    if (!instance)
        return;
    CHECK(instance->latency != NULL, "latency extension");
    CHECK(activate(instance), "activate");
    CHECK(omx_clap_host_latency(instance) == FAKE_LATENCY, "latency %u frames read at activate", omx_clap_host_latency(instance));

    CHECK(param_set(instance, 0, FAKE_LATENCY_NEXT) == 0, "param_set 0 = %i while idle", FAKE_LATENCY_NEXT);
    CHECK(param_get(instance, 0, &value) == 0 && value == FAKE_LATENCY_NEXT, "param_get 0 = %g", value);
    idle(instance);
    CHECK(atomic_load(&instance->restart_requested) == 0 && instance->active, "the restart it asked for ran on idle");
    CHECK(omx_clap_host_latency(instance) == FAKE_LATENCY_NEXT, "latency %u frames after the change", omx_clap_host_latency(instance));

    omx_clap_host_close(instance);
    CHECK(omx_clap_host_binaries_open() == 0, "fake binary closed");
}

/* one block of a synth on this thread: the MIDI messages first, at the frames given, then the cycle */
static void run_block(struct omx_clap_instance *instance, const midi_at_t *messages, uint32_t count, float *out_l, float *out_r)
{
    float *outputs[2] = { out_l, out_r };
    uint32_t i;

    for (i = 0; i < count; i++)
        omx_clap_host_note_in(instance, messages[i].time, messages[i].data, messages[i].size);
    omx_clap_run_io(&instance->stage, NULL, outputs, BLOCK);
}

static float sine_at(double velocity, int key, uint32_t age)
{
    const double hz = 440.0 * pow(2.0, ((double)key - 69.0) / 12.0);

    return (float)(velocity * sin(2.0 * M_PI * hz * (double)age / SAMPLE_RATE));
}

/* the samples in [from, to) are the synth's sine, `age` frames in when `from` is, within float rounding */
static int is_sine(const float *buffer, uint32_t from, uint32_t to, double velocity, int key, uint32_t age)
{
    uint32_t i;

    for (i = from; i < to; i++)
        if (fabsf(buffer[i] - sine_at(velocity, key, age + i - from)) > 1e-6f)
            return 0;
    return 1;
}

static struct omx_clap_instance *open_synth(const char *path, const char *id, uint32_t outputs, uint32_t dialect)
{
    struct omx_clap_instance *instance = NULL;
    float prime_l[BLOCK], prime_r[BLOCK];

    CHECK(open_ok(path, id, &instance), "open %s", id);
    if (!instance)
        return NULL;
    CHECK(instance->in_channels == 0 && instance->channels == outputs, "no main input, %u output channels (%u, %u)",
          outputs, instance->in_channels, instance->channels);
    CHECK(instance->note_inputs == 1 && instance->note_dialect == dialect, "one note input, dialect %u (%u, %u)", dialect,
          instance->note_inputs, instance->note_dialect);
    CHECK(activate(instance), "activate %s", id);
    omx_clap_host_publish(instance, pthread_self());
    // the first cycle after arming fades the plugin in over the block, as it does for an effect
    run_block(instance, NULL, 0, prime_l, prime_r);
    return instance;
}

/* the same notes through either dialect make the same samples: on at frame 100, off at frame 50 of the next block */
static void synth_notes_checks(const char *path, const char *id, uint32_t outputs, uint32_t dialect)
{
    static const midi_at_t on[] = { { 100, { 0x90, 69, 100 }, 3 } };
    static const midi_at_t off[] = { { 50, { 0x80, 69, 64 }, 3 } };
    const double velocity = 100.0 / 127.0;
    float out_l[BLOCK], out_r[BLOCK];
    struct omx_clap_instance *synth = open_synth(path, id, outputs, dialect);

    if (!synth)
        return;
    run_block(synth, NULL, 0, out_l, out_r);
    CHECK(is_silent(out_l, 0, BLOCK), "%s: silence before a note", id);
    run_block(synth, on, 1, out_l, out_r);
    CHECK(is_silent(out_l, 0, 100), "%s: silent up to the frame the note on arrived on", id);
    CHECK(is_sine(out_l, 100, BLOCK, velocity, 69, 0), "%s: the sine starts on frame 100, phase 0, amplitude %.4f", id, velocity);
    CHECK(outputs == 1 || is_sine(out_r, 100, BLOCK, velocity, 69, 0), "%s: on every output channel", id);
    run_block(synth, off, 1, out_l, out_r);
    CHECK(is_sine(out_l, 0, 50, velocity, 69, BLOCK - 100), "%s: the note sounds up to the frame of the note off", id);
    CHECK(is_silent(out_l, 50, BLOCK), "%s: silence from the note off on", id);
    run_block(synth, NULL, 0, out_l, out_r);
    CHECK(is_silent(out_l, 0, BLOCK), "%s: and stays silent", id);
    CHECK(atomic_load(&synth->stage.notes_delivered) == 2 && atomic_load(&synth->stage.notes_dropped) == 0, "%s: two notes delivered, none dropped (%u, %u)", id,
          atomic_load(&synth->stage.notes_delivered), atomic_load(&synth->stage.notes_dropped));
    CHECK(atomic_load(&synth->stage.process_errors) == 0 && atomic_load(&synth->thread_violations) == 0, "%s: no process error, no thread violation", id);
    omx_clap_host_close(synth);
}

/* What synth-poly saw: the fake's own counters, read through the symbol fake_synth.clap exports (the core holds the
 * file open, so this dlopen shares it). */
typedef struct
{
    uint32_t notes_on, ids_reused, no_id, ended, end_refused, voices, voices_max;
    uint32_t host_dialects, voice_count, ports_gone, renamed;
} poly_stats_t;

static const poly_stats_t *poly_stats(const char *path)
{
    void *file = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    const poly_stats_t *(*stats)(void) = file ? (const poly_stats_t *(*)(void))dlsym(file, "fake_synth_poly_stats") : NULL;
    const poly_stats_t *answer = stats ? stats() : NULL;

    if (file)
        dlclose(file);
    return answer;
}

/*
 * Note ids through the core, against synth-poly, a voice per id that pushes NOTE_END FAKE_SYNTH_RELEASE frames after
 * its note off. 40 blocks of 32 note ons, each block ending the previous block's 32: 1280 notes, five times the pool,
 * and up to 32 held and 160 in their release at once. The core frees an id only on its NOTE_END (the hold is far longer
 * than the run), so no note is skipped, no id is handed out while its voice still sounds, and every id is free at the end.
 */
#define POLY_PER_BLOCK  32u
#define POLY_BLOCKS     40u
static void synth_ids_checks(const char *path)
{
    midi_at_t m[2 * POLY_PER_BLOCK];
    float out_l[BLOCK], out_r[BLOCK];
    struct omx_clap_instance *synth = open_synth(path, SYNTH_POLY, 2, CLAP_NOTE_DIALECT_CLAP);
    const poly_stats_t *seen;
    uint32_t b, k, n, note = 0;

    if (!synth)
        return;
    seen = poly_stats(path);
    CHECK(seen != NULL, "synth-poly's counters are read through fake_synth_poly_stats");
    if (!seen)
    {
        omx_clap_host_close(synth);
        return;
    }
    CHECK(synth->note_ids.hold_frames == (uint32_t)(SAMPLE_RATE * OMX_CLAP_NOTE_ID_HOLD_MS / 1000.0) &&
          synth->note_ids.hold_frames > (POLY_BLOCKS + 8) * BLOCK,
          "the hold is %u ms at the activated rate, %u frames, longer than the run", OMX_CLAP_NOTE_ID_HOLD_MS, synth->note_ids.hold_frames);
    for (b = 0; b < POLY_BLOCKS + 8; b++)
    {
        n = 0;
        for (k = 0; b > 0 && b <= POLY_BLOCKS && k < POLY_PER_BLOCK; k++)    // the previous block's notes end
        {
            const uint32_t prev = note - POLY_PER_BLOCK + k;

            m[n++] = (midi_at_t){ k, { (uint8_t)(0x80 | (prev / 128 % 16)), (uint8_t)(prev % 128), 0 }, 3 };
        }
        for (k = 0; b < POLY_BLOCKS && k < POLY_PER_BLOCK; k++, note++)
            m[n++] = (midi_at_t){ 64 + k, { (uint8_t)(0x90 | (note / 128 % 16)), (uint8_t)(note % 128), 100 }, 3 };
        run_block(synth, m, n, out_l, out_r);
    }
    CHECK(seen->notes_on == POLY_BLOCKS * POLY_PER_BLOCK && seen->no_id == 0, "%u notes reached the synth, each with an id (%u, %u without)",
          POLY_BLOCKS * POLY_PER_BLOCK, seen->notes_on, seen->no_id);
    CHECK(seen->ids_reused == 0, "no id was handed out while a voice still sounded under it (%u)", seen->ids_reused);
    CHECK(atomic_load(&synth->note_ids.notes_skipped) == 0 && atomic_load(&synth->note_ids.ids_expired) == 0,
          "no note skipped and no hold ran out: NOTE_END alone freed the ids (%u skipped, %u expired)",
          atomic_load(&synth->note_ids.notes_skipped), atomic_load(&synth->note_ids.ids_expired));
    CHECK(seen->ended == POLY_BLOCKS * POLY_PER_BLOCK && seen->end_refused == 0 &&
          atomic_load(&synth->note_ids.notes_ended) == POLY_BLOCKS * POLY_PER_BLOCK,
          "every voice ended with NOTE_END and the core freed its id (%u pushed, %u freed)", seen->ended,
          atomic_load(&synth->note_ids.notes_ended));
    CHECK(synth->note_ids.n_free == OMX_CLAP_NOTE_IDS && seen->voices == 0, "every id is free again (%u of %u)",
          synth->note_ids.n_free, OMX_CLAP_NOTE_IDS);
    CHECK(seen->voices_max > POLY_PER_BLOCK, "voices overlapped: up to %u at once", seen->voices_max);
    omx_clap_host_close(synth);
}

/*
 * clap.voice-info through the core: synth-poly declares 4 of 8 voices with overlapping notes, and answers only while
 * active, so the declaration is read at activate and nothing is read before it; it is kept across a deactivate. synth
 * offers no voice-info and reads as no declaration, every output 0, never a count of 0 voices.
 */
#define POLY_VOICE_COUNT     4u     /* the fake's FAKE_SYNTH_VOICE_COUNT */
#define POLY_VOICE_CAPACITY  8u     /* the fake's FAKE_SYNTH_VOICE_CAPACITY */
static void synth_voice_checks(const char *path)
{
    struct omx_clap_instance *synth = NULL;
    uint32_t count = 99, capacity = 99;
    uint64_t flags = 99;

    CHECK(open_ok(path, SYNTH_POLY, &synth), "open %s", SYNTH_POLY);
    if (!synth)
        return;
    CHECK(omx_clap_host_voice_info(synth, &count, &capacity, &flags) == 0 && count == 0 && capacity == 0 && flags == 0,
          "%s before activate: no voice info, all 0 (%u, %u, %llu)", SYNTH_POLY, count, capacity, (unsigned long long)flags);
    CHECK(activate(synth), "activate %s", SYNTH_POLY);
    CHECK(omx_clap_host_voice_info(synth, &count, &capacity, &flags) == 1 && count == POLY_VOICE_COUNT &&
              capacity == POLY_VOICE_CAPACITY && flags == CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES,
          "%s after activate: %u of %u voices, overlapping notes (%u, %u, %llu)", SYNTH_POLY, POLY_VOICE_COUNT,
          POLY_VOICE_CAPACITY, count, capacity, (unsigned long long)flags);
    CHECK(omx_clap_host_voice_info(synth, NULL, &capacity, NULL) == 1 && capacity == POLY_VOICE_CAPACITY,
          "the capacity alone, the other pointers NULL (%u)", capacity);
    omx_clap_host_deactivate(synth);
    CHECK(omx_clap_host_voice_info(synth, &count, &capacity, NULL) == 1 && count == POLY_VOICE_COUNT && capacity == POLY_VOICE_CAPACITY,
          "the last activate's declaration is kept across a deactivate (%u, %u)", count, capacity);
    omx_clap_host_close(synth);

    synth = open_synth(path, SYNTH, 2, CLAP_NOTE_DIALECT_CLAP);
    if (!synth)
        return;
    count = capacity = 99;
    flags = 99;
    CHECK(omx_clap_host_voice_info(synth, &count, &capacity, &flags) == 0 && count == 0 && capacity == 0 && flags == 0,
          "%s offers no voice-info: absent, all 0 (%u, %u, %llu)", SYNTH, count, capacity, (unsigned long long)flags);
    omx_clap_host_close(synth);
    CHECK(omx_clap_host_voice_info(NULL, &count, NULL, NULL) == 0 && count == 0, "no instance: no voice info");
}

/* one of synth-poly's hooks into its host (fake_synth_poly_rescan, _rename, _revoice), through the file the core holds */
static void *poly_hook(const char *path, const char *name)
{
    void *file = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    void *hook = file ? dlsym(file, name) : NULL;

    if (file)
        dlclose(file);
    return hook;
}

/*
 * The host's side of an instrument's details, against synth-poly: its clap.note-ports asks what the host can feed and
 * hears every dialect a note input is fed in; its three note names read through the core, the third refused by the
 * plugin, and again after it says they changed; a voice count it says changed is asked again by the tick while active;
 * a rescan of the note names is no restart, a rescan of every note port while active is, and the restart reads the
 * note ports again, refusing ports open would have refused.
 */
#define NOTE_PORTS_GONE (1u << 31)      /* the fake's FAKE_SYNTH_PORTS_GONE */
static void synth_host_details_checks(const char *path)
{
    struct omx_clap_instance *synth = NULL;
    int (*rescan)(uint32_t);
    int (*rename)(void);
    int (*revoice)(uint32_t);
    const poly_stats_t *seen;
    clap_note_name_t name;
    uint32_t count = 0;
    char why[OMX_CLAP_WHY_MAX];

    CHECK(open_ok(path, SYNTH_POLY, &synth), "open %s", SYNTH_POLY);
    if (!synth)
        return;
    seen = poly_stats(path);
    rescan = (int (*)(uint32_t))poly_hook(path, "fake_synth_poly_rescan");
    rename = (int (*)(void))poly_hook(path, "fake_synth_poly_rename");
    revoice = (int (*)(uint32_t))poly_hook(path, "fake_synth_poly_revoice");
    CHECK(seen && rescan && rename && revoice, "synth-poly's counters and hooks are read through its exports");
    if (!seen || !rescan || !rename || !revoice)
    {
        omx_clap_host_close(synth);
        return;
    }
    CHECK(seen->host_dialects == OMX_CLAP_HOST_NOTE_DIALECTS &&
              seen->host_dialects == (CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE | CLAP_NOTE_DIALECT_MIDI2),
          "the host's note-ports answers CLAP, MIDI, MIDI MPE and MIDI 2.0 (%#x)", seen->host_dialects);

    CHECK(omx_clap_host_note_name_count(synth) == 3, "three note names are counted (%u)", omx_clap_host_note_name_count(synth));
    CHECK(omx_clap_host_note_name(synth, 0, &name) == 0 && !strcmp(name.name, "Kick") && name.port == -1 && name.key == 36 &&
              name.channel == -1,
          "note name 0: Kick, key 36, every port and channel (%s %d %d %d)", name.name, name.port, name.key, name.channel);
    CHECK(omx_clap_host_note_name(synth, 1, &name) == 0 && !strcmp(name.name, "Snare \"rim\"") && name.port == 0 && name.key == 38 &&
              name.channel == 9,
          "note name 1: the snare on key 38 of channel 9, port 0 (%s %d %d %d)", name.name, name.port, name.key, name.channel);
    CHECK(omx_clap_host_note_name(synth, 2, &name) == -1 && omx_clap_host_note_name(synth, 3, &name) == -1 &&
              omx_clap_host_note_name(synth, 0, NULL) == -1,
          "a name the plugin refuses, an index past the count and no output answer -1");
    CHECK(omx_clap_host_note_names_changed(synth) == 0, "no change said yet");
    CHECK(rename() == 0, "the plugin says its note names changed, through the host's clap.note-name");
    CHECK(omx_clap_host_note_names_changed(synth) == 1 && omx_clap_host_note_names_changed(synth) == 0, "the change reads once");
    CHECK(omx_clap_host_note_name(synth, 0, &name) == 0 && !strcmp(name.name, "Bass drum"), "and the new name reads (%s)", name.name);

    CHECK(activate(synth), "activate %s", SYNTH_POLY);
    CHECK(omx_clap_host_voice_info(synth, &count, NULL, NULL) == 1 && count == POLY_VOICE_COUNT, "%u voices at activate (%u)",
          POLY_VOICE_COUNT, count);
    CHECK(revoice(6) == 0, "the plugin says its voice info changed, through the host's clap.voice-info");
    CHECK(omx_clap_host_voice_info(synth, &count, NULL, NULL) == 1 && count == POLY_VOICE_COUNT, "nothing is asked before the tick (%u)", count);
    CHECK(omx_clap_host_tick(synth) == 0, "the tick asks for no restart for it");
    CHECK(omx_clap_host_voice_info(synth, &count, NULL, NULL) == 1 && count == 6, "the tick asked again: 6 voices (%u)", count);

    CHECK(rescan(CLAP_NOTE_PORTS_RESCAN_NAMES) == 0 && omx_clap_host_tick(synth) == 0, "a rescan of the note names is no restart");
    CHECK(omx_clap_host_note_ports_rescanned(synth) == CLAP_NOTE_PORTS_RESCAN_NAMES && omx_clap_host_note_ports_rescanned(synth) == 0,
          "the names rescan reads once");
    CHECK(rescan(CLAP_NOTE_PORTS_RESCAN_ALL) == 0 && omx_clap_host_tick(synth) != 0, "a rescan of every note port while active asks for a restart");
    CHECK(omx_clap_host_note_ports_rescanned(synth) == CLAP_NOTE_PORTS_RESCAN_ALL, "and reads as RESCAN_ALL");
    CHECK(omx_clap_host_restart(synth) == 0 && synth->note_inputs == 1 && synth->note_dialect == CLAP_NOTE_DIALECT_CLAP &&
              omx_clap_host_tick(synth) == 0,
          "the restart reads the same note input again and asks for no other (%u, %u)", synth->note_inputs, synth->note_dialect);

    CHECK(rescan(CLAP_NOTE_PORTS_RESCAN_ALL | NOTE_PORTS_GONE) == 0 && omx_clap_host_tick(synth) != 0,
          "the plugin takes its note input away and rescans every port");
    omx_clap_host_deactivate(synth);
    why[0] = '\0';
    CHECK(omx_clap_host_activate(synth, 48000.0, BLOCK, why) == -1 && !strcmp(why, CLAP_HOST_CODE_NOTE_INPUT) && synth->note_inputs == 1,
          "an instrument left with no note input is refused at activate: %s, the old reading kept (%s)", CLAP_HOST_CODE_NOTE_INPUT, why);
    omx_clap_host_close(synth);
}

/*
 * Note output through the core, against synth-arp: a note on of key 60 on channel 2 at frame 100 sends 60, 64 and 67, a
 * step of FAKE_ARP_STEP_MS apart, each at its own frame across the blocks it falls in; the note off sends their three
 * note offs at its own frame, and the NOTE_END the arpeggiator pushes for the input note is not note output. Read after
 * every block as the audio role reads it, at every rate the contract declares, through the CLAP dialect and through
 * CLAP_EVENT_MIDI (synth-arp-midi): the same bytes at the same frames.
 */
#define ARP_STEP_MS     2.0     /* the fake's FAKE_ARP_STEP_MS */
#define ARP_OUT_MAX     16u
static uint32_t arp_blocks(struct omx_clap_instance *arp, const midi_at_t *messages, uint32_t count, uint32_t blocks,
                           struct omx_clap_note_out_event *got, uint32_t have)
{
    float out_l[BLOCK], out_r[BLOCK];
    uint32_t b;

    for (b = 0; b < blocks; b++)
    {
        run_block(arp, b == 0 ? messages : NULL, b == 0 ? count : 0, out_l, out_r);
        have += omx_clap_host_notes_out(arp, got + have, ARP_OUT_MAX - have);
    }
    return have;
}

static void synth_out_checks(const char *path, const char *id, double rate)
{
    static const midi_at_t on[] = { { 100, { 0x91, 60, 100 }, 3 } };
    static const midi_at_t off[] = { { 10, { 0x81, 60, 0 }, 3 } };
    static const uint8_t keys[] = { 60, 64, 67 };
    const int64_t step = (int64_t)lround(rate * ARP_STEP_MS / 1000.0);
    const uint32_t blocks = (uint32_t)(2 * step / BLOCK) + 2;
    struct omx_clap_note_out_event got[ARP_OUT_MAX];
    struct omx_clap_instance *arp = NULL;
    char why[OMX_CLAP_WHY_MAX];
    float out_l[BLOCK], out_r[BLOCK];
    int64_t start;
    uint32_t n, k, ok;

    CHECK(open_ok(path, id, &arp), "open %s", id);
    if (!arp)
        return;
    CHECK(arp->note_outputs == 1 && arp->note_inputs == 1, "%s: one note output read at open, beside its note input (%u, %u)", id,
          arp->note_outputs, arp->note_inputs);
    CHECK(omx_clap_host_activate(arp, rate, BLOCK, why) == 0, "%s: activate at %.0f (%s)", id, rate, why);
    omx_clap_host_publish(arp, pthread_self());
    run_block(arp, NULL, 0, out_l, out_r);
    CHECK(omx_clap_host_notes_out(arp, got, ARP_OUT_MAX) == 0, "%s @%.0f: nothing sent out before a note", id, rate);

    start = arp->stage.steady_time;
    n = arp_blocks(arp, on, 1, blocks, got, 0);
    ok = n == 3;
    for (k = 0; ok && k < 3; k++)
        ok = got[k].frame == start + 100 + (int64_t)k * step && got[k].time == (uint32_t)((100 + k * step) % BLOCK) && got[k].size == 3 &&
             got[k].data[0] == 0x91 && got[k].data[1] == keys[k] && got[k].data[2] == 100;
    CHECK(ok, "%s @%.0f: one note on gives 60, 64 and 67 at frames +100, +%lld and +%lld, each in its block (%u events; first %lld %02x %u %u)",
          id, rate, (long long)(100 + step), (long long)(100 + 2 * step), n, n ? (long long)(got[0].frame - start) : -1LL,
          n ? got[0].data[0] : 0, n ? got[0].data[1] : 0, n ? got[0].data[2] : 0);

    start = arp->stage.steady_time;
    n = arp_blocks(arp, off, 1, 1, got, 0);
    ok = n == 3;
    for (k = 0; ok && k < 3; k++)
        ok = got[k].frame == start + 10 && got[k].time == 10 && got[k].size == 3 && got[k].data[0] == 0x81 && got[k].data[1] == keys[k] &&
             got[k].data[2] == 0;
    CHECK(ok, "%s @%.0f: its note off gives their three note offs at its own frame, and the plugin's NOTE_END goes nowhere (%u events)",
          id, rate, n);
    CHECK(atomic_load(&arp->note_out.dropped) == 0 && atomic_load(&arp->note_out.unmapped) == 0,
          "%s @%.0f: nothing dropped, nothing unmapped (%u, %u)", id, rate, atomic_load(&arp->note_out.dropped),
          atomic_load(&arp->note_out.unmapped));
    omx_clap_host_close(arp);
}

static void synth_out_all(const char *path)
{
    static const int rates[] = OMX_ORACLE_FULL_RATES_INIT;
    size_t r;

    for (r = 0; r < sizeof(rates) / sizeof(rates[0]); r++)
    {
        synth_out_checks(path, SYNTH_ARP, (double)rates[r]);
        synth_out_checks(path, SYNTH_ARP_MIDI, (double)rates[r]);
    }
    CHECK(omx_clap_host_binaries_open() == 0, "the arpeggiator's binary is closed (%u open)", omx_clap_host_binaries_open());
}

static void synth_checks(const char *path)
{
    static const midi_at_t running_off[] = { { 10, { 0x90, 69, 127 }, 3 }, { 20, { 0x90, 60, 0 }, 3 }, { 40, { 0x90, 69, 0 }, 3 } };
    static const midi_at_t others[] = { { 0, { 0xb0, 1, 64 }, 3 }, { 1, { 0xe0, 0, 64 }, 3 }, { 2, { 0xc0, 5, 0 }, 2 }, { 3, { 0xf8, 0, 0 }, 1 } };
    static const midi_at_t held[] = { { 0, { 0x90, 69, 127 }, 3 } };
    midi_at_t flood[CLAP_HOST_NOTES_PER_BLOCK + 45];
    float out_l[BLOCK], out_r[BLOCK];
    float stop_l[BLOCK], stop_r[BLOCK];
    struct omx_clap_instance *synth;
    uint32_t i;

    check_refused(path, SYNTH_WIDE, CLAP_HOST_CODE_WIDER_THAN_STRIP);
    check_refused(path, SYNTH_NOTES, CLAP_HOST_CODE_NOTE_INPUT);
    check_refused(path, SILENT, CLAP_HOST_CODE_NO_AUDIO_INPUT);
    CHECK(omx_clap_host_binaries_open() == 0, "the synth binary is closed after the refusals (%u open)", omx_clap_host_binaries_open());

    synth = open_synth(path, SYNTH_AUX, 2, CLAP_NOTE_DIALECT_CLAP);
    if (synth)
    {
        CHECK(synth->aux_inputs == 1 && synth->aux_in_channels[0] == 1, "an instrument's side chain is admitted, one mono input (%u inputs, %u channels)",
              synth->aux_inputs, synth->aux_in_channels[0]);
        omx_clap_request_stop(&synth->stage);
        run_block(synth, NULL, 0, stop_l, stop_r);
        CHECK(omx_clap_host_unpublish(synth, 10, 1000000) == 0, "unpublish the instrument with a side chain");
        omx_clap_host_close(synth);
    }

    synth_notes_checks(path, SYNTH, 2, CLAP_NOTE_DIALECT_CLAP);
    synth_notes_checks(path, SYNTH_MIDI, 1, CLAP_NOTE_DIALECT_MIDI);
    // a port that prefers MIDI with MPE reads raw MIDI, the MPE dialect set: the synth plays from CLAP_EVENT_MIDI alone
    synth_notes_checks(path, SYNTH_MPE, 2, CLAP_NOTE_DIALECT_MIDI_MPE);
    // a port that speaks only MIDI 2.0 gets CLAP_EVENT_MIDI2: the synth plays from the translated channel voice messages;
    // so does one that also speaks CLAP and prefers MIDI 2.0
    synth_notes_checks(path, SYNTH_MIDI2, 1, CLAP_NOTE_DIALECT_MIDI2);
    synth_notes_checks(path, SYNTH_MIDI2_PREF, 2, CLAP_NOTE_DIALECT_MIDI2);
    synth_ids_checks(path);
    synth_voice_checks(path);
    synth_host_details_checks(path);

    synth = open_synth(path, SYNTH, 2, CLAP_NOTE_DIALECT_CLAP);
    if (!synth)
        return;
    run_block(synth, running_off, 3, out_l, out_r);
    CHECK(is_sine(out_l, 10, 40, 1.0, 69, 0) && is_silent(out_l, 40, BLOCK), "a note on with velocity 0 is a note off, of its own key only");
    for (i = 0; i < sizeof(others) / sizeof(others[0]); i++)
        omx_clap_host_note_in(synth, others[i].time, others[i].data, others[i].size);
    omx_clap_run_io(&synth->stage, NULL, (float *[]){ out_l, out_r }, BLOCK);
    CHECK(atomic_load(&synth->stage.notes_delivered) == 6 && atomic_load(&synth->stage.notes_unmappable) == 0,
          "controllers, bend and program change reach a note port that also declares the MIDI dialect, as CLAP_EVENT_MIDI; the clock does not (%u delivered, %u unmappable)",
          atomic_load(&synth->stage.notes_delivered), atomic_load(&synth->stage.notes_unmappable));

    // the bypass is silence for a plugin with nothing to pass through: one block of fade, then the plugin idles
    run_block(synth, held, 1, out_l, out_r);
    omx_clap_host_bypass(synth, 1);
    CHECK(omx_clap_host_bypassed(synth) == 1, "bypass 1");
    run_block(synth, NULL, 0, out_l, out_r);
    CHECK(!is_silent(out_l, 0, BLOCK) && out_l[BLOCK - 1] != 0.0f, "the block after bypass fades from the note (out[%u] = %g)", BLOCK - 1, (double)out_l[BLOCK - 1]);
    run_block(synth, NULL, 0, out_l, out_r);
    CHECK(is_silent(out_l, 0, BLOCK) && is_silent(out_r, 0, BLOCK), "steady bypass: silence");
    omx_clap_host_bypass(synth, 0);
    CHECK(omx_clap_host_bypassed(synth) == 0, "bypass 0");
    omx_clap_host_close(synth);

    // more messages than a cycle takes: the surplus is counted, never written past the array
    synth = open_synth(path, SYNTH, 2, CLAP_NOTE_DIALECT_CLAP);
    if (!synth)
        return;
    // each note on a key of its own (128 keys a channel), so no note on ends an earlier one under its id
    for (i = 0; i < CLAP_HOST_NOTES_PER_BLOCK + 44; i++)
    {
        flood[i].time = i % BLOCK;
        flood[i].data[0] = (uint8_t)(0x90 | (i / 128));
        flood[i].data[1] = (uint8_t)(i % 128);
        flood[i].data[2] = 100;
        flood[i].size = 3;
    }
    // the last message a note off: the note ons stop OMX_CLAP_NOTES_RESERVED short of the end, the note off arrives
    flood[CLAP_HOST_NOTES_PER_BLOCK + 44].time = BLOCK - 1;
    flood[CLAP_HOST_NOTES_PER_BLOCK + 44].data[0] = 0x80;
    flood[CLAP_HOST_NOTES_PER_BLOCK + 44].data[1] = 69;
    flood[CLAP_HOST_NOTES_PER_BLOCK + 44].data[2] = 0;
    flood[CLAP_HOST_NOTES_PER_BLOCK + 44].size = 3;
    run_block(synth, flood, CLAP_HOST_NOTES_PER_BLOCK + 45, out_l, out_r);
    CHECK(atomic_load(&synth->stage.notes_delivered) == CLAP_HOST_NOTES_PER_BLOCK - OMX_CLAP_NOTES_RESERVED + 1 &&
          atomic_load(&synth->stage.notes_dropped) == OMX_CLAP_NOTES_RESERVED + 44 && atomic_load(&synth->stage.releases_dropped) == 0,
          "%u note ons and a note off: %u delivered, the note off with them, %u dropped, %u of them releases", CLAP_HOST_NOTES_PER_BLOCK + 44,
          atomic_load(&synth->stage.notes_delivered), atomic_load(&synth->stage.notes_dropped), atomic_load(&synth->stage.releases_dropped));
    omx_clap_host_close(synth);
    CHECK(omx_clap_host_binaries_open() == 0, "synth binary closed");
}

#define MOVER           "org.omx-clap-host.test.mover"
#define MOVER_LEVEL     0
#define MOVER_MOD       1
#define MOVER_GESTURES  2
#define MOVER_TRIM      3

/* `blocks` cycles of the mover on a thread of their own, the audio role's, as a host's RT thread runs them: this thread
 * stays the main thread, never the audio role */
struct mover_cycles
{
    struct omx_clap_instance *instance;
    uint32_t blocks;
    _Atomic int go;
};

/* A cycle during a re-activation. The host's buffer-size path (omx-clap-host, effects.c) stops, deactivates and
 * activates the instance again on JACK's notification thread, under a lock of its own; the audio role never takes that
 * lock, and a cycle that lands while the instance is deactivated returns at once, runs nothing and writes nothing (the
 * stage is unbound: no lane to copy onto), so the host passes the cycle dry itself around it (effects.c process). */
struct cycle_timed { struct omx_clap_instance *instance; double ms; int dry; };
static void *cycle_timed_run(void *arg)
{
    struct cycle_timed *c = arg;
    float in_l[BLOCK], in_r[BLOCK], out_l[BLOCK], out_r[BLOCK];
    const float *inputs[2] = { in_l, in_r };
    float *outputs[2] = { out_l, out_r };
    const double t0 = now_ms();
    uint32_t i;

    for (i = 0; i < BLOCK; i++)
        in_l[i] = (float)i / BLOCK, in_r[i] = -(float)i / BLOCK, out_l[i] = out_r[i] = 7.0f;
    omx_clap_run_io(&c->instance->stage, inputs, outputs, BLOCK);
    c->ms = now_ms() - t0;
    c->dry = out_l[0] == 7.0f && out_r[0] == 7.0f;     // untouched: nothing written, nothing run
    return NULL;
}
static void test_cycle_while_deactivated(const char *path)
{
    struct omx_clap_instance *instance = NULL;
    struct cycle_timed c = { NULL, 0.0, 0 };
    pthread_t t;

    if (!open_ok(path, FAKE_PASSTHROUGH, &instance) || !activate(instance))
    {
        CHECK(0, "cycle while deactivated: the fake opens and activates");
        return;
    }
    omx_clap_host_stop(instance);
    omx_clap_host_deactivate(instance);
    c.instance = instance;
    CHECK(pthread_create(&t, NULL, cycle_timed_run, &c) == 0, "cycle while deactivated: a cycle runs on another thread");
    pthread_join(t, NULL);
    CHECK(c.ms < 100.0 && c.dry && atomic_load(&instance->stage.h.runs) == 0,
          "cycle while deactivated: it returns at once (%.1f ms), writes nothing (%d) and runs nothing (%u runs)", c.ms, c.dry,
          atomic_load(&instance->stage.h.runs));
    omx_clap_host_close(instance);
}

static void *mover_cycles_run(void *arg)
{
    struct mover_cycles *c = arg;
    float in_l[BLOCK] = { 0 }, in_r[BLOCK] = { 0 }, out_l[BLOCK], out_r[BLOCK];
    const float *inputs[2] = { in_l, in_r };
    float *outputs[2] = { out_l, out_r };
    uint32_t b;

    while (!atomic_load(&c->go))
        usleep(100);
    for (b = 0; b < c->blocks; b++)
        omx_clap_run_io(&c->instance->stage, inputs, outputs, BLOCK);
    return NULL;
}

static void mover_blocks(struct omx_clap_instance *instance, uint32_t blocks)
{
    struct mover_cycles c = { instance, blocks, 0 };
    pthread_t t;

    if (pthread_create(&t, NULL, mover_cycles_run, &c) != 0)
        return;
    if (atomic_load(&instance->stage.state) == OMX_CLAP_IDLE)
        omx_clap_host_publish(instance, t);
    else
        omx_clap_host_set_audio_thread(instance, t);
    atomic_store(&c.go, 1);
    pthread_join(t, NULL);
}

static void mover_block(struct omx_clap_instance *instance)
{
    mover_blocks(instance, 1);
}

/* the mover opened under FAKE_MOVER `action` and FAKE_MOVER_FLOOD `flood` (NULL: unset), activated and run `blocks` blocks */
static struct omx_clap_instance *mover_run(const char *path, const char *action, const char *flood, uint32_t blocks)
{
    struct omx_clap_instance *instance = NULL;

    if (action)
        setenv("FAKE_MOVER", action, 1);
    else
        unsetenv("FAKE_MOVER");
    if (flood)
        setenv("FAKE_MOVER_FLOOD", flood, 1);
    else
        unsetenv("FAKE_MOVER_FLOOD");
    CHECK(open_ok(path, MOVER, &instance), "open %s (FAKE_MOVER %s, FAKE_MOVER_FLOOD %s)", MOVER, action ? action : "unset",
          flood ? flood : "unset");
    if (!instance)
        return NULL;
    CHECK(activate(instance), "activate the mover");
    mover_blocks(instance, blocks);
    return instance;
}

/* the plugin's own parameter events, the host's modulation and gestures, rescans, clears and the plugin's text */
static void mover_checks(const char *path)
{
    struct omx_clap_instance *instance;
    struct omx_clap_param_event ev[OMX_CLAP_PARAM_OUT_DEPTH + 8];
    struct omx_clap_param_row row;
    char text[32];
    double value, applied;
    uint32_t n;

    instance = mover_run(path, NULL, NULL, 7);
    if (!instance)
        return;
    CHECK(omx_clap_host_param_events_take(instance, ev, 8) == 0, "nothing to take before the plugin moves");
    mover_block(instance);
    n = omx_clap_host_param_events_take(instance, ev, 8);
    CHECK(n == 3 && ev[0].kind == OMX_CLAP_PARAM_EVENT_GESTURE_BEGIN && ev[0].id == MOVER_LEVEL
          && ev[1].kind == OMX_CLAP_PARAM_EVENT_VALUE && ev[1].id == MOVER_LEVEL && ev[1].value == 0.75
          && ev[2].kind == OMX_CLAP_PARAM_EVENT_GESTURE_END && ev[2].id == MOVER_LEVEL,
          "the plugin's move, in order: gesture begin, the value 0.75, gesture end (%u events)", n);
    CHECK(omx_clap_host_param_events_take(instance, ev, 8) == 0 && omx_clap_host_param_events_dropped(instance) == 0,
          "taken once, none dropped");
    CHECK(omx_clap_host_param_read(instance, MOVER_LEVEL, &value) == 0 && value == 0.75, "the plugin holds 0.75 (%g)", value);
    CHECK(atomic_load(&instance->stage.plugin_changed) == 1 && atomic_load(&instance->stage.out_events_seen) == 3,
          "the stage still counts the events (%u) and marks the read-back due", atomic_load(&instance->stage.out_events_seen));

    // modulation: the plugin hears the amount, its set value stays
    CHECK(omx_clap_host_param_modulate(instance, MOVER_LEVEL, 0.25) == 0, "modulate level by 0.25");
    CHECK(omx_clap_host_param_modulate(instance, MOVER_TRIM, 0.25) == -1, "trim is not modulatable: refused");
    CHECK(omx_clap_host_param_modulate(instance, MOVER_MOD, 0.25) == -1, "a read-only parameter: refused");
    CHECK(omx_clap_host_param_modulate(instance, MOVER_LEVEL, NAN) == -1, "a NaN amount: refused");
    mover_block(instance);
    CHECK(omx_clap_host_param_read(instance, MOVER_MOD, &value) == 0 && value == 0.25, "the plugin heard the amount 0.25 (%g)", value);
    CHECK(omx_clap_host_param_read(instance, MOVER_LEVEL, &value) == 0 && value == 0.75,
          "and its set value is what it was, 0.75 (%g)", value);
    CHECK(omx_clap_host_param_compare(instance, MOVER_LEVEL, &applied) == -1,
          "a modulation writes no read-back shadow: nothing was delivered to compare");

    // the host's own gesture around a write: the plugin sees begin, the value, end
    CHECK(omx_clap_host_param_gesture(instance, MOVER_LEVEL, 1) == 0 && omx_clap_host_param_write(instance, MOVER_LEVEL, 0.6) == 0
          && omx_clap_host_param_gesture(instance, MOVER_LEVEL, 0) == 0, "a host gesture around a write, queued");
    CHECK(omx_clap_host_param_gesture(instance, MOVER_MOD, 1) == -1, "no gesture on a read-only parameter");
    mover_block(instance);
    CHECK(omx_clap_host_param_read(instance, MOVER_GESTURES, &value) == 0 && value == 2.0
          && omx_clap_host_param_read(instance, MOVER_LEVEL, &applied) == 0 && applied == 0.6,
          "the plugin saw two gestures (%g) and the value between them (%g)", value, applied);

    // the plugin's text
    CHECK(omx_clap_host_param_text(instance, MOVER_LEVEL, 0.75, text, sizeof(text)) == 0 && strcmp(text, "75%") == 0,
          "the plugin's text for 0.75 is %s", text);
    CHECK(omx_clap_host_param_parse(instance, MOVER_LEVEL, "40%", &value) == 0 && value == 0.4, "and it reads 40%% as %g", value);
    CHECK(omx_clap_host_param_text(instance, MOVER_TRIM, 0.5, text, sizeof(text)) == -1
          && omx_clap_host_param_parse(instance, MOVER_LEVEL, "loud", &value) == -1 && omx_clap_host_param_text(instance, 99, 0.5, text, sizeof(text)) == -1,
          "no text where the plugin has none, nor for an unknown id");
    idle(instance);
    CHECK(omx_clap_host_params_rescanned(instance) == 0, "no rescan without FAKE_MOVER");
    CHECK(atomic_load(&instance->thread_violations) == 0, "no host call off its thread (%u)", atomic_load(&instance->thread_violations));
    omx_clap_host_close(instance);

    // a rescan of the info: the row answers the new name, the flag is there once
    instance = mover_run(path, "info", NULL, 8);
    if (instance)
    {
        idle(instance);     // on_main_thread: the rename and the rescan
        CHECK(omx_clap_host_tick(instance) == 0 && omx_clap_host_params_rescanned(instance) == CLAP_PARAM_RESCAN_INFO,
              "params.rescan(INFO) is seen once, and asks for no restart");
        CHECK(omx_clap_host_params_rescanned(instance) == 0, "and read once");
        CHECK(omx_clap_host_param_row_of(instance, MOVER_LEVEL, &row) == 0 && strcmp(row.name, "gain") == 0,
              "the row answers the new name (%s)", row.name);
        omx_clap_host_close(instance);
    }

    // R3: the contract a rescan finds is the one the open trusted, or the instance is uncontracted from then on
    setenv("FAKE_MOVER_CONTRACT", "keep", 1);
    instance = mover_run(path, "info", NULL, 8);
    if (instance)
    {
        uint32_t version = 0;
        const char *why = NULL;
        struct omx_clap_param_meaning m;

        CHECK(omx_clap_host_param_contract(instance, &version, &why) == 1 && version == 1, "keep: the mover is contracted at open");
        idle(instance);     // on_main_thread: the rename and the rescan
        omx_clap_host_tick(instance);
        CHECK(omx_clap_host_param_contract(instance, &version, &why) == 1 && version == 1,
              "keep: a rescan that renames only the display name keeps the contract (%s)", why ? why : "NULL");
        CHECK(omx_clap_host_param_meaning(instance, MOVER_LEVEL, &m) == 0 && strcmp(m.symbol, "level") == 0,
              "keep: level's meaning is still level (%s)", m.symbol);
        omx_clap_host_close(instance);
    }
    setenv("FAKE_MOVER_CONTRACT", "follow", 1);
    instance = mover_run(path, "info", NULL, 8);
    if (instance)
    {
        uint32_t version = 0;
        const char *why = NULL, *said;
        struct omx_clap_param_meaning m;

        CHECK(omx_clap_host_param_contract(instance, &version, &why) == 1 && version == 1, "follow: the mover is contracted at open");
        idle(instance);     // on_main_thread: the rename, the symbol with it, and the rescan
        omx_clap_host_tick(instance);
        CHECK(omx_clap_host_param_contract(instance, &version, &why) == 0 && version == 0 && why && strncmp(why, "R3", 2) == 0,
              "follow: a rescan that changes a parameter's identity is refused, R3 (%s)", why ? why : "NULL");
        CHECK(omx_clap_host_param_meaning(instance, MOVER_LEVEL, &m) == -1, "follow: no meaning for level any more, old or new");
        omx_clap_host_tick(instance);
        said = omx_clap_host_log_take(instance);
        CHECK(said && strstr(said, "parameter contract is not trusted") && strstr(said, "R3"), "follow: said in the log (%s)",
              said ? said : "NULL");
        omx_clap_host_close(instance);
    }
    unsetenv("FAKE_MOVER_CONTRACT");

    // a rescan of everything while active: a restart
    instance = mover_run(path, "all", NULL, 8);
    if (instance)
    {
        omx_clap_host_tick(instance);       // on_main_thread: the rescan
        CHECK(omx_clap_host_tick(instance) == 1, "params.rescan(ALL) while active asks for a restart");
        CHECK(omx_clap_host_params_rescanned(instance) == CLAP_PARAM_RESCAN_ALL, "and is seen");
        CHECK(omx_clap_host_restart(instance) == 0 && omx_clap_host_tick(instance) == 0 && instance->active, "the restart ran");
        omx_clap_host_close(instance);
    }

    // a clear of everything: the host drops its read-back of the parameter
    instance = mover_run(path, "clear", NULL, 2);
    if (instance)
    {
        CHECK(param_set(instance, MOVER_LEVEL, 0.3) == 0, "write level 0.3 before the clear");
        mover_block(instance);
        CHECK(omx_clap_host_param_compare(instance, MOVER_LEVEL, &applied) == 1, "its read-back holds");
        mover_blocks(instance, 6);
        omx_clap_host_tick(instance);       // on_main_thread: the clear
        CHECK(omx_clap_host_param_compare(instance, MOVER_LEVEL, &applied) == -1, "params.clear(ALL): the host's read-back is gone");
        omx_clap_host_close(instance);
    }

    // more events than the ring holds: the surplus is dropped and counted, the ring keeps the first
    instance = mover_run(path, NULL, "300", 8);
    if (instance)
    {
        n = omx_clap_host_param_events_take(instance, ev, OMX_CLAP_PARAM_OUT_DEPTH + 8);
        CHECK(n == OMX_CLAP_PARAM_OUT_DEPTH && omx_clap_host_param_events_dropped(instance) == 303 - OMX_CLAP_PARAM_OUT_DEPTH,
              "303 events into a ring of %u: %u taken, %u dropped", OMX_CLAP_PARAM_OUT_DEPTH, n,
              omx_clap_host_param_events_dropped(instance));
        CHECK(ev[0].kind == OMX_CLAP_PARAM_EVENT_GESTURE_BEGIN && ev[3].kind == OMX_CLAP_PARAM_EVENT_VALUE && ev[3].id == MOVER_TRIM,
              "in order from the first");
        omx_clap_host_close(instance);
    }
    unsetenv("FAKE_MOVER_FLOOD");
    CHECK(omx_clap_host_binaries_open() == 0, "mover binary closed");
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : NULL;
    const char *fake_path = argc > 2 ? argv[2] : NULL;
    const char *synth_path = argc > 3 ? argv[3] : NULL;
    const char *mover_path = argc > 4 ? argv[4] : NULL;
    struct omx_clap_instance *first = NULL, *second = NULL;
    float in_l[BLOCK], in_r[BLOCK], out_l[BLOCK], out_r[BLOCK];
    const float *inputs[2] = { in_l, in_r };
    float *outputs[2] = { out_l, out_r };
    float max_diff = 0.0f;
    double elapsed;
    int finite = 1;
    uint32_t b, i;
    double value;
    static uint8_t state[CLAP_HOST_STATE_MAX_BYTES];
    size_t state_length = 0;

    if (!path)
    {
        fprintf(stderr, "usage: %s <omx-delay.clap | -> [fake.clap [fake_synth.clap [fake_mover.clap]]]\n", argv[0]);
        return 2;
    }
    CHECK(configure() == 0, "the process is configured as the isolated host");
    if (strcmp(path, "-") == 0)
    {
        if (!fake_path)
        {
            fprintf(stderr, "%s: '-' needs the fake plugin\n", argv[0]);
            return 2;
        }
        configuration_checks(fake_path);
        fake_plugin_checks(fake_path);
        test_cycle_while_deactivated(fake_path);
        contract_checks(fake_path);
        if (synth_path)
        {
            synth_checks(synth_path);
            synth_out_all(synth_path);
        }
        if (mover_path)
            mover_checks(mover_path);
        return report("clap host test ok");
    }

    CHECK(open_ok(path, PLUGIN_ID, &first), "open %s#%s", path, PLUGIN_ID);
    if (!first)
        return 1;

    CHECK(first->params != NULL, "params extension");
    CHECK(first->audio_ports != NULL, "audio-ports extension");
    CHECK(first->state != NULL, "state extension");
    CHECK(first->latency != NULL, "latency extension");
    printf("     preset-load extension: %s\n", first->preset_load ? "present" : "absent");
    CHECK(first->in_channels == 2 && first->channels == 2, "main ports: %u in, %u out", first->in_channels, first->channels);

    CHECK(activate(first), "activate at %.0f / %u", SAMPLE_RATE, BLOCK);
    printf("     latency: %u frames\n", omx_clap_host_latency(first));

    CHECK(param_set(first, 99, 1.0) == -1, "unknown id 99 refused");
    CHECK(param_set(first, PARAM_BYPASS, 1.0) == -1, "the bypass id is not a param_set target");
    CHECK(!omx_clap_host_param_is_row(first, PARAM_BYPASS) && omx_clap_host_param_readable(first, PARAM_BYPASS), "and is no row, though readable");

    omx_clap_host_publish(first, pthread_self());

    CHECK(param_set(first, PARAM_MIX, 1.0) == 0, "param_set %u = 1.0", PARAM_MIX);
    CHECK(param_set(first, PARAM_TIME_MS, 5.0) == 0, "param_set %u = 5.0", PARAM_TIME_MS);
    CHECK(param_get(first, PARAM_MIX, &value) == 0 && value == 1.0, "param_get %u = %g before any cycle ran", PARAM_MIX, value);

    for (b = 0; b < BLOCKS; b++)
    {
        fill_tone(in_l, BLOCK, b * BLOCK, 0.5f);
        memcpy(in_r, in_l, sizeof(in_l));
        omx_clap_run_io(&first->stage, inputs, outputs, BLOCK);
        finite = finite && all_finite(out_l, BLOCK) && all_finite(out_r, BLOCK);
        for (i = 0; i < BLOCK; i++)
        {
            const float d = fabsf(out_l[i] - in_l[i]);
            if (d > max_diff)
                max_diff = d;
        }
    }
    CHECK(finite, "%u blocks of %u frames: every output sample finite", BLOCKS, BLOCK);
    CHECK(max_diff > 1e-3f, "output differs from input (max |out - in| = %g)", (double)max_diff);
    CHECK(atomic_load(&first->stage.h.runs) == BLOCKS, "process called %u times", atomic_load(&first->stage.h.runs));
    CHECK(atomic_load(&first->stage.events_delivered) == 2, "%u parameter events delivered", atomic_load(&first->stage.events_delivered));
    CHECK(atomic_load(&first->stage.process_errors) == 0, "%u process errors", atomic_load(&first->stage.process_errors));
    CHECK(atomic_load(&first->thread_violations) == 0, "%u thread-check violations", atomic_load(&first->thread_violations));

    CHECK(param_get(first, PARAM_MIX, &value) == 0 && value == 1.0, "param_get %u = %g", PARAM_MIX, value);
    CHECK(param_get(first, PARAM_TIME_MS, &value) == 0 && value == 5.0, "param_get %u = %g", PARAM_TIME_MS, value);

    idle(first);
    CHECK(param_set(first, PARAM_TIME_MS, 7.0) == 0, "param_set %u = 7.0 while processing", PARAM_TIME_MS);
    elapsed = now_ms();
    CHECK(param_get(first, PARAM_TIME_MS, &value) == 0 && value == 7.0, "param_get %u = %g with no cycle running", PARAM_TIME_MS, value);
    elapsed = now_ms() - elapsed;
    CHECK(elapsed < 100.0, "answered in %.1f ms with no cycle coming", elapsed);
    elapsed = now_ms();
    idle(first);
    elapsed = now_ms() - elapsed;
    CHECK(elapsed < 100.0, "the idle tick returns in %.1f ms with no cycle coming", elapsed);
    fill_tone(in_l, BLOCK, 0, 0.5f);
    memcpy(in_r, in_l, sizeof(in_l));
    omx_clap_run_io(&first->stage, inputs, outputs, BLOCK);
    CHECK(atomic_load(&first->stage.state) == OMX_CLAP_PROCESSING && atomic_load(&first->stage.h.runs) == BLOCKS + 1, "the next cycle processes again (state %u, %u runs)",
          atomic_load(&first->stage.state), atomic_load(&first->stage.h.runs));
    CHECK(atomic_load(&first->stage.events_delivered) == 3, "%u parameter events delivered", atomic_load(&first->stage.events_delivered));
    CHECK(atomic_load(&first->thread_violations) == 0, "%u thread-check violations", atomic_load(&first->thread_violations));

    // bypass is the host's one-block crossfade to the dry input, the plugin's own bypass parameter untouched
    omx_clap_host_bypass(first, 1);
    fill_tone(in_l, BLOCK, BLOCK, 0.5f);
    memcpy(in_r, in_l, sizeof(in_l));
    omx_clap_run_io(&first->stage, inputs, outputs, BLOCK);
    CHECK(memcmp(out_l, in_l, sizeof(in_l)) != 0, "the block after bypass fades from wet (out[0] = %g, in[0] = %g)", (double)out_l[0], (double)in_l[0]);
    fill_tone(in_l, BLOCK, 2 * BLOCK, 0.5f);
    memcpy(in_r, in_l, sizeof(in_l));
    omx_clap_run_io(&first->stage, inputs, outputs, BLOCK);
    CHECK(memcmp(out_l, in_l, sizeof(in_l)) == 0 && memcmp(out_r, in_r, sizeof(in_r)) == 0, "steady bypass: output bit-identical to the input");
    CHECK(atomic_load(&first->stage.h.runs) == BLOCKS + 2, "the plugin idles under a steady bypass (%u runs)", atomic_load(&first->stage.h.runs));
    CHECK(param_get(first, PARAM_BYPASS, &value) == 0 && value == 0.0, "the plugin's own bypass parameter stays %g", value);
    CHECK(omx_clap_host_bypassed(first) == 1, ":bypass reads 1");
    omx_clap_host_bypass(first, 0);
    fill_tone(in_l, BLOCK, 3 * BLOCK, 0.5f);
    memcpy(in_r, in_l, sizeof(in_l));
    omx_clap_run_io(&first->stage, inputs, outputs, BLOCK);
    CHECK(out_l[0] == in_l[0] && memcmp(out_l, in_l, sizeof(in_l)) != 0, "the block after unbypass fades from dry (out[0] = %g)", (double)out_l[0]);
    CHECK(atomic_load(&first->stage.h.runs) == BLOCKS + 3, "the plugin runs again (%u runs)", atomic_load(&first->stage.h.runs));
    CHECK(omx_clap_host_bypassed(first) == 0, ":bypass reads 0");

    omx_clap_host_settle(first, SETTLE_US);
    CHECK(omx_clap_host_state_save(first, state, sizeof(state), &state_length) == 0 && state_length > 0, "state_save (%zu bytes)", state_length);
    CHECK(omx_clap_host_state_save(first, state, 4, &state_length) == -1, "a state past the buffer is refused, never truncated");
    CHECK(omx_clap_host_state_save(first, state, sizeof(state), &state_length) == 0, "and saved again whole");

    CHECK(open_ok(path, PLUGIN_ID, &second), "open a second instance");
    if (second)
    {
        CHECK(omx_clap_host_binaries_open() == 1, "one dlopen shared by two instances (%u open)", omx_clap_host_binaries_open());
        CHECK(activate(second), "activate the second instance");
        CHECK(param_get(second, PARAM_MIX, &value) == 0 && value != 1.0, "second instance starts at its default (%g)", value);
        CHECK(omx_clap_host_state_load(second, state, state_length) == 0, "state_load into the second instance");
        CHECK(param_get(second, PARAM_MIX, &value) == 0 && value == 1.0, "second instance param %u = %g after load", PARAM_MIX, value);
        CHECK(param_get(second, PARAM_TIME_MS, &value) == 0 && value == 7.0, "second instance param %u = %g after load", PARAM_TIME_MS, value);
    }

    omx_clap_host_stop(first);
    CHECK(atomic_load(&first->stage.state) == OMX_CLAP_IDLE, "stopped");
    omx_clap_host_close(first);
    if (second)
        omx_clap_host_close(second);
    CHECK(omx_clap_host_binaries_open() == 0, "binary closed after the last instance");

    if (fake_path)
    {
        configuration_checks(fake_path);
        fake_plugin_checks(fake_path);
    }
    if (synth_path)
    {
        synth_checks(synth_path);
        synth_out_all(synth_path);
    }

    return report("clap host test ok");
}
