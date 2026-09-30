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

/* The CLAP lifecycle against a real plugin, on this thread, no jack:
 * open, activate, parameters by id, a tone through process, state to a
 * file and back into a second instance, close. Written for omx-delay.clap
 * (parameter 0 = time in ms, 2 = mix, 5 = its own bypass). A second
 * argument names tests/fake.clap, whose layouts the host must refuse with
 * the reason on stderr, and whose passthrough reports a latency. A third
 * argument names tests/fake_synth.clap: the instrument layouts the host
 * admits and refuses, and the notes it makes from MIDI, to the sample. */

#include <math.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../src/clap_host.h"
#include "host-errors.h"

#define PLUGIN_ID       "org.freemixer.openmixer.delay"
#define SAMPLE_RATE     48000.0
#define BLOCK           256
#define BLOCKS          64
#define PARAM_TIME_MS   0
#define PARAM_MIX       2
#define PARAM_BYPASS    5

#define FAKE_WIDE           "org.omx-clap-host.test.wide"
#define FAKE_SIDECHAIN      "org.omx-clap-host.test.sidechain"
#define FAKE_NOTES          "org.omx-clap-host.test.notes"
#define FAKE_PASSTHROUGH    "org.omx-clap-host.test.passthrough"
#define FAKE_WIDEN          "org.omx-clap-host.test.widen"
#define FAKE_AUXOUT         "org.omx-clap-host.test.auxout"
#define SYNTH               "org.omx-clap-host.test.synth"
#define SYNTH_MIDI          "org.omx-clap-host.test.synth-midi"
#define SYNTH_AUX           "org.omx-clap-host.test.synth-aux"
#define SYNTH_WIDE          "org.omx-clap-host.test.synth-wide"
#define SYNTH_NOTES         "org.omx-clap-host.test.synth-notes"
#define SYNTH_MPE           "org.omx-clap-host.test.synth-mpe"
#define SILENT              "org.omx-clap-host.test.silent"
#define FAKE_LATENCY        64
#define FAKE_LATENCY_NEXT   128

static int g_failures;

#define CHECK(cond, ...) \
    do { \
        if (cond) { printf("ok   " __VA_ARGS__); printf("\n"); } \
        else { printf("FAIL " __VA_ARGS__); printf("\n"); g_failures++; } \
    } while (0)

static void fill_tone(float *buffer, uint32_t nframes, uint32_t offset)
{
    uint32_t i;
    for (i = 0; i < nframes; i++)
        buffer[i] = 0.5f * sinf(2.0f * (float)M_PI * 1000.0f * (float)(offset + i) / (float)SAMPLE_RATE);
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static int all_finite(const float *buffer, uint32_t nframes)
{
    uint32_t i;
    for (i = 0; i < nframes; i++)
        if (!isfinite(buffer[i]))
            return 0;
    return 1;
}

/* open must fail with -102 and print the reason */
static void check_refused(const char *path, const char *id, const char *reason)
{
    clap_instance_t *instance = NULL;
    char log_file[] = "/tmp/clap_host_test_XXXXXX";
    char captured[512];
    int fd, saved, ret;
    ssize_t n;

    fd = mkstemp(log_file);
    fflush(stderr);
    saved = dup(STDERR_FILENO);
    dup2(fd, STDERR_FILENO);
    ret = clap_host_open(path, id, &instance);
    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);

    lseek(fd, 0, SEEK_SET);
    n = read(fd, captured, sizeof(captured) - 1);
    captured[n > 0 ? n : 0] = '\0';
    close(fd);
    unlink(log_file);

    CHECK(ret == ERR_LV2_INSTANTIATION && instance == NULL, "%s refused with %i", id, ret);
    CHECK(strstr(captured, reason) != NULL, "%s names the reason \"%s\" (stderr: %s)", id, reason,
          n > 0 ? captured : "(nothing)");
    if (instance)
        clap_host_close(instance);
}

/* a write one sample past the last bounce buffer, or into the page below the bounce, dies on a guard page of its mapping;
 * the child has no core dump to leave */
