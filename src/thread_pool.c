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
************************************************************************************************************************
*
* The thread pool (#395, docs/thread-pool.md). One pool per process: N - 1 helpers for the N cores of the affinity mask,
* each pinned to one core and run at the audio thread's scheduling, parked on a futex between requests. A request is one
* 64-bit word, the generation, the task count and the next index; the helpers and the requesting audio thread each take
* an index with one compare-and-swap, and the request returns once the completion count reaches the task count. Nothing
* is allocated and no lock is taken on the request's path; the control thread's lock guards the making and the ending.
*
************************************************************************************************************************
*/


/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "clap_host.h"
#include "thread_pool.h"


/*
************************************************************************************************************************
*           LOCAL DEFINES
************************************************************************************************************************
*/

#define POOL_PARK_SPIN_NS       50000u      // a helper spins this long after its last task before it sleeps
#define POOL_START_WAIT_NS      2000000000u // the making waits this long for every helper to say it runs
#define POOL_OWNER_SLEEP_NS     1000000L    // past the block the requesting thread sleeps on the count this long at a time
#define POOL_TASKS_MAX          0xffffu     // the hand-out word's task count and index are 16 bits each
#define POOL_CLAIM(gen, num, next)  (((uint64_t)(gen) << 32) | ((uint64_t)(num) << 16) | (uint64_t)(next))


/*
************************************************************************************************************************
*           LOCAL DATA TYPES
************************************************************************************************************************
*/

struct pool_helper
{
    _Atomic pthread_t self;         // the helper's own pthread_self, written by it before `started`
    _Atomic uint32_t started;       // 1 once it runs at its scheduling on its core, 2 when it could not set them
    _Atomic(const struct omx_clap_instance *) running;  // the instance whose task it runs, NULL between tasks
    _Atomic uint64_t tasks;         // the tasks it ran
    _Atomic uint64_t busy_ns;       // the time it spent inside them
    int core;                       // the core it is pinned to
};


/*
************************************************************************************************************************
*           LOCAL GLOBAL VARIABLES
************************************************************************************************************************
*/

static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_holders;          // the active instances holding the pool; the control thread's, under g_pool_lock
static uint32_t g_cores;            // the cores of the mask when the pool was last asked for
static int32_t g_priority;          // the helpers' priority
static char g_state[CLAP_HOST_LOG_BYTES];   // why the last instance that asked runs serial
static pthread_t g_join[OMX_CLAP_THREAD_POOL_HELPERS_MAX];  // what pthread_create answered, for the join
static struct pool_helper g_helper[OMX_CLAP_THREAD_POOL_HELPERS_MAX];

static _Atomic uint32_t g_helpers;  // the helpers running, published once every one says it runs
static _Atomic uint32_t g_stop;     // 1: the helpers end
static _Atomic uint32_t g_wake;     // the futex the helpers park on: one more per request
static _Atomic uint32_t g_sleepers; // the helpers asleep on it, or about to be
static _Atomic uint32_t g_owner;    // 1 while a request holds the pool
static _Atomic uint64_t g_claim;    // the request: POOL_CLAIM(generation, tasks, next index)
static _Atomic(struct omx_clap_instance *) g_job;   // the instance whose tasks the request hands out
static _Atomic uint32_t g_done;     // the tasks of the request ended; the futex the requesting thread sleeps on
static _Atomic uint32_t g_owner_sleeping;   // 1 while the requesting thread sleeps on g_done
static _Atomic uint64_t g_requests, g_busy, g_late, g_tasks_audio;


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS
************************************************************************************************************************
*/

static uint64_t pool_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static inline void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#endif
}

static void futex_wait(_Atomic uint32_t *word, uint32_t expected, const struct timespec *timeout)
{
    syscall(SYS_futex, (uint32_t *)word, FUTEX_WAIT_PRIVATE, expected, timeout, NULL, 0);
}

static void futex_wake(_Atomic uint32_t *word, int count)
{
    syscall(SYS_futex, (uint32_t *)word, FUTEX_WAKE_PRIVATE, count, NULL, NULL, 0);
}

