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
 * What one block of a plugin that renders its voices as clap.thread-pool tasks costs, pool off against pool on (#395):
 * a measurement, not a test. This thread is the audio thread: SCHED_FIFO at <priority> (0 the normal class) and pinned
 * to the first core of the affinity mask, the one the pool leaves to it. It runs tests/fake_pool.clap block after block,
 * each started at its period (frames / rate, on CLOCK_MONOTONIC, so the helpers park between blocks as they do under
 * JACK), and times omx_clap_host_run. One line per block to <out> (block, ns), then one summary line to stdout:
 *   mode frames tasks voices blocks budget_us mean_us p50_us p99_us max_us over late busy_pct per helper ...
 * busy_pct is a helper's time inside tasks over the run's wall time.
 *   thread_pool_bench <fake_pool.clap> <rate> <frames> <tasks> <voices> <blocks> off|on <priority> <out>
 */

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/clap_host.h"

#define POOL_ID "org.omx-clap-host.test.pool"

typedef struct {
    uint32_t tasks, voices, spin_us, nested, probe_sched, spin_other_us;
} pool_config_t;

static uint64_t ns(const struct timespec *t)
{
    return (uint64_t)t->tv_sec * 1000000000u + (uint64_t)t->tv_nsec;
}

static int by_value(const void *a, const void *b)
{
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    struct omx_clap_instance *in = NULL;
    struct omx_clap_thread_pool_info info;
    struct timespec next, a, b, t0, t1;
    pool_config_t *(*config)(void);
    char why[OMX_CLAP_WHY_MAX];
    double rate, sum = 0.0;
    uint32_t frames, tasks, voices, blocks, over = 0, i;
    uint64_t period, *cost, *sorted;
    float *l, *r;
    int on, priority;
    cpu_set_t mask, one;
    FILE *out;
    void *file;

    if (argc != 10)
    {
        fprintf(stderr, "usage: %s <fake_pool.clap> <rate> <frames> <tasks> <voices> <blocks> off|on <priority> <out>\n", argv[0]);
        return 2;
    }
    rate = atof(argv[2]);
    frames = (uint32_t)atoi(argv[3]);
    tasks = (uint32_t)atoi(argv[4]);
    voices = (uint32_t)atoi(argv[5]);
    blocks = (uint32_t)atoi(argv[6]);
    on = !strcmp(argv[7], "on");
    priority = atoi(argv[8]);
    period = (uint64_t)((double)frames * 1e9 / rate);
    cost = calloc(blocks, sizeof(*cost));
    sorted = calloc(blocks, sizeof(*sorted));
    l = calloc(frames, sizeof(float));
    r = calloc(frames, sizeof(float));
    out = fopen(argv[9], "w");
    if (!cost || !sorted || !l || !r || !out || frames == 0 || blocks == 0)
        return 2;

    // the audio thread: the mask's first core, the one the pool leaves to it, at the priority given
    CPU_ZERO(&mask);
    sched_getaffinity(0, sizeof(mask), &mask);
    CPU_ZERO(&one);
    for (int c = 0; c < CPU_SETSIZE; c++)
        if (CPU_ISSET(c, &mask))
        {
            CPU_SET(c, &one);
            break;
        }
    if (priority > 0)
    {
        struct sched_param p = { .sched_priority = priority };

        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &p) != 0)
        {
            fprintf(stderr, "SCHED_FIFO %d refused\n", priority);
            return 1;
        }
    }

    if (omx_clap_host_open(argv[1], POOL_ID, &in, why) != 0)
    {
        fprintf(stderr, "open: %s\n", why);
        return 1;
    }
    file = dlopen(argv[1], RTLD_NOW | RTLD_NOLOAD);
    config = file ? (pool_config_t *(*)(void))dlsym(file, "fake_pool_config") : NULL;
    if (!config)
        return 1;
    config()->tasks = tasks;
    config()->voices = voices;
    if (on)
        omx_clap_host_thread_pool_use(in, 1, priority);
    // the pool reads the whole mask at activate; the audio thread is moved to its core after
    if (omx_clap_host_activate(in, rate, frames, why) != 0)
    {
        fprintf(stderr, "activate: %s\n", why);
        return 1;
    }
    sched_setaffinity(0, sizeof(one), &one);
    omx_clap_host_tick(in);
    {
        const char *line = omx_clap_host_log_take(in);

        if (line)
            fprintf(stderr, "%s\n", line);
    }
    omx_clap_host_publish(in, pthread_self());
    for (i = 0; i < 200; i++)
        omx_clap_host_run(in, l, r, frames);

    clock_gettime(CLOCK_MONOTONIC, &next);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    omx_clap_host_thread_pool(&info);
    {
        uint64_t busy0[OMX_CLAP_THREAD_POOL_HELPERS_MAX], late0 = info.late;

        for (uint32_t h = 0; h < OMX_CLAP_THREAD_POOL_HELPERS_MAX; h++)
            busy0[h] = info.helper_busy_ns[h];
        for (i = 0; i < blocks; i++)
        {
            uint64_t t = ns(&next) + period;

            next.tv_sec = (time_t)(t / 1000000000u);
            next.tv_nsec = (long)(t % 1000000000u);
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
            clock_gettime(CLOCK_MONOTONIC, &a);
            omx_clap_host_run(in, l, r, frames);
            clock_gettime(CLOCK_MONOTONIC, &b);
            cost[i] = ns(&b) - ns(&a);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        omx_clap_host_thread_pool(&info);
        for (i = 0; i < blocks; i++)
        {
            fprintf(out, "%u %llu\n", i, (unsigned long long)cost[i]);
            sum += (double)cost[i];
            over += cost[i] > period;
            sorted[i] = cost[i];
        }
        qsort(sorted, blocks, sizeof(*sorted), by_value);
        printf("%s %u %u %u %u %.1f %.1f %.1f %.1f %.1f %u %llu", on ? "on" : "off", frames, tasks, voices, blocks, period / 1e3,
               sum / blocks / 1e3, sorted[blocks / 2] / 1e3, sorted[(uint64_t)blocks * 99u / 100u] / 1e3, sorted[blocks - 1] / 1e3,
               over, (unsigned long long)(info.late - late0));
        for (uint32_t h = 0; h < info.helpers; h++)
            printf(" %.1f", 100.0 * (double)(info.helper_busy_ns[h] - busy0[h]) / (double)(ns(&t1) - ns(&t0)));
        printf("\n");
    }
    fclose(out);
    omx_clap_request_stop(&in->stage);
    omx_clap_host_run(in, l, r, frames);
    omx_clap_host_unpublish(in, 10, 1000000);
    omx_clap_host_close(in);
    return 0;
}
