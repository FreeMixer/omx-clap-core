/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * state.load is a main-thread call, and a plugin may answer false while it processes, as most of omx-plugins do: the
 * host loads a state, or a preset, with the instance out of processing and runs it again after (tests/fake_state.clap).
 *
 *   clap_state_busy_test <fake_state.clap>
 */

#include <pthread.h>
#include <time.h>

#include "../src/clap_host.c"
#include "../src/idle_hold.c"
#include "../src/thread_pool.c"

#define FAKE_STATE "org.omx-clap-host.test.state"
#define FRAMES 64u
#define LOADS 200

static int g_failures;

#define CHECK(cond, ...) \
    do { \
        if (cond) { printf("ok   " __VA_ARGS__); printf("\n"); } \
        else { printf("FAIL " __VA_ARGS__); printf("\n"); g_failures++; } \
    } while (0)

static float g_in[2][FRAMES], g_out[2][FRAMES];

static void cycle(struct omx_clap_instance *in)
{
    const float *inputs[2] = { g_in[0], g_in[1] };
    float *outputs[2] = { g_out[0], g_out[1] };

    omx_clap_run_io(&in->stage, inputs, outputs, FRAMES);
}

static double value_of(struct omx_clap_instance *in)
{
    double value = -1.0;

    in->params->get_value(in->plugin, 0, &value);
    return value;
}

static _Atomic int g_cycles_done;

static void *audio(void *arg)
{
    const struct timespec step = { 0, 200000 };

    while (!atomic_load(&g_cycles_done))
    {
        cycle(arg);
        nanosleep(&step, NULL);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    struct omx_clap_instance *in = NULL;
    char why[OMX_CLAP_WHY_MAX];
    const double saved = 7.5, other = 2.5;
    pthread_t thread;
    int i, refused = 0;

    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <fake_state.clap>\n", argv[0]);
        return 2;
    }
    CHECK(omx_clap_host_open(argv[1], FAKE_STATE, &in, why) == 0 && in, "open %s", FAKE_STATE);
    if (!in)
        return 1;

    CHECK(omx_clap_host_state_load(in, &other, sizeof(other)) == 0 && value_of(in) == other, "not yet active, it loads");
    CHECK(omx_clap_host_activate(in, 48000.0, FRAMES, why) == 0, "activate");
    CHECK(omx_clap_host_state_load(in, &saved, sizeof(saved)) == 0 && value_of(in) == saved && atomic_load(&in->stage.state) == OMX_CLAP_IDLE,
          "active, not armed, it loads and is left as it was");
    omx_clap_arm(&in->stage);
    CHECK(omx_clap_host_state_load(in, &other, sizeof(other)) == 0 && value_of(in) == other && atomic_load(&in->stage.state) == OMX_CLAP_ARMED,
          "armed, it loads and stays armed");
    cycle(in);
    CHECK(atomic_load(&in->stage.state) == OMX_CLAP_PROCESSING, "a cycle runs it");

    // no cycle comes: the control thread stands in for it, and the next cycle runs the plugin again
    CHECK(omx_clap_host_state_load(in, &saved, sizeof(saved)) == 0 && value_of(in) == saved, "processing, no cycle coming, it loads");
    CHECK(atomic_load(&in->stage.state) == OMX_CLAP_ARMED, "and it is armed again");
    cycle(in);
    CHECK(atomic_load(&in->stage.state) == OMX_CLAP_PROCESSING, "and the next cycle runs it");

    // a state the plugin refuses for what it holds answers so, and the plugin runs on
    CHECK(omx_clap_host_state_load(in, &saved, 1) == -1, "a state of the wrong size is refused");
    cycle(in);
    CHECK(atomic_load(&in->stage.state) == OMX_CLAP_PROCESSING && value_of(in) == saved, "and the plugin runs on with what it had");

    // a preset's load is the same call on the same thread
    CHECK(omx_clap_host_preset_load_key(in, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, NULL, "any") == 0 && value_of(in) == 9.0, "processing, a preset loads");
    CHECK(atomic_load(&in->stage.state) == OMX_CLAP_ARMED, "and it is armed again");
    cycle(in);
    CHECK(atomic_load(&in->stage.state) == OMX_CLAP_PROCESSING, "and the next cycle runs it");

    // cycles coming all along: every load reaches the plugin
    pthread_create(&thread, NULL, audio, in);
    for (i = 0; i < LOADS; i++)
    {
        const double v = i;

        if (omx_clap_host_state_load(in, &v, sizeof(v)) != 0 || value_of(in) != v)
            refused++;
    }
    atomic_store(&g_cycles_done, 1);
    pthread_join(thread, NULL);
    CHECK(refused == 0, "%d loads with the cycles running: %d refused", LOADS, refused);
    cycle(in);
    CHECK(atomic_load(&in->stage.state) == OMX_CLAP_PROCESSING, "and it is processing after them");

    omx_clap_host_stop(in);
    omx_clap_host_close(in);
    printf("%s\n", g_failures ? "state busy: FAILED" : "state busy: ok");
    return g_failures ? 1 : 0;
}