/* one task: on a helper, timed and counted as the helper's, with the helper on the instance's audio role for it */
static void run_task(struct pool_helper *self, struct omx_clap_instance *in, uint32_t index)
{
    uint64_t t0;

    if (!self)
    {
        in->thread_pool->exec(in->plugin, index);
        atomic_fetch_add_explicit(&g_tasks_audio, 1u, memory_order_relaxed);
        return;
    }
    t0 = pool_now_ns();
    atomic_store_explicit(&self->running, in, memory_order_relaxed);
    in->thread_pool->exec(in->plugin, index);
    atomic_store_explicit(&self->running, NULL, memory_order_relaxed);
    atomic_fetch_add_explicit(&self->tasks, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&self->busy_ns, pool_now_ns() - t0, memory_order_relaxed);
}

/* Take tasks of the open request until none is left. The instance read between the word's load and the swap that takes
 * an index is the request's: the requesting thread writes the next request's instance only after it has read the word
 * with every index taken, and every swap before that read is a release it acquires, so a swap that took an index of
 * this request was preceded by a read of this request's instance. A helper's last task wakes a requesting thread that
 * sleeps on the count. */
static void take_tasks(struct pool_helper *self)
{
    uint64_t w = atomic_load_explicit(&g_claim, memory_order_acquire);

    for (;;)
    {
        const uint32_t num = (uint32_t)(w >> 16) & 0xffffu, next = (uint32_t)w & 0xffffu;
        struct omx_clap_instance *in;

        if (next >= num)
            return;
        in = atomic_load_explicit(&g_job, memory_order_relaxed);
        if (!atomic_compare_exchange_weak_explicit(&g_claim, &w, w + 1u, memory_order_acq_rel, memory_order_acquire))
            continue;
        run_task(self, in, next);
        if (atomic_fetch_add_explicit(&g_done, 1u, memory_order_seq_cst) + 1u == num && self &&
            atomic_load_explicit(&g_owner_sleeping, memory_order_seq_cst))
            futex_wake(&g_done, 1);
        w = atomic_load_explicit(&g_claim, memory_order_acquire);
    }
}

/* a helper: says it runs, then takes tasks and parks until the pool ends */
static void *helper_main(void *arg)
{
    struct pool_helper *self = arg;
    uint32_t seen = atomic_load_explicit(&g_wake, memory_order_acquire);

    atomic_store_explicit(&self->self, pthread_self(), memory_order_relaxed);
    atomic_store_explicit(&self->started, 1u, memory_order_release);
    while (!atomic_load_explicit(&g_stop, memory_order_acquire))
    {
        uint64_t t0;
        uint32_t spins = 0;

        take_tasks(self);
        // a while spinning, for a plugin that asks again within the block, then asleep until the next request
        t0 = pool_now_ns();
        while (atomic_load_explicit(&g_wake, memory_order_acquire) == seen && !atomic_load_explicit(&g_stop, memory_order_relaxed))
        {
            cpu_relax();
            if ((++spins & 63u) == 0u && pool_now_ns() - t0 > POOL_PARK_SPIN_NS)
            {
                atomic_fetch_add_explicit(&g_sleepers, 1u, memory_order_seq_cst);
                if (atomic_load_explicit(&g_wake, memory_order_seq_cst) == seen && !atomic_load_explicit(&g_stop, memory_order_seq_cst))
                    futex_wait(&g_wake, seen, NULL);
                atomic_fetch_sub_explicit(&g_sleepers, 1u, memory_order_seq_cst);
                break;
            }
        }
        seen = atomic_load_explicit(&g_wake, memory_order_acquire);
    }
    return NULL;
}

/* control thread, under g_pool_lock: the helpers told to end and joined */
static void end_helpers(uint32_t count)
{
    atomic_store_explicit(&g_helpers, 0u, memory_order_release);
    atomic_store_explicit(&g_stop, 1u, memory_order_seq_cst);
    atomic_fetch_add_explicit(&g_wake, 1u, memory_order_seq_cst);
    futex_wake(&g_wake, INT_MAX);
    for (uint32_t i = 0; i < count; i++)
        pthread_join(g_join[i], NULL);
}

/* why the instance runs serial, from g_state: kept for omx_clap_host_thread_pool, and the caller's log line */
static void say(char *line, size_t size)
{
    if (line && size)
        snprintf(line, size, "%s", g_state);
}


/*
************************************************************************************************************************
*           GLOBAL FUNCTIONS
************************************************************************************************************************
*/