static int dies_writing(volatile float *at)
{
    struct rlimit no_core = { 0, 0 };
    int status;
    pid_t pid;

    fflush(stdout);
    pid = fork();
    if (pid == 0)
    {
        setrlimit(RLIMIT_CORE, &no_core);
        *at = 1.0f;
        _exit(0);
    }
    waitpid(pid, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
}

static void guard_checks(clap_instance_t *instance)
{
    const uint32_t last = instance->output_channels - 1;

    CHECK(clap_host_activate(instance, SAMPLE_RATE, BLOCK) == SUCCESS, "activate for the guard pages");
    CHECK(!dies_writing(&instance->output_buffers[last][BLOCK - 1]), "the last sample of the bounce is writable");
    CHECK(dies_writing(&instance->output_buffers[last][BLOCK]), "the first sample past the bounce faults on the guard page");
    CHECK(dies_writing(instance->bounce_map), "the guard page below the bounce faults");
}

static void fake_plugin_checks(const char *path)
{
    clap_instance_t *instance = NULL;
    double value;

    CHECK(CLAP_HOST_MAIN_PORT_CHANNELS == 2, "CLAP_HOST_MAIN_PORT_CHANNELS is %i", CLAP_HOST_MAIN_PORT_CHANNELS);
    check_refused(path, FAKE_WIDE, CLAP_HOST_CODE_WIDER_THAN_STRIP);
    check_refused(path, FAKE_SIDECHAIN, CLAP_HOST_CODE_EXTRA_INPUTS);
    check_refused(path, FAKE_WIDEN, CLAP_HOST_CODE_WIDER_THAN_STRIP);
    CHECK(clap_host_binaries_open() == 0, "the fake binary is closed after the refusals (%u open)", clap_host_binaries_open());

    CHECK(clap_host_open(path, FAKE_NOTES, &instance) == SUCCESS && instance != NULL, "open %s: a note input is admitted", FAKE_NOTES);
    if (instance)
    {
        CHECK(instance->note_inputs == 1 && instance->note_dialect == CLAP_NOTE_DIALECT_CLAP, "one note input, fed the CLAP dialect");
        CHECK(instance->input_channels == 2 && instance->output_channels == 2, "and the effect's audio pair is kept (%u in, %u out)",
              instance->input_channels, instance->output_channels);
        clap_host_close(instance);
    }

    CHECK(clap_host_open(path, FAKE_PASSTHROUGH, &instance) == SUCCESS && instance != NULL, "open %s for the guard pages", FAKE_PASSTHROUGH);
    if (instance)
    {
        guard_checks(instance);
        clap_host_close(instance);
    }

    CHECK(clap_host_open(path, FAKE_AUXOUT, &instance) == SUCCESS && instance != NULL, "open %s: an auxiliary output is admitted", FAKE_AUXOUT);
    if (instance)
    {
        float in_l[BLOCK], in_r[BLOCK], out_l[BLOCK], out_r[BLOCK];
        const float *inputs[2] = { in_l, in_r };
        float *outputs[2] = { out_l, out_r };

        fill_tone(in_l, BLOCK, 0);
        memcpy(in_r, in_l, sizeof(in_l));
        CHECK(instance->aux_outputs == 1 && instance->aux_channels[0] == 2, "one auxiliary output of two channels (%u, %u)", instance->aux_outputs, instance->aux_channels[0]);
        CHECK(clap_host_activate(instance, SAMPLE_RATE, BLOCK) == SUCCESS, "activate with the auxiliary output");
        clap_host_set_audio_thread(instance, pthread_self());
        clap_host_arm(instance);
        clap_host_run(instance, inputs, outputs, BLOCK);
        clap_host_run(instance, inputs, outputs, BLOCK);
        CHECK(atomic_load(&instance->process_errors) == 0 && memcmp(out_l, in_l, sizeof(in_l)) == 0,
              "the plugin was handed the auxiliary buffers (%u process errors) and the main output is its input", atomic_load(&instance->process_errors));
        clap_host_close(instance);
    }

    CHECK(clap_host_open(path, FAKE_PASSTHROUGH, &instance) == SUCCESS && instance != NULL, "open %s", FAKE_PASSTHROUGH);
    if (!instance)
        return;
    CHECK(instance->latency != NULL, "latency extension");
    CHECK(clap_host_activate(instance, SAMPLE_RATE, BLOCK) == SUCCESS, "activate");
    CHECK(instance->latency_frames == FAKE_LATENCY, "latency %u frames read at activate", instance->latency_frames);

    CHECK(clap_host_param_set(instance, 0, FAKE_LATENCY_NEXT) == SUCCESS, "param_set 0 = %i while idle", FAKE_LATENCY_NEXT);
    CHECK(clap_host_param_get(instance, 0, &value) == SUCCESS && value == FAKE_LATENCY_NEXT, "param_get 0 = %g", value);
    clap_host_idle(instance);
    CHECK(atomic_load(&instance->restart_requested) == 0 && instance->active, "the restart it asked for ran on idle");
    CHECK(instance->latency_frames == FAKE_LATENCY_NEXT, "latency %u frames after the change", instance->latency_frames);

    clap_host_close(instance);
    CHECK(clap_host_binaries_open() == 0, "fake binary closed");
}

/* one block of a synth on this thread: the MIDI messages first, at the frames given, then the cycle */
typedef struct MIDI_AT_T {
    uint32_t time;
    uint8_t data[3];
    uint8_t size;
} midi_at_t;

static void run_block(clap_instance_t *instance, const midi_at_t *messages, uint32_t count, float *out_l, float *out_r)
{
    float *outputs[2] = { out_l, out_r };
    uint32_t i;

    for (i = 0; i < count; i++)
        clap_host_midi_in(instance, messages[i].time, messages[i].data, messages[i].size);
    clap_host_run(instance, NULL, outputs, BLOCK);
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

static int is_silent(const float *buffer, uint32_t from, uint32_t to)
{
    uint32_t i;

    for (i = from; i < to; i++)
        if (buffer[i] != 0.0f)
            return 0;
    return 1;
}

static clap_instance_t *open_synth(const char *path, const char *id, uint32_t outputs, uint32_t dialect)
{
    clap_instance_t *instance = NULL;
    float prime_l[BLOCK], prime_r[BLOCK];

    CHECK(clap_host_open(path, id, &instance) == SUCCESS && instance != NULL, "open %s", id);
    if (!instance)
        return NULL;
    CHECK(instance->input_channels == 0 && instance->output_channels == outputs, "no main input, %u output channels (%u, %u)",
          outputs, instance->input_channels, instance->output_channels);
    CHECK(instance->note_inputs == 1 && instance->note_dialect == dialect, "one note input, dialect %u (%u, %u)", dialect,
          instance->note_inputs, instance->note_dialect);
    CHECK(clap_host_activate(instance, SAMPLE_RATE, BLOCK) == SUCCESS, "activate %s", id);
    clap_host_set_audio_thread(instance, pthread_self());
    clap_host_arm(instance);
    /* the first cycle after arming fades the plugin in over the block, as it does for an effect */
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
    clap_instance_t *synth = open_synth(path, id, outputs, dialect);

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
    CHECK(atomic_load(&synth->notes_delivered) == 2 && atomic_load(&synth->notes_dropped) == 0, "%s: two notes delivered, none dropped (%u, %u)", id,
          atomic_load(&synth->notes_delivered), atomic_load(&synth->notes_dropped));
    CHECK(atomic_load(&synth->process_errors) == 0 && atomic_load(&synth->thread_violations) == 0, "%s: no process error, no thread violation", id);
    clap_host_close(synth);
}

static void synth_checks(const char *path)
{
    static const midi_at_t running_off[] = { { 10, { 0x90, 69, 127 }, 3 }, { 20, { 0x90, 60, 0 }, 3 }, { 40, { 0x90, 69, 0 }, 3 } };
    static const midi_at_t others[] = { { 0, { 0xb0, 1, 64 }, 3 }, { 1, { 0xe0, 0, 64 }, 3 }, { 2, { 0xc0, 5, 0 }, 2 }, { 3, { 0xf8, 0, 0 }, 1 } };
    static const midi_at_t held[] = { { 0, { 0x90, 69, 127 }, 3 } };
    midi_at_t flood[CLAP_HOST_NOTES_PER_BLOCK + 44];
    float out_l[BLOCK], out_r[BLOCK];
    clap_instance_t *synth;
    uint32_t i;

    check_refused(path, SYNTH_AUX, CLAP_HOST_CODE_EXTRA_INPUTS);
    check_refused(path, SYNTH_WIDE, CLAP_HOST_CODE_WIDER_THAN_STRIP);
    check_refused(path, SYNTH_NOTES, CLAP_HOST_CODE_NOTE_INPUT);
    check_refused(path, SYNTH_MPE, CLAP_HOST_CODE_NOTE_INPUT);
    check_refused(path, SILENT, CLAP_HOST_CODE_NO_AUDIO_INPUT);
    CHECK(clap_host_binaries_open() == 0, "the synth binary is closed after the refusals (%u open)", clap_host_binaries_open());

    synth_notes_checks(path, SYNTH, 2, CLAP_NOTE_DIALECT_CLAP);
    synth_notes_checks(path, SYNTH_MIDI, 1, CLAP_NOTE_DIALECT_MIDI);

    synth = open_synth(path, SYNTH, 2, CLAP_NOTE_DIALECT_CLAP);
    if (!synth)
        return;
    run_block(synth, running_off, 3, out_l, out_r);
    CHECK(is_sine(out_l, 10, 40, 1.0, 69, 0) && is_silent(out_l, 40, BLOCK), "a note on with velocity 0 is a note off, of its own key only");
    for (i = 0; i < sizeof(others) / sizeof(others[0]); i++)
        clap_host_midi_in(synth, others[i].time, others[i].data, others[i].size);
    clap_host_run(synth, NULL, (float *[]){ out_l, out_r }, BLOCK);
    CHECK(atomic_load(&synth->notes_delivered) == 3, "controllers, bend, program change and clock are not notes to a CLAP-dialect input (%u delivered)",
          atomic_load(&synth->notes_delivered));

    /* the bypass is silence for a plugin with nothing to pass through: one block of fade, then the plugin idles */
    run_block(synth, held, 1, out_l, out_r);
    CHECK(clap_host_bypass(synth, 1) == SUCCESS, "bypass 1");
    run_block(synth, NULL, 0, out_l, out_r);
    CHECK(!is_silent(out_l, 0, BLOCK) && out_l[BLOCK - 1] != 0.0f, "the block after bypass fades from the note (out[%u] = %g)", BLOCK - 1, (double)out_l[BLOCK - 1]);
    run_block(synth, NULL, 0, out_l, out_r);
    CHECK(is_silent(out_l, 0, BLOCK) && is_silent(out_r, 0, BLOCK), "steady bypass: silence");
    CHECK(clap_host_bypass(synth, 0) == SUCCESS, "bypass 0");
    clap_host_close(synth);

    /* more messages than a cycle takes: the surplus is counted, never written past the array */
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
    run_block(synth, flood, CLAP_HOST_NOTES_PER_BLOCK + 44, out_l, out_r);
    CHECK(atomic_load(&synth->notes_delivered) == CLAP_HOST_NOTES_PER_BLOCK && atomic_load(&synth->notes_dropped) == 44,
          "%u messages: %u delivered, %u dropped", CLAP_HOST_NOTES_PER_BLOCK + 44, atomic_load(&synth->notes_delivered), atomic_load(&synth->notes_dropped));
    clap_host_close(synth);
    CHECK(clap_host_binaries_open() == 0, "synth binary closed");
}

static int report(void)
{
    printf("%s\n", g_failures == 0 ? "clap host test ok" : "clap host test FAILED");
    return g_failures == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : NULL;
    const char *fake_path = argc > 2 ? argv[2] : NULL;
    const char *synth_path = argc > 3 ? argv[3] : NULL;
    clap_instance_t *first = NULL, *second = NULL;
    float in_l[BLOCK], in_r[BLOCK], out_l[BLOCK], out_r[BLOCK];
    const float *inputs[2] = { in_l, in_r };
    float *outputs[2] = { out_l, out_r };
    float max_diff = 0.0f;
    double elapsed;
    int finite = 1;
    uint32_t b, i;
    double value;
    char state_file[] = "/tmp/clap_host_test_XXXXXX";
    int fd;

    if (!path)
    {
        fprintf(stderr, "usage: %s <omx-delay.clap | -> [fake.clap [fake_synth.clap]]\n", argv[0]);
        return 2;
    }
    if (strcmp(path, "-") == 0)
    {
        if (!fake_path)
        {
            fprintf(stderr, "%s: '-' needs the fake plugin\n", argv[0]);
            return 2;
        }
        fake_plugin_checks(fake_path);
        if (synth_path)
            synth_checks(synth_path);
        return report();
    }

    CHECK(clap_host_open(path, PLUGIN_ID, &first) == SUCCESS && first != NULL, "open %s#%s", path, PLUGIN_ID);
    if (!first)
        return 1;

    CHECK(first->params != NULL, "params extension");
    CHECK(first->audio_ports != NULL, "audio-ports extension");
    CHECK(first->state != NULL, "state extension");
    CHECK(first->latency != NULL, "latency extension");
    printf("     preset-load extension: %s\n", first->preset_load ? "present" : "absent");
    CHECK(first->input_channels == 2 && first->output_channels == 2, "main ports: %u in, %u out",
          first->input_channels, first->output_channels);

    CHECK(clap_host_activate(first, SAMPLE_RATE, BLOCK) == SUCCESS, "activate at %.0f / %u", SAMPLE_RATE, BLOCK);
    printf("     latency: %u frames\n", first->latency_frames);

    CHECK(clap_host_param_set(first, 99, 1.0) == ERR_LV2_INVALID_PARAM_SYMBOL, "unknown id 99 refused");
    CHECK(clap_host_param_set(first, PARAM_BYPASS, 1.0) == ERR_LV2_INVALID_PARAM_SYMBOL, "the bypass id is not a param_set target");

    clap_host_set_audio_thread(first, pthread_self());
    clap_host_arm(first);

    CHECK(clap_host_param_set(first, PARAM_MIX, 1.0) == SUCCESS, "param_set %u = 1.0", PARAM_MIX);
    CHECK(clap_host_param_set(first, PARAM_TIME_MS, 5.0) == SUCCESS, "param_set %u = 5.0", PARAM_TIME_MS);
    CHECK(clap_host_param_get(first, PARAM_MIX, &value) == SUCCESS && value == 1.0, "param_get %u = %g before any cycle ran", PARAM_MIX, value);

    for (b = 0; b < BLOCKS; b++)
    {
        fill_tone(in_l, BLOCK, b * BLOCK);
        memcpy(in_r, in_l, sizeof(in_l));
        clap_host_run(first, inputs, outputs, BLOCK);
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
    CHECK(atomic_load(&first->runs) == BLOCKS, "process called %u times", atomic_load(&first->runs));
    CHECK(atomic_load(&first->events_delivered) == 2, "%u parameter events delivered", atomic_load(&first->events_delivered));
    CHECK(atomic_load(&first->process_errors) == 0, "%u process errors", atomic_load(&first->process_errors));
    CHECK(atomic_load(&first->thread_violations) == 0, "%u thread-check violations", atomic_load(&first->thread_violations));

    CHECK(clap_host_param_get(first, PARAM_MIX, &value) == SUCCESS && value == 1.0, "param_get %u = %g", PARAM_MIX, value);
    CHECK(clap_host_param_get(first, PARAM_TIME_MS, &value) == SUCCESS && value == 5.0, "param_get %u = %g", PARAM_TIME_MS, value);

    clap_host_idle(first);
    CHECK(clap_host_param_set(first, PARAM_TIME_MS, 7.0) == SUCCESS, "param_set %u = 7.0 while processing", PARAM_TIME_MS);
    elapsed = now_ms();
    CHECK(clap_host_param_get(first, PARAM_TIME_MS, &value) == SUCCESS && value == 7.0, "param_get %u = %g with no cycle running", PARAM_TIME_MS, value);
    elapsed = now_ms() - elapsed;
    CHECK(elapsed < 100.0, "answered in %.1f ms with no cycle coming", elapsed);
    elapsed = now_ms();
    clap_host_idle(first);
    elapsed = now_ms() - elapsed;
    CHECK(elapsed < 100.0, "the idle tick returns in %.1f ms with no cycle coming", elapsed);
    fill_tone(in_l, BLOCK, 0);
    memcpy(in_r, in_l, sizeof(in_l));
    clap_host_run(first, inputs, outputs, BLOCK);
    CHECK(atomic_load(&first->run_state) == CLAP_HOST_PROCESSING && atomic_load(&first->runs) == BLOCKS + 1, "the next cycle processes again (state %u, %u runs)",
          atomic_load(&first->run_state), atomic_load(&first->runs));
    CHECK(atomic_load(&first->events_delivered) == 3, "%u parameter events delivered", atomic_load(&first->events_delivered));
    CHECK(atomic_load(&first->thread_violations) == 0, "%u thread-check violations", atomic_load(&first->thread_violations));

    /* bypass is the host's one-block crossfade to the dry input, the plugin's own bypass parameter untouched */
    CHECK(clap_host_bypass(first, 1) == SUCCESS, "bypass 1");
    fill_tone(in_l, BLOCK, BLOCK);
    memcpy(in_r, in_l, sizeof(in_l));
    clap_host_run(first, inputs, outputs, BLOCK);
    CHECK(memcmp(out_l, in_l, sizeof(in_l)) != 0, "the block after bypass fades from wet (out[0] = %g, in[0] = %g)", (double)out_l[0], (double)in_l[0]);
    fill_tone(in_l, BLOCK, 2 * BLOCK);
    memcpy(in_r, in_l, sizeof(in_l));
    clap_host_run(first, inputs, outputs, BLOCK);
    CHECK(memcmp(out_l, in_l, sizeof(in_l)) == 0 && memcmp(out_r, in_r, sizeof(in_r)) == 0, "steady bypass: output bit-identical to the input");
    CHECK(atomic_load(&first->runs) == BLOCKS + 2, "the plugin idles under a steady bypass (%u runs)", atomic_load(&first->runs));
    CHECK(clap_host_param_get(first, PARAM_BYPASS, &value) == SUCCESS && value == 0.0, "the plugin's own bypass parameter stays %g", value);
    CHECK(clap_host_bypassed(first) == 1, ":bypass reads 1");
    CHECK(clap_host_bypass(first, 0) == SUCCESS, "bypass 0");
    fill_tone(in_l, BLOCK, 3 * BLOCK);
    memcpy(in_r, in_l, sizeof(in_l));
    clap_host_run(first, inputs, outputs, BLOCK);
    CHECK(out_l[0] == in_l[0] && memcmp(out_l, in_l, sizeof(in_l)) != 0, "the block after unbypass fades from dry (out[0] = %g)", (double)out_l[0]);
    CHECK(atomic_load(&first->runs) == BLOCKS + 3, "the plugin runs again (%u runs)", atomic_load(&first->runs));
    CHECK(clap_host_bypassed(first) == 0, ":bypass reads 0");

    fd = mkstemp(state_file);
    CHECK(fd >= 0, "state file %s", state_file);
    if (fd >= 0)
        close(fd);
    CHECK(clap_host_state_save(first, state_file) == SUCCESS, "state_save");

    CHECK(clap_host_open(path, PLUGIN_ID, &second) == SUCCESS && second != NULL, "open a second instance");
    if (second)
    {
        CHECK(clap_host_binaries_open() == 1, "one dlopen shared by two instances (%u open)", clap_host_binaries_open());
        CHECK(clap_host_activate(second, SAMPLE_RATE, BLOCK) == SUCCESS, "activate the second instance");
        CHECK(clap_host_param_get(second, PARAM_MIX, &value) == SUCCESS && value != 1.0, "second instance starts at its default (%g)", value);
        CHECK(clap_host_state_load(second, state_file) == SUCCESS, "state_load into the second instance");
        CHECK(clap_host_param_get(second, PARAM_MIX, &value) == SUCCESS && value == 1.0, "second instance param %u = %g after load", PARAM_MIX, value);
        CHECK(clap_host_param_get(second, PARAM_TIME_MS, &value) == SUCCESS && value == 7.0, "second instance param %u = %g after load", PARAM_TIME_MS, value);
    }
    unlink(state_file);

    clap_host_stop(first);
    CHECK(atomic_load(&first->run_state) == CLAP_HOST_IDLE, "stopped");
    clap_host_close(first);
    if (second)
        clap_host_close(second);
    CHECK(clap_host_binaries_open() == 0, "binary closed after the last instance");

    if (fake_path)
        fake_plugin_checks(fake_path);
    if (synth_path)
        synth_checks(synth_path);

    return report();
}
