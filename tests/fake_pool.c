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
 * A plugin built for the tests and the bench of the host's thread pool (#395): org.omx-clap-host.test.pool, a stereo
 * source that renders its voices as clap.thread-pool tasks. Inside process() it asks the host for `tasks` tasks; each
 * task
 *  - records the thread that ran it, and whether the host's is_audio_thread answered true there;
 *  - renders `voices` sine voices over the block into a buffer of its own, allocated at activate, and spins for
 *    `spin_us` microseconds besides (a load the tests can size without a block size behind it), `spin_other_us` more
 *    off the audio thread (a helper slower than the audio thread);
 *  - with `nested`, asks the pool again from inside the task, which a host refuses;
 *  - with `probe_sched`, records the scheduling policy and priority it ran at when off the audio thread.
 * After request_exec returns, process() counts the tasks done: a request that returned before its last task ended is
 * `incomplete`. The output is the sum of the task buffers, so the work cannot be optimised away. Everything is in the
 * struct fake_pool_stats returns, the load in the one fake_pool_config returns, for a program that dlsyms them; a host
 * started by a script takes the load from FAKE_POOL_TASKS and FAKE_POOL_VOICES at init.
 */

#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <clap/clap.h>

#define ID_POOL             "org.omx-clap-host.test.pool"
#define FAKE_POOL_TASKS_MAX 256u

typedef struct FAKE_POOL_CONFIG_T {
    uint32_t tasks;         ///< the tasks process() asks for, 0 for none (the program sets it)
    uint32_t voices;        ///< the sine voices each task renders over the block
    uint32_t spin_us;       ///< the microseconds each task spins besides
    uint32_t nested;        ///< 1: each task asks the pool again from inside exec
    uint32_t probe_sched;   ///< 1: a task off the audio thread records its policy and priority
    uint32_t spin_other_us; ///< the microseconds a task off the audio thread spins besides `spin_us`
} fake_pool_config_t;

typedef struct FAKE_POOL_STATS_T {
    uint32_t process_calls;     ///< process() calls
    uint32_t requests;          ///< request_exec calls that answered true
    uint32_t refused;           ///< request_exec calls that answered false (the plugin then ran its tasks itself)
    uint32_t incomplete;        ///< requests that returned before every task had ended
    uint32_t last_threads;      ///< the distinct threads that ran the last request's tasks
    uint32_t max_threads;       ///< the most distinct threads of any request
    uint64_t tasks_audio;       ///< tasks run on the thread that called process()
    uint64_t tasks_other;       ///< tasks run on any other thread
    uint64_t not_audio_role;    ///< tasks whose is_audio_thread answered false
    uint32_t nested_ok;         ///< nested requests the host ran (it must refuse them: 0)
    uint32_t nested_refused;    ///< nested requests it refused
    int32_t other_policy;       ///< the policy a task off the audio thread ran at, -1 before any
    int32_t other_priority;     ///< and its priority
    uint32_t pad;
    uint64_t task_thread[FAKE_POOL_TASKS_MAX];  ///< the last request's thread per task, as pthread_self
} fake_pool_stats_t;

static fake_pool_config_t g_cfg;
static fake_pool_stats_t g_stats;
static const clap_host_t *g_host;
static const clap_plugin_t *g_plugin;
static const clap_host_thread_check_t *g_check;
static float *g_buf;                ///< FAKE_POOL_TASKS_MAX buffers of g_max frames, made at activate
static uint32_t g_max;              ///< the largest block, from activate
static uint32_t g_frames;           ///< this block's frames, written before request_exec
static double g_phase_step;         ///< the first voice's phase step at the rate
static uint32_t g_task_done;        ///< the tasks ended in this request, counted with __atomic
static pthread_t g_audio;           ///< the thread inside process()

CLAP_EXPORT fake_pool_stats_t *fake_pool_stats(void)
{
    return &g_stats;
}

CLAP_EXPORT fake_pool_config_t *fake_pool_config(void)
{
    return &g_cfg;
}

