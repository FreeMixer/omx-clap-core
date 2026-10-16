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
        omx_clap_note_in(&instance->stage, messages[i].time, messages[i].data, messages[i].size);
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
    check_refused(path, SYNTH_MPE, CLAP_HOST_CODE_NOTE_INPUT);
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

    synth = open_synth(path, SYNTH, 2, CLAP_NOTE_DIALECT_CLAP);
    if (!synth)
        return;
    run_block(synth, running_off, 3, out_l, out_r);
    CHECK(is_sine(out_l, 10, 40, 1.0, 69, 0) && is_silent(out_l, 40, BLOCK), "a note on with velocity 0 is a note off, of its own key only");
    for (i = 0; i < sizeof(others) / sizeof(others[0]); i++)
        omx_clap_note_in(&synth->stage, others[i].time, others[i].data, others[i].size);
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
    for (i = 0; i < CLAP_HOST_NOTES_PER_BLOCK + 44; i++)
    {
        flood[i].time = i % BLOCK;
        flood[i].data[0] = 0x90;
        flood[i].data[1] = 69;
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
        if (synth_path)
            synth_checks(synth_path);
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
        synth_checks(synth_path);

    return report("clap host test ok");
}