int thread_pool_acquire(int32_t priority, char *line, size_t size)
{
    int cores[CPU_SETSIZE];
    uint32_t n = 0, count, i;
    uint64_t t0;
    cpu_set_t mask;

    pthread_mutex_lock(&g_pool_lock);
    if (g_holders)
    {
        g_holders++;
        pthread_mutex_unlock(&g_pool_lock);
        return 1;
    }
    CPU_ZERO(&mask);
    if (sched_getaffinity(0, sizeof(mask), &mask) == 0)
        for (int c = 0; c < CPU_SETSIZE; c++)
            if (CPU_ISSET(c, &mask))
                cores[n++] = c;
    g_cores = n;
    if (n < 2)
    {
        snprintf(g_state, sizeof(g_state), "thread pool: serial: one core in the affinity mask");
        say(line, size);
        pthread_mutex_unlock(&g_pool_lock);
        return 0;
    }
    count = n - 1u < OMX_CLAP_THREAD_POOL_HELPERS_MAX ? n - 1u : OMX_CLAP_THREAD_POOL_HELPERS_MAX;
    atomic_store_explicit(&g_stop, 0u, memory_order_relaxed);
    atomic_store_explicit(&g_requests, 0u, memory_order_relaxed);
    atomic_store_explicit(&g_busy, 0u, memory_order_relaxed);
    atomic_store_explicit(&g_late, 0u, memory_order_relaxed);
    atomic_store_explicit(&g_tasks_audio, 0u, memory_order_relaxed);
    for (i = 0; i < count; i++)
    {
        struct sched_param param = { .sched_priority = priority };
        pthread_attr_t attr;
        cpu_set_t one;
        int rc;

        g_helper[i].core = cores[i + 1u];       // the mask's first core is left to the audio thread
        atomic_store_explicit(&g_helper[i].started, 0u, memory_order_relaxed);
        atomic_store_explicit(&g_helper[i].running, NULL, memory_order_relaxed);
        atomic_store_explicit(&g_helper[i].tasks, 0u, memory_order_relaxed);
        atomic_store_explicit(&g_helper[i].busy_ns, 0u, memory_order_relaxed);
        CPU_ZERO(&one);
        CPU_SET(g_helper[i].core, &one);
        pthread_attr_init(&attr);
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&attr, priority > 0 ? SCHED_FIFO : SCHED_OTHER);
        if (priority <= 0)
            param.sched_priority = 0;
        pthread_attr_setschedparam(&attr, &param);
        pthread_attr_setaffinity_np(&attr, sizeof(one), &one);
        rc = pthread_create(&g_join[i], &attr, helper_main, &g_helper[i]);
        pthread_attr_destroy(&attr);
        if (rc != 0)
        {
            end_helpers(i);
            if (rc == EPERM && priority > 0)
                snprintf(g_state, sizeof(g_state), "thread pool: serial: SCHED_FIFO %d refused (%s)", (int)priority, strerror(rc));
            else
                snprintf(g_state, sizeof(g_state), "thread pool: serial: helper %u not started (%s)", i, strerror(rc));
            say(line, size);
            pthread_mutex_unlock(&g_pool_lock);
            return 0;
        }
    }
    t0 = pool_now_ns();
    for (i = 0; i < count; i++)
        while (atomic_load_explicit(&g_helper[i].started, memory_order_acquire) == 0u)
        {
            struct timespec ts = { 0, 100000L };

            if (pool_now_ns() - t0 > POOL_START_WAIT_NS)
            {
                end_helpers(count);
                snprintf(g_state, sizeof(g_state), "thread pool: serial: helper %u did not start", i);
                say(line, size);
                pthread_mutex_unlock(&g_pool_lock);
                return 0;
            }
            nanosleep(&ts, NULL);
        }
    g_priority = priority > 0 ? priority : 0;
    g_state[0] = '\0';
    g_holders = 1;
    atomic_store_explicit(&g_helpers, count, memory_order_release);
    pthread_mutex_unlock(&g_pool_lock);
    return 1;
}

void thread_pool_release(void)
{
    pthread_mutex_lock(&g_pool_lock);
    if (g_holders && --g_holders == 0)
        end_helpers(atomic_load_explicit(&g_helpers, memory_order_relaxed));
    pthread_mutex_unlock(&g_pool_lock);
}