static void spin(uint32_t us)
{
    struct timespec a, b;

    if (!us)
        return;
    clock_gettime(CLOCK_MONOTONIC, &a);
    do
        clock_gettime(CLOCK_MONOTONIC, &b);
    while ((b.tv_sec - a.tv_sec) * 1000000000L + (b.tv_nsec - a.tv_nsec) < (long)us * 1000L);
}

static void pool_exec(const clap_plugin_t *plugin, uint32_t task)
{
    const pthread_t self = pthread_self();
    const int audio = pthread_equal(self, g_audio);
    float *out;

    (void)plugin;
    if (task >= FAKE_POOL_TASKS_MAX)
        return;
    g_stats.task_thread[task] = (uint64_t)self;
    if (audio)
        __atomic_fetch_add(&g_stats.tasks_audio, 1u, __ATOMIC_RELAXED);
    else
        __atomic_fetch_add(&g_stats.tasks_other, 1u, __ATOMIC_RELAXED);
    if (g_check && !g_check->is_audio_thread(g_host))
        __atomic_fetch_add(&g_stats.not_audio_role, 1u, __ATOMIC_RELAXED);
    if (!audio && g_cfg.probe_sched)
    {
        struct sched_param p;
        int policy;

        if (pthread_getschedparam(self, &policy, &p) == 0)
        {
            __atomic_store_n(&g_stats.other_policy, policy, __ATOMIC_RELAXED);
            __atomic_store_n(&g_stats.other_priority, p.sched_priority, __ATOMIC_RELAXED);
        }
    }
    out = g_buf + (size_t)task * g_max;
    for (uint32_t i = 0; i < g_frames; i++)
    {
        float s = 0.0f;

        for (uint32_t v = 0; v < g_cfg.voices; v++)
            s += sinf((float)((double)(i + task * 7u + v * 13u) * g_phase_step * (1.0 + v * 0.01)));
        out[i] = s * 0.001f;
    }
    spin(g_cfg.spin_us + (audio ? 0u : g_cfg.spin_other_us));
    if (g_cfg.nested)
    {
        const clap_host_thread_pool_t *pool = g_host->get_extension(g_host, CLAP_EXT_THREAD_POOL);

        if (pool && pool->request_exec(g_host, 1))
            __atomic_fetch_add(&g_stats.nested_ok, 1u, __ATOMIC_RELAXED);
        else
            __atomic_fetch_add(&g_stats.nested_refused, 1u, __ATOMIC_RELAXED);
    }
    __atomic_fetch_add(&g_task_done, 1u, __ATOMIC_RELEASE);
}

static uint32_t distinct(const uint64_t *t, uint32_t n)
{
    uint32_t count = 0;

    for (uint32_t i = 0; i < n; i++)
    {
        uint32_t j = 0;

        while (j < i && t[j] != t[i])
            j++;
        count += j == i;
    }
    return count;
}

static clap_process_status plugin_process(const clap_plugin_t *plugin, const clap_process_t *process)
{
    const clap_host_thread_pool_t *pool = g_host->get_extension(g_host, CLAP_EXT_THREAD_POOL);
    const uint32_t tasks = g_cfg.tasks < FAKE_POOL_TASKS_MAX ? g_cfg.tasks : FAKE_POOL_TASKS_MAX;
    uint32_t c, i, t;

    (void)plugin;
    g_stats.process_calls++;
    g_audio = pthread_self();
    g_frames = process->frames_count < g_max ? process->frames_count : g_max;
    if (tasks)
    {
        __atomic_store_n(&g_task_done, 0u, __ATOMIC_RELAXED);
        if (pool && pool->request_exec(g_host, tasks))
        {
            g_stats.requests++;
            if (__atomic_load_n(&g_task_done, __ATOMIC_ACQUIRE) != tasks)
                g_stats.incomplete++;
        }
        else
        {
            g_stats.refused++;
            for (t = 0; t < tasks; t++)
                pool_exec(plugin, t);
        }
        g_stats.last_threads = distinct(g_stats.task_thread, tasks);
        if (g_stats.last_threads > g_stats.max_threads)
            g_stats.max_threads = g_stats.last_threads;
    }
    for (c = 0; c < 2; c++)
        for (i = 0; i < process->frames_count; i++)
        {
            float s = 0.0f;

            for (t = 0; t < tasks && i < g_frames; t++)
                s += g_buf[(size_t)t * g_max + i];
            process->audio_outputs[0].data32[c][i] = s;
        }
    return CLAP_PROCESS_CONTINUE;
}

