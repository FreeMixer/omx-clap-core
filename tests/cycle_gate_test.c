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
 * cycle_gate_test: omx-clap-host's restart and relayout (effects.c clap_idle, ports_relayout) against a cycle thread
 * that never stops, as a JACK client's does not: the stereo-to-mono fake, whose right leg is a bounce block the
 * deactivate unmaps, restarted and re-described through cycle_gate.h while the cycle enters the gate, reads the active
 * flag and runs omx_clap_host_run_ports as effects.c process() does. Built with -fsanitize=thread (make
 * test-transport-tsan): a cycle reading the stage, the bounce or the flag while the control thread clears them is a
 * report, and the run halts on it.
 *
 * usage: cycle_gate_test <fake.clap>
 */

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "clap_host.h"
#include "cycle_gate.h"

#define BLOCK       64u
#define SAMPLE_RATE 48000.0
#define ROUNDS      40u
#define STEREO_MONO "org.omx-clap-host.test.stereo-mono"

static int g_failures;

#define CHECK(cond, ...)                         \
    do                                           \
    {                                            \
        if (cond)                                \
            printf("ok   ");                     \
        else                                     \
        {                                        \
            printf("FAIL ");                     \
            g_failures++;                        \
        }                                        \
        printf(__VA_ARGS__);                     \
        printf("\n");                            \
    } while (0)

struct rig {
    struct omx_clap_instance *in;
    struct omx_cycle_gate gate;
    _Atomic int stop;
    _Atomic uint32_t cycles;
    _Atomic uint32_t nonfinite;
};

/* effects.c process(), its ports two in and one out: the gate entered, the flag read, the stage run or the lane dry */
static void *cycles_run(void *arg)
{
    struct rig *r = arg;
    float in_l[BLOCK], in_r[BLOCK], out[BLOCK];
    const float *ins[2] = { in_l, in_r };
    float *outs[1] = { out };
    uint32_t i;

    for (i = 0; i < BLOCK; i++)
        in_l[i] = 0.25f, in_r[i] = -0.125f;
    while (!atomic_load(&r->stop))
    {
        if (omx_cycle_gate_enter(&r->gate) && __atomic_load_n(&r->in->active, __ATOMIC_ACQUIRE))
            omx_clap_host_run_ports(r->in, ins, outs, BLOCK);
        else
            memcpy(out, in_l, sizeof(out));
        omx_cycle_gate_leave(&r->gate);
        for (i = 0; i < BLOCK; i++)
            if (!isfinite(out[i]))
            {
                atomic_fetch_add(&r->nonfinite, 1u);
                break;
            }
        atomic_fetch_add(&r->cycles, 1u);
        usleep(20);
    }
    return NULL;
}

/* wait until the cycle thread ran `n` more cycles */
static void cycles_wait(struct rig *r, uint32_t n)
{
    const uint32_t from = atomic_load(&r->cycles);
    unsigned waited = 0;

    while (atomic_load(&r->cycles) - from < n && waited < 2000000u)
    {
        usleep(100);
        waited += 100;
    }
}

/* ports_relayout's body: the cycles shut out, the description read again, activated with the cycle's audio role, armed */
static int relayout(struct rig *r, pthread_t thread)
{
    char why[OMX_CLAP_WHY_MAX] = "";
    int ok;

    if (omx_cycle_gate_deactivate(&r->gate, r->in, CLAP_HOST_ROLE_POLL_US, CLAP_HOST_ROLE_TIMEOUT_US) != 0)
        return 0;
    ok = omx_clap_host_request_layout(r->in, NULL, 0, why) >= 0 && omx_clap_host_activate(r->in, SAMPLE_RATE, BLOCK, why) == 0;
    if (ok)
    {
        omx_clap_host_set_audio_thread(r->in, thread);
        omx_clap_host_arm(r->in);
    }
    omx_cycle_gate_open(&r->gate);
    return ok;
}

/* one arm: `knob` set for the fake (NULL none), ROUNDS restarts, each followed by a relayout when it answered
 * OMX_CLAP_RESTART_RESCANNED and by a relayout of the active instance otherwise, the cycle thread running throughout */
static void arm(const char *path, const char *knob, int want_rescanned)
{
    struct rig r;
    char why[OMX_CLAP_WHY_MAX] = "";
    pthread_t t;
    uint32_t i, restarted = 0, rescanned = 0, failed = 0, relaid = 0, runs;

    memset(&r, 0, sizeof(r));
    if (knob)
        setenv(knob, "1", 1);
    CHECK(omx_clap_host_open(path, STEREO_MONO, &r.in, why) == 0 && r.in, "%s opens (%s)", STEREO_MONO, knob ? knob : "no knob");
    if (knob)
        unsetenv(knob);
    if (!r.in)
        return;
    CHECK(omx_clap_host_activate(r.in, SAMPLE_RATE, BLOCK, why) == 0, "it activates (%s)", why);
    (void)omx_clap_host_ports_rescanned(r.in);
    if (pthread_create(&t, NULL, cycles_run, &r) != 0)
    {
        CHECK(0, "the cycle thread starts");
        omx_clap_host_close(r.in);
        return;
    }
    omx_clap_host_publish(r.in, t);
    cycles_wait(&r, 8);
    for (i = 0; i < ROUNDS; i++)
    {
        const int ret = omx_cycle_gate_restart(&r.gate, r.in, CLAP_HOST_ROLE_POLL_US, CLAP_HOST_ROLE_TIMEOUT_US);

        if (ret == 0)
            restarted++;
        else if (ret == OMX_CLAP_RESTART_RESCANNED)
            rescanned++;
        else
            failed++;
        cycles_wait(&r, 2);
        (void)omx_clap_host_ports_rescanned(r.in);
        relaid += relayout(&r, t);
        cycles_wait(&r, 2);
    }
    runs = atomic_load(&r.in->stage.h.runs);
    cycles_wait(&r, 8);
    CHECK(failed == 0 && (want_rescanned ? rescanned == ROUNDS : restarted == ROUNDS),
          "%u restarts under running cycles: %u restarted, %u stopped by a rescan, %u failed", ROUNDS, restarted, rescanned, failed);
    CHECK(relaid == ROUNDS && r.in->active, "and %u relayouts under them, each armed again (%u)", ROUNDS, relaid);
    CHECK(atomic_load(&r.in->stage.h.runs) > runs, "the plugin runs on the cycle thread after them (%u, then %u)", runs,
          atomic_load(&r.in->stage.h.runs));
    CHECK(atomic_load(&r.nonfinite) == 0, "no cycle wrote a non-finite sample (%u cycles)", atomic_load(&r.cycles));
    atomic_store(&r.stop, 1);
    pthread_join(t, NULL);
    omx_clap_host_stop(r.in);
    omx_clap_host_close(r.in);
}

int main(int argc, char **argv)
{
    struct omx_clap_host_config config;

    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <fake.clap>\n", argv[0]);
        return 2;
    }
    // as omx-clap-host configures the core: a JACK port per main channel
    omx_clap_host_config_default(&config);
    config.lane_channels = 0;
    if (omx_clap_host_configure(&config) != 0)
    {
        fprintf(stderr, "the core refuses the configuration\n");
        return 1;
    }
    arm(argv[1], NULL, 0);
    arm(argv[1], "FAKE_RESCAN_ON_DEACTIVATE", 1);
    if (g_failures)
    {
        printf("FAILED\n");
        return 1;
    }
    printf("cycle gate test ok\n");
    return 0;
}