int thread_pool_run(struct omx_clap_instance *in, uint32_t num, uint64_t budget_ns, int *late)
{
    uint32_t idle = 0, spins = 0;
    uint64_t start;

    *late = 0;
    if (num < 2u || num > POOL_TASKS_MAX || !atomic_load_explicit(&g_helpers, memory_order_acquire))
        return -1;
    if (!atomic_compare_exchange_strong_explicit(&g_owner, &idle, 1u, memory_order_acquire, memory_order_relaxed))
    {
        atomic_fetch_add_explicit(&g_busy, 1u, memory_order_relaxed);
        return -1;
    }
    start = pool_now_ns();
    atomic_store_explicit(&g_job, in, memory_order_relaxed);
    atomic_store_explicit(&g_done, 0u, memory_order_relaxed);
    atomic_store_explicit(&g_claim, POOL_CLAIM((atomic_load_explicit(&g_claim, memory_order_relaxed) >> 32) + 1u, num, 0u),
                          memory_order_release);
    atomic_fetch_add_explicit(&g_wake, 1u, memory_order_seq_cst);
    if (atomic_load_explicit(&g_sleepers, memory_order_seq_cst))
        futex_wake(&g_wake, INT_MAX);
    // the calling thread takes tasks too: a helper not awake yet is never waited for
    take_tasks(NULL);
    // what is left is tasks a helper has started: spun on for the block, then slept on
    while (atomic_load_explicit(&g_done, memory_order_acquire) < num)
    {
        cpu_relax();
        if ((++spins & 63u) == 0u && pool_now_ns() - start > budget_ns)
        {
            const struct timespec ts = { 0, POOL_OWNER_SLEEP_NS };
            uint32_t done;

            if (!*late)
            {
                *late = 1;
                atomic_fetch_add_explicit(&g_late, 1u, memory_order_relaxed);
            }
            atomic_store_explicit(&g_owner_sleeping, 1u, memory_order_seq_cst);
            done = atomic_load_explicit(&g_done, memory_order_seq_cst);
            if (done < num)
                futex_wait(&g_done, done, &ts);
            atomic_store_explicit(&g_owner_sleeping, 0u, memory_order_relaxed);
        }
    }
    atomic_fetch_add_explicit(&g_requests, 1u, memory_order_relaxed);
    atomic_store_explicit(&g_owner, 0u, memory_order_release);
    return 0;
}

int thread_pool_on_helper(const struct omx_clap_instance *in)
{
    const pthread_t self = pthread_self();
    const uint32_t count = atomic_load_explicit(&g_helpers, memory_order_acquire);

    for (uint32_t i = 0; i < count; i++)
        if (pthread_equal(self, atomic_load_explicit(&g_helper[i].self, memory_order_relaxed)))
            return atomic_load_explicit(&g_helper[i].running, memory_order_relaxed) == in;
    return 0;
}

int thread_pool_is_helper(void)
{
    const pthread_t self = pthread_self();
    const uint32_t count = atomic_load_explicit(&g_helpers, memory_order_acquire);

    for (uint32_t i = 0; i < count; i++)
        if (pthread_equal(self, atomic_load_explicit(&g_helper[i].self, memory_order_relaxed)))
            return 1;
    return 0;
}

void thread_pool_info(struct omx_clap_thread_pool_info *out)
{
    uint32_t i, count;

    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&g_pool_lock);
    count = atomic_load_explicit(&g_helpers, memory_order_relaxed);
    out->helpers = count;
    out->holders = g_holders;
    out->cores = g_cores;
    out->priority = g_priority;
    snprintf(out->state, sizeof(out->state), "%s", g_state);
    out->requests = atomic_load_explicit(&g_requests, memory_order_relaxed);
    out->busy = atomic_load_explicit(&g_busy, memory_order_relaxed);
    out->late = atomic_load_explicit(&g_late, memory_order_relaxed);
    out->tasks_audio = atomic_load_explicit(&g_tasks_audio, memory_order_relaxed);
    for (i = 0; i < OMX_CLAP_THREAD_POOL_HELPERS_MAX; i++)
    {
        out->helper_tasks[i] = atomic_load_explicit(&g_helper[i].tasks, memory_order_relaxed);
        out->helper_busy_ns[i] = atomic_load_explicit(&g_helper[i].busy_ns, memory_order_relaxed);
        out->helper_core[i] = i < count ? g_helper[i].core : -1;
    }
    pthread_mutex_unlock(&g_pool_lock);
}
