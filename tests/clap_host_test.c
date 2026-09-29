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
 * the reason on stderr, and whose passthrough reports a latency. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static void fake_plugin_checks(const char *path)
{
    clap_instance_t *instance = NULL;
    double value;

    CHECK(CLAP_HOST_MAX_CHANNELS == 2, "CLAP_HOST_MAX_CHANNELS is %i", CLAP_HOST_MAX_CHANNELS);
    check_refused(path, FAKE_WIDE, "main port has 4 channels");
    check_refused(path, FAKE_SIDECHAIN, "1 sidechain/aux ports");
    check_refused(path, FAKE_NOTES, "note input");
    CHECK(clap_host_binaries_open() == 0, "the fake binary is closed after the refusals (%u open)", clap_host_binaries_open());

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

static int report(void)
{
    printf("%s\n", g_failures == 0 ? "clap host test ok" : "clap host test FAILED");
    return g_failures == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : NULL;
    const char *fake_path = argc > 2 ? argv[2] : NULL;
    clap_instance_t *first = NULL, *second = NULL;
    float in_l[BLOCK], in_r[BLOCK], out_l[BLOCK], out_r[BLOCK];
    const float *inputs[2] = { in_l, in_r };
    float *outputs[2] = { out_l, out_r };
    float max_diff = 0.0f;
    int finite = 1;
    uint32_t b, i;
    double value;
    char state_file[] = "/tmp/clap_host_test_XXXXXX";
    int fd;

    if (!path)
    {
        fprintf(stderr, "usage: %s <omx-delay.clap | -> [fake.clap]\n", argv[0]);
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
    CHECK(first->bypass_param == PARAM_BYPASS, "bypass parameter is id %u", first->bypass_param);

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
    CHECK(clap_host_param_get(first, PARAM_TIME_MS, &value) == SUCCESS && value == 7.0, "param_get %u = %g with no cycle running", PARAM_TIME_MS, value);
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

    return report();
}