static const char *const g_features[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, NULL };
static const clap_plugin_descriptor_t g_desc = { CLAP_VERSION_INIT, ID_POOL, "pool", "omx-clap-host", "", "", "", "0", "", g_features };

static bool plugin_init(const clap_plugin_t *plugin)
{
    const char *tasks = getenv("FAKE_POOL_TASKS"), *voices = getenv("FAKE_POOL_VOICES");

    (void)plugin;
    g_check = g_host->get_extension(g_host, CLAP_EXT_THREAD_CHECK);
    // a host started by a script sets the load through the environment
    if (tasks)
        g_cfg.tasks = (uint32_t)atoi(tasks);
    if (voices)
        g_cfg.voices = (uint32_t)atoi(voices);
    return true;
}

static void plugin_destroy(const clap_plugin_t *plugin)
{
    (void)plugin;
    free(g_buf);
    g_buf = NULL;
    g_host = NULL;
    g_plugin = NULL;
}

static bool plugin_activate(const clap_plugin_t *plugin, double rate, uint32_t min_frames, uint32_t max_frames)
{
    (void)plugin;
    (void)min_frames;
    free(g_buf);
    g_max = max_frames;
    g_buf = calloc((size_t)FAKE_POOL_TASKS_MAX * max_frames, sizeof(float));
    g_phase_step = 2.0 * 3.14159265358979 * 220.0 / rate;
    return g_buf != NULL;
}

static void plugin_deactivate(const clap_plugin_t *plugin) { (void)plugin; }
static bool plugin_start_processing(const clap_plugin_t *plugin) { (void)plugin; return true; }
static void plugin_stop_processing(const clap_plugin_t *plugin) { (void)plugin; }
static void plugin_reset(const clap_plugin_t *plugin) { (void)plugin; }

static uint32_t ports_count(const clap_plugin_t *plugin, bool is_input) { (void)plugin; return is_input ? 0 : 1; }

static bool ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_audio_port_info_t *info)
{
    (void)plugin;
    if (is_input || index != 0)
        return false;
    memset(info, 0, sizeof(*info));
    info->id = 0;
    strcpy(info->name, "out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t g_ports = { ports_count, ports_get };
static const clap_plugin_thread_pool_t g_pool = { pool_exec };

static const void *plugin_get_extension(const clap_plugin_t *plugin, const char *id)
{
    (void)plugin;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &g_ports;
    if (!strcmp(id, CLAP_EXT_THREAD_POOL))
        return &g_pool;
    return NULL;
}

static void plugin_on_main_thread(const clap_plugin_t *plugin) { (void)plugin; }

static clap_plugin_t g_the_plugin = { &g_desc, NULL, plugin_init, plugin_destroy, plugin_activate, plugin_deactivate,
                                      plugin_start_processing, plugin_stop_processing, plugin_reset, plugin_process,
                                      plugin_get_extension, plugin_on_main_thread };

static uint32_t factory_count(const clap_plugin_factory_t *factory) { (void)factory; return 1; }

static const clap_plugin_descriptor_t *factory_descriptor(const clap_plugin_factory_t *factory, uint32_t index)
{
    (void)factory;
    return index == 0 ? &g_desc : NULL;
}

/* one instance at a time: its stats start over at each create, the load the program set is kept */
static const clap_plugin_t *factory_create(const clap_plugin_factory_t *factory, const clap_host_t *host, const char *id)
{
    (void)factory;
    if (!id || strcmp(id, ID_POOL) != 0 || g_plugin)
        return NULL;
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.other_policy = -1;
    g_stats.other_priority = -1;
    g_host = host;
    g_plugin = &g_the_plugin;
    return g_plugin;
}

static const clap_plugin_factory_t g_factory = { factory_count, factory_descriptor, factory_create };

static bool entry_init(const char *path) { (void)path; return true; }
static void entry_deinit(void) {}
static const void *entry_get_factory(const char *id) { return !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &g_factory : NULL; }

CLAP_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory };
