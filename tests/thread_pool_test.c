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
 * The thread pool (#395), through libomx-clap-core against tests/fake_pool.clap, whose process() asks for its tasks
 * and records the thread that ran each:
 *  - an instance never granted the pool runs every task on the audio thread, every one done before request_exec
 *    returns;
 *  - granted it (omx_clap_host_thread_pool_use), its tasks spread across the helpers and the audio thread, every one
 *    done before request_exec returns, each task on the audio role; the pool has one helper per core of the affinity
 *    mask but one, and ends with the last deactivate;
 *  - a request from inside a task, on a helper or on the audio thread, is refused;
 *  - a helper's task that runs past the block is waited for and counted late;
 *  - an affinity mask of one core gives no pool, with the log line that says so;
 *  - SCHED_FIFO: where this process may set it, the helpers run at the priority given; where it may not, the instance
 *    runs serial with the log line that names the refusal;
 *  - granted and then withdrawn, the instance runs serial again.
 *   thread_pool_test <fake_pool.clap>
 */

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>

#include "../src/clap_host.h"
#include "test_util.h"

#define POOL_ID     "org.omx-clap-host.test.pool"
#define BLK         256u
#define RATE        48000.0
#define TASKS_MAX   256u

typedef struct {
    uint32_t tasks, voices, spin_us, nested, probe_sched, spin_other_us;
} pool_config_t;

typedef struct {
    uint32_t process_calls, requests, refused, incomplete, last_threads, max_threads;
    uint64_t tasks_audio, tasks_other, not_audio_role;
    uint32_t nested_ok, nested_refused;
    int32_t other_policy, other_priority;
    uint32_t pad;
    uint64_t task_thread[TASKS_MAX];
} pool_stats_t;

static float g_l[BLK], g_r[BLK];
static pool_config_t *g_cfg;
static pool_stats_t *g_st;

static void *hook(const char *path, const char *name)
{
    void *file = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    void *h = file ? dlsym(file, name) : NULL;

    if (file)
        dlclose(file);
    return h;
}

static uint32_t cores(void)
{
    cpu_set_t set;

    CPU_ZERO(&set);
    return sched_getaffinity(0, sizeof(set), &set) == 0 ? (uint32_t)CPU_COUNT(&set) : 1u;
}

/* activate, the log line of the activate taken, published on this thread and the fade-in cycle run */
static int start(struct omx_clap_instance *in, char *log, size_t size)
{
    char why[OMX_CLAP_WHY_MAX];
    const char *line;

    if (omx_clap_host_activate(in, RATE, BLK, why) != 0)
    {
        printf("     activate: %s\n", why);
        return -1;
    }
    omx_clap_host_tick(in);
    line = omx_clap_host_log_take(in);
    snprintf(log, size, "%s", line ? line : "");
    omx_clap_host_publish(in, pthread_self());
    omx_clap_host_run(in, g_l, g_r, BLK);
    return 0;
}

static void finish(struct omx_clap_instance *in)
{
    omx_clap_request_stop(&in->stage);
    omx_clap_host_run(in, g_l, g_r, BLK);
    omx_clap_host_unpublish(in, 10, 1000000);
    omx_clap_host_deactivate(in);
}

/* `blocks` blocks of `tasks` tasks each, the counts started over */
static void blocks(struct omx_clap_instance *in, uint32_t n, uint32_t tasks, uint32_t spin_us)
{
    g_st->requests = g_st->refused = g_st->incomplete = g_st->max_threads = 0;
    g_st->tasks_audio = g_st->tasks_other = g_st->not_audio_role = 0;
    g_cfg->tasks = tasks;
    g_cfg->spin_us = spin_us;
    for (uint32_t i = 0; i < n; i++)
        omx_clap_host_run(in, g_l, g_r, BLK);
    g_cfg->tasks = 0;
}

/* whether this process may make a SCHED_FIFO thread at `priority` */
static void *nothing(void *arg) { return arg; }
static int fifo_allowed(int priority)
{
    struct sched_param p = { .sched_priority = priority };
    pthread_attr_t attr;
    pthread_t t;
    int ok;

    pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    pthread_attr_setschedparam(&attr, &p);
    ok = pthread_create(&t, &attr, nothing, NULL) == 0;
    if (ok)
        pthread_join(t, NULL);
    pthread_attr_destroy(&attr);
    return ok;
}

int main(int argc, char **argv)
{
    struct omx_clap_instance *in = NULL;
    struct omx_clap_thread_pool_info info;
    char why[OMX_CLAP_WHY_MAX], log[CLAP_HOST_LOG_BYTES];
    const uint32_t n = cores();
    const uint32_t helpers = n > 1u ? (n - 1u < OMX_CLAP_THREAD_POOL_HELPERS_MAX ? n - 1u : OMX_CLAP_THREAD_POOL_HELPERS_MAX) : 0u;

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <fake_pool.clap>\n", argv[0]);
        return 2;
    }
    CHECK(omx_clap_host_open(argv[1], POOL_ID, &in, why) == 0, "fake_pool opens (%s)", why);
    if (!in)
        return 1;
    {
        pool_config_t *(*config)(void) = (pool_config_t *(*)(void))hook(argv[1], "fake_pool_config");
        pool_stats_t *(*stats)(void) = (pool_stats_t *(*)(void))hook(argv[1], "fake_pool_stats");

        g_cfg = config ? config() : NULL;
        g_st = stats ? stats() : NULL;
    }
    CHECK(g_cfg && g_st, "its hooks are found");
    if (!g_cfg || !g_st)
        return 1;
    printf("     %u cores in the affinity mask, %u helpers expected\n", n, helpers);

    /* never granted: serial on the audio thread */
    CHECK(start(in, log, sizeof log) == 0, "activated, never granted the pool");
    omx_clap_host_thread_pool(&info);
    CHECK(info.helpers == 0 && info.holders == 0, "no pool is made for it (%u helpers, %u holders)", info.helpers, info.holders);
    blocks(in, 20, 16, 20);
    CHECK(g_st->requests == 20 && g_st->incomplete == 0, "20 requests, every task done before request_exec returned (%u, %u incomplete)",
          g_st->requests, g_st->incomplete);
    CHECK(g_st->tasks_audio == 320 && g_st->tasks_other == 0 && g_st->max_threads == 1,
          "every task ran on the audio thread (%llu on it, %llu off it, %u threads)", (unsigned long long)g_st->tasks_audio,
          (unsigned long long)g_st->tasks_other, g_st->max_threads);
    CHECK(g_st->not_audio_role == 0, "every task is on the audio role");
    CHECK(omx_clap_host_thread_pool_use(in, 1, 0) == -1, "the gate is refused while the instance is active");
    finish(in);

    CHECK(omx_clap_host_thread_pool_use(in, 1, -1) == -1 && omx_clap_host_thread_pool_use(in, 1, 100) == -1,
          "a priority outside 0..99 is refused");

    /* granted, the normal class (the audio thread here is not real-time) */
    CHECK(omx_clap_host_thread_pool_use(in, 1, 0) == 0, "granted the pool at the normal class");
    CHECK(start(in, log, sizeof log) == 0, "activated with the pool (%s)", log[0] ? log : "no log line");
    omx_clap_host_thread_pool(&info);
    CHECK(info.helpers == helpers && info.holders == (helpers ? 1u : 0u) && info.cores == n && info.priority == 0,
          "the pool has %u helpers for %u cores (%u helpers, %u holders, %u cores, priority %d)", helpers, n, info.helpers,
          info.holders, info.cores, info.priority);
    if (helpers)
    {
        uint64_t sum = 0;
        uint32_t pinned = 1;

        for (uint32_t h = 0; h < info.helpers; h++)
        {
            pinned &= info.helper_core[h] >= 0;
            for (uint32_t k = 0; k < h; k++)
                pinned &= info.helper_core[k] != info.helper_core[h];
        }
        CHECK(pinned, "each helper is pinned to a core of its own");
        blocks(in, 40, 32, 200);
        CHECK(g_st->requests == 40 && g_st->refused == 0 && g_st->incomplete == 0,
              "40 requests, every task done before request_exec returned (%u, %u refused, %u incomplete)", g_st->requests,
              g_st->refused, g_st->incomplete);
        CHECK(g_st->tasks_other > 0 && g_st->max_threads >= 2,
              "the tasks spread across threads (%llu on the audio thread, %llu on helpers, at most %u threads in one request)",
              (unsigned long long)g_st->tasks_audio, (unsigned long long)g_st->tasks_other, g_st->max_threads);
        CHECK(g_st->tasks_audio + g_st->tasks_other == 40u * 32u, "each task ran once (%llu)",
              (unsigned long long)(g_st->tasks_audio + g_st->tasks_other));
        CHECK(g_st->not_audio_role == 0, "a helper inside a task is on the audio role of the instance");
        omx_clap_host_thread_pool(&info);
        for (uint32_t h = 0; h < info.helpers; h++)
            sum += info.helper_tasks[h];
        CHECK(info.requests >= 40 && sum + info.tasks_audio >= 40u * 32u && sum == g_st->tasks_other,
              "the pool counts what ran where (%llu requests, %llu on helpers, %llu on the audio thread)",
              (unsigned long long)info.requests, (unsigned long long)sum, (unsigned long long)info.tasks_audio);

        /* one task: no hand-out */
        blocks(in, 4, 1, 0);
        CHECK(g_st->tasks_other == 0 && g_st->tasks_audio == 4, "a request of one task runs on the audio thread");

        /* nested, from the audio thread and from the helpers */
        g_cfg->nested = 1;
        g_st->nested_ok = g_st->nested_refused = 0;
        blocks(in, 4, 16, 50);
        CHECK(g_st->nested_ok == 0 && g_st->nested_refused == 64 && g_st->incomplete == 0,
              "a request from inside a task is refused on every thread (%u ran, %u refused)", g_st->nested_ok, g_st->nested_refused);
        g_cfg->nested = 0;

        /* a helper's task past the block: waited for, counted late */
        {
            const uint64_t late = info.late;
            const uint32_t late_in = omx_clap_host_thread_pool_late(in);

            g_cfg->spin_other_us = 8000;
            blocks(in, 3, helpers + 1u, 1000);
            g_cfg->spin_other_us = 0;
            omx_clap_host_thread_pool(&info);
            CHECK(g_st->incomplete == 0 && g_st->tasks_other > 0,
                  "helper tasks of 9 ms in a block of 5.3 ms are still all done before request_exec returns (%llu on helpers)",
                  (unsigned long long)g_st->tasks_other);
            CHECK(info.late >= late + 3u && omx_clap_host_thread_pool_late(in) >= late_in + 3u,
                  "and each of the 3 waits past the block is counted late (%llu in the pool, %u for the instance)",
                  (unsigned long long)(info.late - late), omx_clap_host_thread_pool_late(in) - late_in);
        }
    }
    else
        printf("skip one core: the spread needs two\n");
    finish(in);
    omx_clap_host_thread_pool(&info);
    CHECK(info.helpers == 0 && info.holders == 0, "the last deactivate ends the pool (%u helpers)", info.helpers);

    /* a mask of one core: serial, said */
    {
        cpu_set_t all, one;

        CPU_ZERO(&all);
        sched_getaffinity(0, sizeof(all), &all);
        CPU_ZERO(&one);
        for (int c = 0; c < CPU_SETSIZE; c++)
            if (CPU_ISSET(c, &all))
            {
                CPU_SET(c, &one);
                break;
            }
        sched_setaffinity(0, sizeof(one), &one);
        CHECK(start(in, log, sizeof log) == 0 && strstr(log, "thread pool: serial: one core") != NULL,
              "an affinity mask of one core gives no pool, and says so (%s)", log);
        blocks(in, 4, 16, 0);
        CHECK(g_st->tasks_other == 0 && g_st->incomplete == 0, "its tasks run on the audio thread");
        finish(in);
        sched_setaffinity(0, sizeof(all), &all);
    }

    /* SCHED_FIFO */
    if (helpers)
    {
        const int fifo = fifo_allowed(1);

        CHECK(omx_clap_host_thread_pool_use(in, 1, 1) == 0, "granted the pool at SCHED_FIFO 1");
        g_cfg->probe_sched = 1;
        CHECK(start(in, log, sizeof log) == 0, "activated (%s)", log[0] ? log : "no log line");
        blocks(in, 20, 32, 200);
        if (fifo)
            CHECK(g_st->tasks_other > 0 && g_st->other_policy == SCHED_FIFO && g_st->other_priority == 1,
                  "this process may set SCHED_FIFO: the helpers run at SCHED_FIFO 1 (policy %d, priority %d)", g_st->other_policy,
                  g_st->other_priority);
        else
            CHECK(g_st->tasks_other == 0 && strstr(log, "thread pool: serial: SCHED_FIFO 1 refused") != NULL,
                  "this process may not set SCHED_FIFO: serial, and the log line names it (%s)", log);
        CHECK(g_st->incomplete == 0, "every task done before request_exec returned");
        g_cfg->probe_sched = 0;
        finish(in);
    }

    /* withdrawn */
    CHECK(omx_clap_host_thread_pool_use(in, 0, 0) == 0, "the grant withdrawn");
    CHECK(start(in, log, sizeof log) == 0, "activated again");
    blocks(in, 8, 16, 20);
    omx_clap_host_thread_pool(&info);
    CHECK(info.helpers == 0 && g_st->tasks_other == 0 && g_st->tasks_audio == 128, "serial again, no pool made");
    finish(in);

    omx_clap_host_close(in);
    printf("%s: %d failure(s)\n", argv[0], g_failures);
    return g_failures ? 1 : 0;
}
