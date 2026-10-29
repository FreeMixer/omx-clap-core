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
* The control-thread host of libomx-clap-core (see clap_host.h for the contract).
*
* Threads, as CLAP assigns them: everything here is [main-thread] and runs on the control thread that opened the
* instance, except the host callbacks a plugin may call from its audio thread ([thread-safe]: request_*, log,
* params.request_flush), which touch only atomics and one lock-free message slot. A [main-thread] host callback reached
* from anywhere but the control thread is counted as a thread-check violation and otherwise ignored.
*
************************************************************************************************************************
*/


/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "clap_host.h"
#include "clap_meters.h"
#include "idle_hold.h"
#include "param_contract.h"
#include "resource_hints.h"
#include "thread_pool.h"


/*
************************************************************************************************************************
*           LOCAL DEFINES
************************************************************************************************************************
*/

/* major * 10000 + minor * 100 + patch of the file VERSION, passed in by the Makefile */
#ifndef OMX_CLAP_VERSION_NUM
#error "build with make: the version is passed in OMX_CLAP_VERSION_NUM from the file VERSION"
#endif
#define CORE_VERSION                    OMX_CLAP_VERSION_NUM


/*
************************************************************************************************************************
*           LOCAL DATA TYPES
************************************************************************************************************************
*/

/* one dlopen per file, or one linked entry, shared by its instances */
struct omx_clap_binary
{
    char *path;
    void *so;                   // NULL for a linked entry
    const clap_plugin_entry_t *entry;
    const clap_plugin_factory_t *factory;
    uint32_t refs;
    struct omx_clap_binary *next;
};

struct bounded_stream
{
    uint8_t *buf;
    const uint8_t *in;
    size_t cap, len, pos;
    int overflow;
};

struct timed_open
{
    const char *path, *id;
    struct omx_clap_instance *out;
    char why[OMX_CLAP_WHY_MAX];
    int rc;
};


/*
************************************************************************************************************************
*           LOCAL GLOBAL VARIABLES
************************************************************************************************************************
*/

static struct omx_clap_binary *g_binaries;

/* the defaults until a host configures the process */
static struct omx_clap_host_config g_config =
{
    OMX_CLAP_CORE_ABI, sizeof(struct omx_clap_host_config), 1, 1, 1, 0, 0, "omx-clap-core", "Pau Aliagas", "https://github.com/FreeMixer/omx-clap-core", "0", 0, 0, 2, CLAP_HOST_IDLE_HOLD_US
};
static int g_configured;
static int g_sealed;        // a binary was opened: the configuration can no longer change
static const clap_host_gui_t *g_gui;   // the caller's clap.gui (omx_clap_host_offer_gui), NULL for none


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS: THE BINARIES
************************************************************************************************************************
*/

static void why_set(char why[OMX_CLAP_WHY_MAX], const char *code)
{
    if (why)
        snprintf(why, OMX_CLAP_WHY_MAX, "%s", code);
}

/* Take `entry` (from a dlopen'd `so`, or linked in when `so` is NULL): version, init, factory. On refusal nothing
 * stays loaded and `reason` says why. */
static struct omx_clap_binary *binary_adopt(const char *path, void *so, const clap_plugin_entry_t *entry, char *reason, size_t reason_size)
{
    const clap_plugin_factory_t *factory;
    struct omx_clap_binary *binary;

    if (!entry || !clap_version_is_compatible(entry->clap_version) || !entry->init || !entry->init(path))
    {
        snprintf(reason, reason_size, "can't init %s", path);
        if (so)
            dlclose(so);
        return NULL;
    }
    factory = (const clap_plugin_factory_t *)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (!factory)
    {
        snprintf(reason, reason_size, "no plugin factory in %s", path);
        entry->deinit();
        if (so)
            dlclose(so);
        return NULL;
    }
    binary = calloc(1, sizeof(*binary));
    binary->path = strdup(path);
    binary->so = so;
    binary->entry = entry;
    binary->factory = factory;
    binary->refs = 1;
    binary->next = g_binaries;
    g_binaries = binary;
    g_sealed = 1;
    return binary;
}

int omx_clap_host_bundle_object(const char *dir, char *object, size_t cap, char *reason, size_t reason_size)
{
    DIR *handle = opendir(dir);
    struct dirent *entry;
    char clap_one[PATH_MAX] = "", so_one[PATH_MAX] = "";
    unsigned n_clap = 0, n_so = 0;

    if (!handle)
    {
        snprintf(reason, reason_size, "can't read %s: %s", dir, strerror(errno));
        return 0;
    }
    while ((entry = readdir(handle)) != NULL)
    {
        const size_t n = strlen(entry->d_name);
        char path[PATH_MAX];
        struct stat st;
        int is_clap = n > 5 && strcmp(entry->d_name + n - 5, ".clap") == 0;
        int is_so = n > 3 && strcmp(entry->d_name + n - 3, ".so") == 0;

        if (!is_clap && !is_so)
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) >= (int)sizeof(path))
            continue;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        if (is_clap)
        {
            if (n_clap++ == 0)
                snprintf(clap_one, sizeof(clap_one), "%s", path);
        }
        else if (n_so++ == 0)
            snprintf(so_one, sizeof(so_one), "%s", path);
    }
    closedir(handle);
    if (n_clap == 1 || (n_clap == 0 && n_so == 1))
    {
        if (snprintf(object, cap, "%s", n_clap ? clap_one : so_one) >= (int)cap)
        {
            snprintf(reason, reason_size, "the object's path does not fit %zu bytes", cap);
            return 0;
        }
        return 1;
    }
    if (n_clap > 1)
    {
        snprintf(reason, reason_size, "%u .clap files, one expected", n_clap);
        return (int)n_clap;
    }
    if (n_so > 1)
    {
        snprintf(reason, reason_size, "%u shared objects, one expected", n_so);
        return (int)n_so;
    }
    snprintf(reason, reason_size, "no shared object in %s", dir);
    return 0;
}

/* A foreign .clap: one dlopen per path. A bundle directory is its one object, keyed by the object's path. */
static struct omx_clap_binary *binary_ref(const char *path, char *reason, size_t reason_size)
{
    struct omx_clap_binary *binary;
    void *so;
    struct stat st;
    char object[PATH_MAX];

    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
    {
        if (omx_clap_host_bundle_object(path, object, sizeof(object), reason, reason_size) != 1)
            return NULL;
        path = object;
    }
    for (binary = g_binaries; binary; binary = binary->next)
    {
        if (binary->so && strcmp(binary->path, path) == 0)
        {
            binary->refs++;
            return binary;
        }
    }
    so = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!so)
    {
        snprintf(reason, reason_size, "can't open %s: %s", path, dlerror());
        return NULL;
    }
    return binary_adopt(path, so, (const clap_plugin_entry_t *)dlsym(so, "clap_entry"), reason, reason_size);
}

/* A plugin linked into the process, keyed by its entry, never dlopen'd. init receives the descriptor id, the only
 * "path" a linked plugin has. */
static struct omx_clap_binary *binary_ref_entry(const clap_plugin_entry_t *entry, const char *id, char *reason, size_t reason_size)
{
    struct omx_clap_binary *binary;

    for (binary = g_binaries; binary; binary = binary->next)
    {
        if (!binary->so && binary->entry == entry)
        {
            binary->refs++;
            return binary;
        }
    }
    return binary_adopt(id ? id : "", NULL, entry, reason, reason_size);
}

static void binary_unref(struct omx_clap_binary *binary)
{
    struct omx_clap_binary **link;

    if (!binary || --binary->refs > 0)
        return;

    for (link = &g_binaries; *link; link = &(*link)->next)
    {
        if (*link == binary)
        {
            *link = binary->next;
            break;
        }
    }

    binary->entry->deinit();
    if (binary->so)
        dlclose(binary->so);
    free(binary->path);
    free(binary);
}


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS: THE HOST OBJECT
************************************************************************************************************************
*/

static struct omx_clap_instance *instance_of(const clap_host_t *host)
{
    return (struct omx_clap_instance *)host->host_data;
}

/* The predicate and its context as one pair: retried while a write is under way or one landed between the reads. */
static int named_by_role(const struct omx_clap_instance *in, pthread_t self)
{
    omx_clap_audio_role_fn is_audio;
    void *ctx;
    uint32_t seq;

    do
    {
        seq = atomic_load_explicit(&in->audio_role_seq, memory_order_acquire);
        is_audio = atomic_load_explicit(&in->audio_role_is, memory_order_relaxed);
        ctx = atomic_load_explicit(&in->audio_role_ctx, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
    } while ((seq & 1u) || seq != atomic_load_explicit(&in->audio_role_seq, memory_order_relaxed));
    return is_audio && is_audio(ctx, self);
}

/* The thread the carrier's slot holds, read whole: the slot and the thread are atomics, no write ever half seen. */
static int named_by_slot(const struct omx_clap_instance *in, pthread_t self)
{
    const _Atomic(pthread_t) *slot = atomic_load_explicit(&in->audio_slot, memory_order_acquire);

    return slot && pthread_equal(self, atomic_load_explicit(slot, memory_order_acquire));
}

static int on_audio_role(const struct omx_clap_instance *in)
{
    const pthread_t self = pthread_self();

    if (named_by_slot(in, self))
        return 1;
    // a helper of the thread pool holds the role of the instance whose task it runs, for that task (#395)
    if (in->pool_held && thread_pool_on_helper(in))
        return 1;
    if (!in->audio_role_held)
        return 0;
    return pthread_equal(self, in->audio_thread) || named_by_role(in, self);
}

/* A [main-thread] callback reached from anywhere but the control thread is a violation. */
static int main_thread_call(const clap_host_t *host)
{
    struct omx_clap_instance *in = instance_of(host);

    if (pthread_equal(pthread_self(), in->main_thread) && !on_audio_role(in))
        return 1;
    atomic_fetch_add_explicit(&in->thread_violations, 1u, memory_order_relaxed);
    return 0;
}

static void log_to_ring(struct omx_clap_instance *in, const char *msg)
{
    size_t i = 0;

    // one lock-free slot, written by whoever logs, drained by the control thread's tick; a message that arrives before
    // the drain replaces the last, and nothing is ever written on the RT beyond it
    if (msg)
    {
        for (; i < sizeof(in->log_ring) - 1 && msg[i]; i++)
            in->log_ring[i] = msg[i];
        in->log_ring[i] = '\0';
    }
    atomic_store_explicit(&in->log_pending, 1u, memory_order_release);
}

static void host_log(const clap_host_t *host, clap_log_severity severity, const char *msg)
{
    struct omx_clap_instance *in = instance_of(host);

    (void)severity;
    atomic_fetch_add_explicit(&in->log_calls, 1u, memory_order_relaxed);
    log_to_ring(in, msg);
}

static bool host_is_main_thread(const clap_host_t *host)
{
    struct omx_clap_instance *in = instance_of(host);

    if (on_audio_role(in))
        return false;
    return pthread_equal(pthread_self(), in->main_thread);
}

static bool host_is_audio_thread(const clap_host_t *host)
{
    return on_audio_role(instance_of(host));
}

static void host_latency_changed(const clap_host_t *host)
{
    if (main_thread_call(host))
        atomic_store_explicit(&instance_of(host)->latency_changed, 1u, memory_order_relaxed);
}

static void host_params_rescan(const clap_host_t *host, clap_param_rescan_flags flags)
{
    if (main_thread_call(host))
        atomic_fetch_or_explicit(&instance_of(host)->params_rescan_flags, (uint32_t)flags, memory_order_relaxed);
}

/* clear: the host keeps no automation and no modulation of its own past the block it queued it for, so what it drops
 * is its reference to the parameter, the read-back shadow, when the plugin clears all of it */
static void host_params_clear(const clap_host_t *host, clap_id id, clap_param_clear_flags flags)
{
    struct omx_clap_instance *in = instance_of(host);
    uint32_t i;

    if (!main_thread_call(host) || !(flags & CLAP_PARAM_CLEAR_ALL))
        return;
    for (i = 0; i < in->shadow_n; i++)
        if (in->shadow[i].id == id)
        {
            in->shadow[i] = in->shadow[--in->shadow_n];
            break;
        }
}

static void host_params_request_flush(const clap_host_t *host)
{
    atomic_store_explicit(&instance_of(host)->flush_requested, 1u, memory_order_relaxed);
}

static bool host_ports_is_rescan_flag_supported(const clap_host_t *host, uint32_t flag)
{
    (void)host;
    (void)flag;
    return false;   // no rescan is applied live: each is parked and reported (omx_clap_host_ports_rescanned), and the
                    // caller re-describes the plugin inactive through omx_clap_host_request_layout
}

static void host_ports_rescan(const clap_host_t *host, uint32_t flags)
{
    (void)flags;
    if (main_thread_call(host))
        atomic_store_explicit(&instance_of(host)->ports_rescan_requested, 1u, memory_order_relaxed);
}

/* surround and ambisonic changed: the plugin's layout moved, which the host takes as the ports rescan it is */
static void host_layout_changed(const clap_host_t *host)
{
    if (main_thread_call(host))
        atomic_store_explicit(&instance_of(host)->ports_rescan_requested, 1u, memory_order_relaxed);
}

/* the plugin's configuration list changed (clap.audio-ports-config's host half). The host holds no copy of the list:
 * omx_clap_host_request_layout reads it live. While the plugin is inactive the change is reported as a ports rescan,
 * since the list is what the next request_layout chooses from (a plugin left DECLARED whose list now has a match).
 * While active it is dropped: the configuration in use stands, a report would only send the carrier through a
 * relayout for no port change, and the list is read again when the host next deactivates and asks (#263) */
static void host_configs_rescan(const clap_host_t *host)
{
    struct omx_clap_instance *in = instance_of(host);

    if (main_thread_call(host) && !in->active)
        atomic_store_explicit(&in->ports_rescan_requested, 1u, memory_order_relaxed);
}

static void host_state_mark_dirty(const clap_host_t *host)
{
    if (main_thread_call(host))
        atomic_store_explicit(&instance_of(host)->state_dirty, 1u, memory_order_relaxed);
}

/* the instance a preset load is in progress for, and whether its plugin reported on_error meanwhile: main thread only, kept
 * here so the public instance struct stays as the soname has it */
static const struct omx_clap_instance *g_preset_loading;
static int g_preset_failed;

static void host_preset_on_error(const clap_host_t *host, uint32_t location_kind, const char *location, const char *load_key,
                                 int32_t os_error, const char *msg)
{
    char text[CLAP_HOST_LOG_BYTES];

    (void)location_kind;
    (void)load_key;
    (void)os_error;
    if (main_thread_call(host))
    {
        if (instance_of(host) == g_preset_loading)
            g_preset_failed = 1;
        snprintf(text, sizeof(text), "preset %s: %s", location ? location : "", msg ? msg : "error");
        log_to_ring(instance_of(host), text);
    }
}

static void host_preset_loaded(const clap_host_t *host, uint32_t location_kind, const char *location, const char *load_key)
{
    (void)location_kind;
    (void)location;
    (void)load_key;
    main_thread_call(host);
}

static const clap_host_log_t g_host_log = { host_log };
static const clap_host_thread_check_t g_host_thread_check = { host_is_main_thread, host_is_audio_thread };
static const clap_host_latency_t g_host_latency = { host_latency_changed };
static const clap_host_params_t g_host_params = { host_params_rescan, host_params_clear, host_params_request_flush };
static const clap_host_audio_ports_t g_host_audio_ports = { host_ports_is_rescan_flag_supported, host_ports_rescan };
static const clap_host_state_t g_host_state = { host_state_mark_dirty };
static const clap_host_preset_load_t g_host_preset_load = { host_preset_on_error, host_preset_loaded };

static bool host_track_info_get(const clap_host_t *host, clap_track_info_t *info)
{
    struct omx_clap_instance *in = instance_of(host);

    if (!main_thread_call(host) || !in->track_set)
        return false;
    *info = in->track;
    return true;
}

static void host_remote_controls_changed(const clap_host_t *host)
{
    if (main_thread_call(host))
        atomic_store(&instance_of(host)->remote_controls_changed, 1u);
}

/* the page a plugin would have shown: no surface follows it */
static void host_remote_controls_suggest_page(const clap_host_t *host, clap_id page_id)
{
    (void)page_id;
    main_thread_call(host);
}

/* what a note input can be fed in, or nothing when the configuration admits none */
static uint32_t host_note_ports_supported_dialects(const clap_host_t *host)
{
    if (!main_thread_call(host))
        return 0;
    return g_config.note_inputs ? OMX_CLAP_HOST_NOTE_DIALECTS : 0;
}

static void host_note_ports_rescan(const clap_host_t *host, uint32_t flags)
{
    if (main_thread_call(host))
        atomic_fetch_or_explicit(&instance_of(host)->note_ports_rescan_flags, flags, memory_order_relaxed);
}

static void host_note_name_changed(const clap_host_t *host)
{
    if (main_thread_call(host))
        atomic_store_explicit(&instance_of(host)->note_names_changed, 1u, memory_order_relaxed);
}

static void host_voice_info_changed(const clap_host_t *host)
{
    if (main_thread_call(host))
        atomic_store_explicit(&instance_of(host)->voice_info_changed, 1u, memory_order_relaxed);
}

/* the host reads no event space but the core one, so it knows no other space to name: every query answers false */
static bool host_event_registry_query(const clap_host_t *host, const char *space_name, uint16_t *space_id)
{
    (void)space_name;
    if (space_id)
        *space_id = UINT16_MAX;
    main_thread_call(host);
    return false;
}

static const clap_host_event_registry_t g_host_event_registry = { host_event_registry_query };

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* timer-support: a slot of the instance, its first call one period from now; the tick calls on_timer when it is due */
static bool host_register_timer(const clap_host_t *host, uint32_t period_ms, clap_id *timer_id)
{
    struct omx_clap_instance *in = instance_of(host);
    uint32_t i;

    if (!main_thread_call(host) || !timer_id)
        return false;
    for (i = 0; i < OMX_CLAP_TIMERS && in->timers[i].used; i++)
        ;
    if (i == OMX_CLAP_TIMERS)
        return false;
    in->timers[i].used = 1;
    in->timers[i].id = in->timer_next_id++;
    in->timers[i].period_ms = period_ms < OMX_CLAP_TIMER_MIN_MS ? OMX_CLAP_TIMER_MIN_MS : period_ms;
    in->timers[i].due_ns = now_ns() + (uint64_t)in->timers[i].period_ms * 1000000u;
    *timer_id = in->timers[i].id;
    return true;
}

static bool host_unregister_timer(const clap_host_t *host, clap_id timer_id)
{
    struct omx_clap_instance *in = instance_of(host);

    if (!main_thread_call(host))
        return false;
    for (uint32_t i = 0; i < OMX_CLAP_TIMERS; i++)
        if (in->timers[i].used && in->timers[i].id == timer_id)
        {
            in->timers[i].used = 0;
            return true;
        }
    return false;
}

/* posix-fd-support: a slot of the instance; the tick polls the descriptors and calls on_fd with what is ready */
static int fd_slot(struct omx_clap_instance *in, int fd)
{
    for (uint32_t i = 0; i < OMX_CLAP_FDS; i++)
        if (in->fds[i].fd == fd)
            return (int)i;
    return -1;
}

static bool host_register_fd(const clap_host_t *host, int fd, clap_posix_fd_flags_t flags)
{
    struct omx_clap_instance *in = instance_of(host);
    int i;

    if (!main_thread_call(host) || fd < 0 || fd_slot(in, fd) >= 0 || (i = fd_slot(in, -1)) < 0)
        return false;
    in->fds[i].fd = fd;
    in->fds[i].flags = flags;
    return true;
}

static bool host_modify_fd(const clap_host_t *host, int fd, clap_posix_fd_flags_t flags)
{
    struct omx_clap_instance *in = instance_of(host);
    int i;

    if (!main_thread_call(host) || fd < 0 || (i = fd_slot(in, fd)) < 0)
        return false;
    in->fds[i].flags = flags;
    return true;
}

static bool host_unregister_fd(const clap_host_t *host, int fd)
{
    struct omx_clap_instance *in = instance_of(host);
    int i;

    if (!main_thread_call(host) || fd < 0 || (i = fd_slot(in, fd)) < 0)
        return false;
    in->fds[i].fd = -1;
    in->fds[i].flags = 0;
    return true;
}

/* thread-pool, on the audio role inside process(): every task has ended when the call returns. An instance holding the
 * process's pool (granted it, docs/thread-pool.md) hands its tasks to the helpers and the calling thread; any other runs
 * them one after the other on the calling thread, which is real-time safe and what the extension allows. A wait for the
 * helpers past the block is counted late. Anywhere else the request is refused, a helper's own included */
static bool host_request_exec(const clap_host_t *host, uint32_t num_tasks)
{
    struct omx_clap_instance *in = instance_of(host);
    int late = 0;

    if (thread_pool_is_helper())
        return false;       // a task asking again from a helper: refused, the plugin runs its own
    if (!in->thread_pool || !in->thread_pool->exec || !on_audio_role(in) || in->in_exec)
        return false;       // a request made from inside exec would nest the tasks: refused, the plugin runs its own
    in->in_exec = 1;
    if (!in->pool_held ||
        thread_pool_run(in, num_tasks, (uint64_t)((double)in->max_block * 1e9 / in->rate), &late) != 0)
        for (uint32_t i = 0; i < num_tasks; i++)
            in->thread_pool->exec(in->plugin, i);
    if (late)
        atomic_fetch_add_explicit(&in->pool_late, 1u, memory_order_relaxed);
    in->in_exec = 0;
    return true;
}

/* tail: the tick reads it again */
static void host_tail_changed(const clap_host_t *host)
{
    atomic_store_explicit(&instance_of(host)->tail_changed, 1u, memory_order_relaxed);
}

static const clap_host_timer_support_t g_host_timer_support = { host_register_timer, host_unregister_timer };
static const clap_host_posix_fd_support_t g_host_posix_fd_support = { host_register_fd, host_modify_fd, host_unregister_fd };
static const clap_host_thread_pool_t g_host_thread_pool = { host_request_exec };
static const clap_host_tail_t g_host_tail = { host_tail_changed };
static const clap_host_surround_t g_host_surround = { host_layout_changed };
static const clap_host_ambisonic_t g_host_ambisonic = { host_layout_changed };
static const clap_host_audio_ports_config_t g_host_audio_ports_config = { host_configs_rescan };
static const clap_host_note_ports_t g_host_note_ports = { host_note_ports_supported_dialects, host_note_ports_rescan };
static const clap_host_note_name_t g_host_note_name = { host_note_name_changed };
static const clap_host_voice_info_t g_host_voice_info = { host_voice_info_changed };

static const clap_host_track_info_t g_host_track_info = { host_track_info_get };
static const clap_host_remote_controls_t g_host_remote_controls = { host_remote_controls_changed,
                                                                    host_remote_controls_suggest_page };

static const char *const g_extension_ids[] = CLAP_HOST_EXTENSIONS_INIT;

/* The extensions a plugin asked for and the host did not offer, once per (instance, id): a fixed table shared by every
 * instance, so the public instance keeps its layout and nothing is allocated where get_extension runs, which may be any
 * thread. The lookup and the claim of a slot are one step under a flag, so an id asked from two threads at once is
 * claimed once; the audio role never waits on the flag, and an id it finds the flag busy for, or one past the table,
 * is counted unsaid. The audio role is the published one (omx_clap_host_publish, set_audio_thread): an audio thread
 * that asks before it is named waits as any other thread does, for the flag's few compares. The owner's tick says one pending id per log line ("extension not offered: <id>"), then the
 * unsaid count ("<N> unoffered ids unsaid, table full or busy"), and the instance's end frees its slots. */
#define UNOFFERED_SLOTS 256
#define UNOFFERED_ID_BYTES 64

_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2, "the unoffered table's atomics are lock-free");

enum { UNOFFERED_FREE, UNOFFERED_PENDING, UNOFFERED_SAID };

static struct
{
    _Atomic(struct omx_clap_instance *) owner;
    _Atomic uint32_t state;
    char id[UNOFFERED_ID_BYTES];
} g_unoffered[UNOFFERED_SLOTS];
static atomic_flag g_unoffered_lock = ATOMIC_FLAG_INIT;
static _Atomic uint32_t g_unoffered_unsaid;

static void unoffered_note(struct omx_clap_instance *in, const char *id)
{
    size_t i, k, free_slot = UNOFFERED_SLOTS;

    if (!in)
        return;
    while (atomic_flag_test_and_set_explicit(&g_unoffered_lock, memory_order_acquire))
        if (on_audio_role(in))
        {
            atomic_fetch_add_explicit(&g_unoffered_unsaid, 1u, memory_order_relaxed);
            return;
        }
    for (i = 0; i < UNOFFERED_SLOTS; i++)
    {
        struct omx_clap_instance *owner = atomic_load_explicit(&g_unoffered[i].owner, memory_order_acquire);

        if (owner == in && strncmp(g_unoffered[i].id, id, UNOFFERED_ID_BYTES - 1) == 0)
            break;
        if (!owner && free_slot == UNOFFERED_SLOTS)
            free_slot = i;
    }
    if (i == UNOFFERED_SLOTS && free_slot == UNOFFERED_SLOTS)
        atomic_fetch_add_explicit(&g_unoffered_unsaid, 1u, memory_order_relaxed);
    else if (i == UNOFFERED_SLOTS)
    {
        // the id and the state first, the owner last: a tick that sees its owner sees the id whole
        for (k = 0; k < UNOFFERED_ID_BYTES - 1 && id[k]; k++)
            g_unoffered[free_slot].id[k] = id[k];
        g_unoffered[free_slot].id[k] = '\0';
        atomic_store_explicit(&g_unoffered[free_slot].state, UNOFFERED_PENDING, memory_order_relaxed);
        atomic_store_explicit(&g_unoffered[free_slot].owner, in, memory_order_release);
    }
    atomic_flag_clear_explicit(&g_unoffered_lock, memory_order_release);
}

/* the control thread's tick, the log's one slot free: one pending id of this instance, else the unsaid count; 1 when it
 * said one */
static int unoffered_say(struct omx_clap_instance *in)
{
    size_t i;
    uint32_t unsaid;

    for (i = 0; i < UNOFFERED_SLOTS; i++)
        if (atomic_load_explicit(&g_unoffered[i].owner, memory_order_acquire) == in
            && atomic_load_explicit(&g_unoffered[i].state, memory_order_relaxed) == UNOFFERED_PENDING)
        {
            snprintf(in->last_log, sizeof(in->last_log), "extension not offered: %s", g_unoffered[i].id);
            atomic_store_explicit(&g_unoffered[i].state, UNOFFERED_SAID, memory_order_relaxed);
            return 1;
        }
    unsaid = atomic_exchange_explicit(&g_unoffered_unsaid, 0u, memory_order_relaxed);
    if (!unsaid)
        return 0;
    snprintf(in->last_log, sizeof(in->last_log), "%u unoffered ids unsaid, table full or busy", unsaid);
    return 1;
}

/* the instance ends, its plugin destroyed: its slots are free for the next */
static void unoffered_forget(struct omx_clap_instance *in)
{
    size_t i;

    while (atomic_flag_test_and_set_explicit(&g_unoffered_lock, memory_order_acquire))
        ;
    for (i = 0; i < UNOFFERED_SLOTS; i++)
        if (atomic_load_explicit(&g_unoffered[i].owner, memory_order_relaxed) == in)
        {
            atomic_store_explicit(&g_unoffered[i].state, UNOFFERED_FREE, memory_order_relaxed);
            atomic_store_explicit(&g_unoffered[i].owner, NULL, memory_order_relaxed);
        }
    atomic_flag_clear_explicit(&g_unoffered_lock, memory_order_release);
}

/* only what the declared list offers, and the preset-load extension where the configuration adds it: a verdict
 * promised no more */
static const void *host_get_extension(const clap_host_t *host, const char *id)
{
    size_t i;
    int declared = 0;

    if (!id)
        return NULL;
    if (g_config.preset_load && (!strcmp(id, CLAP_EXT_PRESET_LOAD) || !strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT)))
        return &g_host_preset_load;
    if (g_config.track_info && (!strcmp(id, CLAP_EXT_TRACK_INFO) || !strcmp(id, CLAP_EXT_TRACK_INFO_COMPAT)))
        return &g_host_track_info;
    if (g_config.remote_controls && (!strcmp(id, CLAP_EXT_REMOTE_CONTROLS) || !strcmp(id, CLAP_EXT_REMOTE_CONTROLS_COMPAT)))
        return &g_host_remote_controls;
    if (g_gui && !strcmp(id, CLAP_EXT_GUI))
        return g_gui;
    // the draft ids of the two layout extensions, answered as their own; the declared list names the final ids only
    if (!strcmp(id, CLAP_EXT_SURROUND_COMPAT))
        return &g_host_surround;
    if (!strcmp(id, CLAP_EXT_AMBISONIC_COMPAT))
        return &g_host_ambisonic;
    for (i = 0; g_extension_ids[i]; i++)
        declared |= strcmp(g_extension_ids[i], id) == 0;
    if (!declared)
    {
        unoffered_note(instance_of(host), id);
        return NULL;
    }
    if (!strcmp(id, CLAP_EXT_LOG))
        return &g_host_log;
    if (!strcmp(id, CLAP_EXT_THREAD_CHECK))
        return &g_host_thread_check;
    if (!strcmp(id, CLAP_EXT_LATENCY))
        return &g_host_latency;
    if (!strcmp(id, CLAP_EXT_PARAMS))
        return &g_host_params;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &g_host_audio_ports;
    if (!strcmp(id, CLAP_EXT_STATE))
        return &g_host_state;
    if (!strcmp(id, CLAP_EXT_NOTE_PORTS))
        return &g_host_note_ports;
    if (!strcmp(id, CLAP_EXT_NOTE_NAME))
        return &g_host_note_name;
    if (!strcmp(id, CLAP_EXT_VOICE_INFO))
        return &g_host_voice_info;
    if (!strcmp(id, CLAP_EXT_EVENT_REGISTRY))
        return &g_host_event_registry;
    if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT))
        return &g_host_timer_support;
    if (!strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT))
        return &g_host_posix_fd_support;
    if (!strcmp(id, CLAP_EXT_THREAD_POOL))
        return &g_host_thread_pool;
    if (!strcmp(id, CLAP_EXT_TAIL))
        return &g_host_tail;
    if (!strcmp(id, CLAP_EXT_SURROUND))
        return &g_host_surround;
    if (!strcmp(id, CLAP_EXT_AMBISONIC))
        return &g_host_ambisonic;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS_CONFIG))
        return &g_host_audio_ports_config;
    return NULL;
}

static void host_request_restart(const clap_host_t *host)
{
    atomic_store_explicit(&instance_of(host)->restart_requested, 1u, memory_order_relaxed);
}

/* a sleeping plugin is woken for its next block; one that is awake is processed anyway */
static void host_request_process(const clap_host_t *host)
{
    atomic_store_explicit(&instance_of(host)->sleep.wake, 1u, memory_order_relaxed);
}

static void host_request_callback(const clap_host_t *host)
{
    atomic_store_explicit(&instance_of(host)->callback_requested, 1u, memory_order_relaxed);
}


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS: OPEN AND THE PORTS
************************************************************************************************************************
*/

/* the note input, when the plugin has one: how it is fed, the code when it can't be */
static const char *read_note_input(struct omx_clap_instance *in)
{
    const clap_plugin_note_ports_t *ports = in->note_ports;
    clap_note_port_info_t info;
    const uint32_t count = ports ? ports->count(in->plugin, true) : 0;

    if (count == 0)
        return NULL;
    if (count > 1)
        return CLAP_HOST_CODE_NOTE_INPUT;
    memset(&info, 0, sizeof(info));
    if (!ports->get(in->plugin, 0, true, &info))
        return CLAP_HOST_CODE_NOTE_INPUT;
    if (info.preferred_dialect == CLAP_NOTE_DIALECT_MIDI_MPE && (info.supported_dialects & CLAP_NOTE_DIALECT_MIDI_MPE))
        in->note_dialect = CLAP_NOTE_DIALECT_MIDI_MPE;     // raw MIDI, the plugin reads the MPE zone itself
    else if (info.preferred_dialect == CLAP_NOTE_DIALECT_MIDI && (info.supported_dialects & CLAP_NOTE_DIALECT_MIDI))
        in->note_dialect = CLAP_NOTE_DIALECT_MIDI;
    else if (info.preferred_dialect == CLAP_NOTE_DIALECT_MIDI2 && (info.supported_dialects & CLAP_NOTE_DIALECT_MIDI2))
        in->note_dialect = CLAP_NOTE_DIALECT_MIDI2;     // MIDI 1.0 in, translated to MIDI 2.0 channel voice messages
    else if (info.supported_dialects & CLAP_NOTE_DIALECT_CLAP)
        in->note_dialect = CLAP_NOTE_DIALECT_CLAP;
    else if (info.supported_dialects & CLAP_NOTE_DIALECT_MIDI)
        in->note_dialect = CLAP_NOTE_DIALECT_MIDI;
    else if (info.supported_dialects & CLAP_NOTE_DIALECT_MIDI_MPE)
        in->note_dialect = CLAP_NOTE_DIALECT_MIDI_MPE;
    else if (info.supported_dialects & CLAP_NOTE_DIALECT_MIDI2)
        in->note_dialect = CLAP_NOTE_DIALECT_MIDI2;
    else
        return CLAP_HOST_CODE_NOTE_INPUT;
    in->note_dialects = info.supported_dialects;
    in->note_inputs = 1;
    return NULL;
}

/* After a rescan of every note port, at activate: the note input read again. The new ports must be ones open admits for
 * this instance, which also means keeping its note input when it had one and taking none when it had none (the audio
 * topology rests on it); otherwise the old reading stays and the code is returned. */
static const char *reread_note_input(struct omx_clap_instance *in)
{
    const uint32_t inputs = in->note_inputs, dialect = in->note_dialect, dialects = in->note_dialects;
    const char *code;

    in->note_ports_stale = 0;
    if (!g_config.note_inputs)
        return in->note_ports && in->note_ports->count(in->plugin, true) > 0 ? CLAP_HOST_CODE_NOTE_INPUT : NULL;
    in->note_inputs = 0;
    code = read_note_input(in);
    if (!code && in->note_inputs != inputs)
        code = CLAP_HOST_CODE_NOTE_INPUT;
    if (code)
    {
        in->note_inputs = inputs;
        in->note_dialect = dialect;
        in->note_dialects = dialects;
    }
    return code;
}

/* the host's placement for a pass-through tool: a stereo port of no id, named for what it is */
static void placement_of(struct omx_clap_port *port, bool is_input)
{
    memset(port, 0, sizeof(*port));
    port->id = CLAP_INVALID_ID;
    snprintf(port->name, sizeof(port->name), "%s (passthrough: host)", is_input ? "in" : "out");
    port->channels = 2;
    port->kind = OMX_CLAP_PORT_STEREO;
    port->in_place_pair = CLAP_INVALID_ID;
}

/* a port as the plugin declares it, into the description */
static void port_of(struct omx_clap_port *port, const clap_audio_port_info_t *info)
{
    memset(port, 0, sizeof(*port));
    port->id = info->id;
    snprintf(port->name, sizeof(port->name), "%.*s", (int)sizeof(info->name), info->name);
    port->channels = info->channel_count;
    port->kind = !info->port_type ? OMX_CLAP_PORT_UNKNOWN
               : !strcmp(info->port_type, CLAP_PORT_MONO) ? OMX_CLAP_PORT_MONO
               : !strcmp(info->port_type, CLAP_PORT_STEREO) ? OMX_CLAP_PORT_STEREO
               : !strcmp(info->port_type, CLAP_PORT_SURROUND) ? OMX_CLAP_PORT_SURROUND
               : !strcmp(info->port_type, CLAP_PORT_AMBISONIC) ? OMX_CLAP_PORT_AMBISONIC
               : OMX_CLAP_PORT_UNKNOWN;
    port->in_place_pair = info->in_place_pair;
}

/*
 * What a surround or an ambisonic port's channels are, from the plugin's extension, at open. A surround port's map is
 * clap.surround's get_channel_map: a count other than the port's channels, or an id past CLAP_SURROUND_TSR, refuses it
 * (channel-map-unknown); with no clap.surround the channels are mapped in index order, 0 to channels - 1, and a port of
 * more than CLAP_SURROUND_TSR + 1 channels, whose index order would name ids the map cannot, is refused the same. An ambisonic
 * port must hold a full order, (order+1)² channels, or it is refused (port-too-wide); its ordering and normalisation
 * are clap.ambisonic's get_config, and without the extension, or when get_config fails, ACN and SN3D (AmbiX) with
 * ambisonic_assumed set: the core read a convention the plugin did not state. Returns the hosting code that refuses, or NULL.
 */
static const char *read_layout(const struct omx_clap_instance *in, struct omx_clap_port *port, bool is_input, uint32_t index)
{
    if (port->kind == OMX_CLAP_PORT_SURROUND)
    {
        uint32_t c;

        if (!in->surround || !in->surround->get_channel_map)
        {
            if (port->channels > CLAP_SURROUND_TSR + 1u)    // index order names ids up to channels - 1
                return CLAP_HOST_CODE_CHANNEL_MAP_UNKNOWN;
            for (c = 0; c < port->channels; c++)
                port->map[c] = (uint8_t)c;
            return NULL;
        }
        if (in->surround->get_channel_map(in->plugin, is_input, index, port->map, CLAP_HOST_PORT_CHANNELS) != port->channels)
            return CLAP_HOST_CODE_CHANNEL_MAP_UNKNOWN;
        for (c = 0; c < port->channels; c++)
            if (port->map[c] > CLAP_SURROUND_TSR)
                return CLAP_HOST_CODE_CHANNEL_MAP_UNKNOWN;
    }
    else if (port->kind == OMX_CLAP_PORT_AMBISONIC)
    {
        clap_ambisonic_config_t config = { CLAP_AMBISONIC_ORDERING_ACN, CLAP_AMBISONIC_NORMALIZATION_SN3D };
        uint32_t root = 0;

        while ((root + 1) * (root + 1) <= port->channels)
            root++;
        if (root * root != port->channels)      // order = root - 1: (order+1)² channels, nothing in between
            return CLAP_HOST_CODE_PORT_TOO_WIDE;
        port->ambisonic_assumed = 1;
        if (in->ambisonic && in->ambisonic->get_config && in->ambisonic->get_config(in->plugin, is_input, index, &config))
            port->ambisonic_assumed = 0;
        else
            config = (clap_ambisonic_config_t){ CLAP_AMBISONIC_ORDERING_ACN, CLAP_AMBISONIC_NORMALIZATION_SN3D };
        port->ambisonic_ordering = config.ordering;
        port->ambisonic_normalization = config.normalization;
    }
    return NULL;
}

/* the next auxiliary port of a side into `ports`, its channels after the last one's in the flat arrays */
static const char *aux_of(struct omx_clap_port *ports, uint32_t *base, uint32_t *count, uint32_t limit, const clap_audio_port_info_t *info)
{
    if (*count >= limit)
        return CLAP_HOST_CODE_TOO_MANY_PORTS;
    port_of(&ports[*count], info);
    base[*count] = *count ? base[*count - 1] + ports[*count - 1].channels : 0;
    (*count)++;
    return NULL;
}

/*
 * Every audio port the plugin declares, described: the main input and output (none of a side is channels 0) and each
 * auxiliary port, in the plugin's order, up to CLAP_HOST_AUX_INPUTS and CLAP_HOST_AUX_OUTPUTS of them. Each port is
 * judged as it is read: no channels refuses it (port-without-channels), more than CLAP_HOST_PORT_CHANNELS refuses it
 * (port-too-wide), a second main port on a side or one auxiliary port past the limit refuses it (too-many-ports), and a
 * surround or an ambisonic port is read through its extension (read_layout) and refused as that says. Then
 * the whole: no main output is nothing to carry (no-audio-output), the main pair may differ in width, no main input
 * at all is a source (an instrument with a note input, a generator without one, #184), its output its own. No audio
 * port at all (a note tool, a controller) is a pass-through tool, the host's placement (#270): a stereo pair named
 * for it on both sides, the strip's audio through the stage untouched, the plugin run for its notes and parameters
 * alone, as the LV2 adapter hosts a control-only tool. Where the configuration admits note inputs, one note input;
 * otherwise none. Returns the hosting code that refuses, or NULL. How wide a main port the caller's lane carries is
 * activation's to judge.
 */
static const char *describe_ports(struct omx_clap_instance *in)
{
    const clap_plugin_audio_ports_t *ports = in->audio_ports;
    const char *code;
    int dir;

    if (g_config.note_inputs)
    {
        code = read_note_input(in);
        if (code)
            return code;
    }

    if (ports)
    {
        for (dir = 0; dir < 2; dir++)
        {
            const bool is_input = dir == 0;
            const uint32_t count = ports->count(in->plugin, is_input);
            struct omx_clap_port *main = is_input ? &in->main_in : &in->main_out;
            uint32_t i;

            for (i = 0; i < count; i++)
            {
                clap_audio_port_info_t info;

                memset(&info, 0, sizeof(info));
                if (!ports->get(in->plugin, i, is_input, &info))
                    continue;
                if (info.channel_count == 0)
                    return CLAP_HOST_CODE_PORT_WITHOUT_CHANNELS;
                if (info.channel_count > CLAP_HOST_PORT_CHANNELS)
                    return CLAP_HOST_CODE_PORT_TOO_WIDE;
                if (info.flags & CLAP_AUDIO_PORT_IS_MAIN)
                {
                    if (main->channels)
                        return CLAP_HOST_CODE_TOO_MANY_PORTS;
                    port_of(main, &info);
                    code = read_layout(in, main, is_input, i);
                    if (code)
                        return code;
                    continue;
                }
                code = is_input ? aux_of(in->aux_in, in->aux_in_base, &in->aux_inputs, CLAP_HOST_AUX_INPUTS, &info)
                                : aux_of(in->aux_out, in->aux_out_base, &in->aux_outputs, CLAP_HOST_AUX_OUTPUTS, &info);
                if (!code)
                    code = read_layout(in, is_input ? &in->aux_in[in->aux_inputs - 1] : &in->aux_out[in->aux_outputs - 1], is_input, i);
                if (code)
                    return code;
            }
        }
    }
    if (in->main_out.channels == 0)
    {
        // no main output. An analyser (audio inputs), or a plugin whose every output is auxiliary, has nothing the
        // host can carry. A plugin with no audio port at all is a pass-through tool: the strip's stereo pair passes
        // through it untouched, the host's placement, and it runs for its notes and parameters alone (#270)
        if (in->main_in.channels || in->aux_inputs || in->aux_outputs)
            return CLAP_HOST_CODE_NO_AUDIO_OUTPUT;
        in->passthrough = 1;
        placement_of(&in->main_in, true);
        placement_of(&in->main_out, false);
    }
    if (!g_config.note_inputs && in->note_ports && in->note_ports->count(in->plugin, true) > 0)
        return CLAP_HOST_CODE_NOTE_INPUT;
    in->channels = in->main_out.channels;
    in->in_channels = in->main_in.channels;
    in->note_outputs = in->note_ports ? in->note_ports->count(in->plugin, false) : 0;
    return NULL;
}

/* the audio description emptied, as a fresh instance has it, for describe_ports to read again */
static void ports_forget(struct omx_clap_instance *in)
{
    memset(&in->main_in, 0, sizeof(in->main_in));
    memset(&in->main_out, 0, sizeof(in->main_out));
    memset(in->aux_in, 0, sizeof(in->aux_in));
    memset(in->aux_out, 0, sizeof(in->aux_out));
    memset(in->aux_in_base, 0, sizeof(in->aux_in_base));
    memset(in->aux_out_base, 0, sizeof(in->aux_out_base));
    in->aux_inputs = 0;
    in->aux_outputs = 0;
    in->in_channels = 0;
    in->channels = 0;
    in->passthrough = 0;
}

/* whether a side's auxiliary ports are still `count` of the widths in `widths`, in that order */
static bool aux_kept(const struct omx_clap_port *ports, uint32_t now, uint32_t count, const uint32_t *widths)
{
    uint32_t i;

    if (now != count)
        return false;
    for (i = 0; i < count; i++)
        if (ports[i].channels != widths[i])
            return false;
    return true;
}

/*
 * The description read again, as open reads it: inactive, main thread. A side whose auxiliary ports keep their count
 * and widths keeps its bindings; the other side's are dropped, its channels no longer where they were. A refusal leaves
 * the description empty (no main output, which activation refuses) and returns its code; NULL otherwise.
 */
static const char *ports_describe_again(struct omx_clap_instance *in)
{
    uint32_t in_widths[CLAP_HOST_AUX_INPUTS], out_widths[CLAP_HOST_AUX_OUTPUTS];
    const uint32_t inputs = in->aux_inputs, outputs = in->aux_outputs;
    const char *code;
    uint32_t i;

    for (i = 0; i < inputs; i++)
        in_widths[i] = in->aux_in[i].channels;
    for (i = 0; i < outputs; i++)
        out_widths[i] = in->aux_out[i].channels;
    // this read answers every ports rescan the plugin made up to here, an apply or a select included
    atomic_store_explicit(&in->ports_rescan_requested, 0u, memory_order_relaxed);
    atomic_store_explicit(&in->ports_rescanned, 0u, memory_order_relaxed);
    ports_forget(in);
    code = describe_ports(in);
    if (code)
        ports_forget(in);
    if (!aux_kept(in->aux_in, in->aux_inputs, inputs, in_widths))
    {
        memset(in->aux_in_bound, 0, sizeof(in->aux_in_bound));
        memset(in->aux_in_frames, 0, sizeof(in->aux_in_frames));
    }
    if (!aux_kept(in->aux_out, in->aux_outputs, outputs, out_widths))
    {
        memset(in->aux_out_bound, 0, sizeof(in->aux_out_bound));
        memset(in->aux_out_frames, 0, sizeof(in->aux_out_frames));
    }
    return code;
}

/* What the plugin says of the carrier's surround map or ambisonic configuration in one request: its channel mask
 * asked of clap.surround (a map id the host does not know refuses it before asking), its configuration of
 * clap.ambisonic. A plugin without the extension is not asked. Returns the code refusing it, or NULL. */
static const char *request_supported(const struct omx_clap_instance *in, const clap_audio_port_configuration_request_t *r)
{
    uint32_t c;

    if (!r->port_type || !r->port_details)
        return NULL;
    if (!strcmp(r->port_type, CLAP_PORT_SURROUND))
    {
        const uint8_t *map = r->port_details;
        uint64_t mask = 0;

        for (c = 0; c < r->channel_count; c++)
        {
            if (map[c] > CLAP_SURROUND_TSR)
                return CLAP_HOST_CODE_CHANNEL_MAP_UNKNOWN;
            mask |= (uint64_t)1 << map[c];
        }
        if (in->surround && in->surround->is_channel_mask_supported && !in->surround->is_channel_mask_supported(in->plugin, mask))
            return CLAP_HOST_CODE_CHANNEL_MAP_UNKNOWN;
    }
    else if (!strcmp(r->port_type, CLAP_PORT_AMBISONIC))
    {
        if (in->ambisonic && in->ambisonic->is_config_supported && !in->ambisonic->is_config_supported(in->plugin, r->port_details))
            return CLAP_HOST_CODE_AMBISONIC_CONFIG;
    }
    return NULL;
}

/* A config's main port type against a request's: the same id, or both none; and a config naming none (unspecified,
 * as audio-ports.h allows) matches a mono request of one channel or a stereo request of two, the types those widths
 * mean. */
static bool config_type_matches(const char *config_type, const clap_audio_port_configuration_request_t *r)
{
    if (config_type == r->port_type || (config_type && r->port_type && !strcmp(config_type, r->port_type)))
        return true;
    return !config_type && r->port_type
           && ((r->channel_count == 1 && !strcmp(r->port_type, CLAP_PORT_MONO))
               || (r->channel_count == 2 && !strcmp(r->port_type, CLAP_PORT_STEREO)));
}

/* Whether an audio-ports-config is the requests: only main ports asked (index 0, one per side at most), and the config's
 * main ports (has_main_input, has_main_output: a config flags its mains by these, no port of it otherwise) the asked
 * ones, a main on each side asked, of its channels and port type (config_type_matches), and no main on a side not
 * asked. Its auxiliary ports, a side chain beside the main input, never stop a match: they are described after, when
 * the description is read again. A config says nothing of an auxiliary port's width, so a request past the main ports
 * matches none. */
static bool config_matches(const clap_audio_ports_config_t *config, const clap_audio_port_configuration_request_t *reqs, uint32_t n)
{
    const clap_audio_port_configuration_request_t *in_req = NULL, *out_req = NULL;
    uint32_t i;

    for (i = 0; i < n; i++)
    {
        const clap_audio_port_configuration_request_t **side = reqs[i].is_input ? &in_req : &out_req;

        if (reqs[i].port_index != 0 || *side)
            return false;
        *side = &reqs[i];
    }
    if (in_req ? (!config->has_main_input || config->input_port_count < 1u) : config->has_main_input)
        return false;
    if (out_req ? (!config->has_main_output || config->output_port_count < 1u) : config->has_main_output)
        return false;
    if (in_req && (config->main_input_channel_count != in_req->channel_count || !config_type_matches(config->main_input_port_type, in_req)))
        return false;
    if (out_req && (config->main_output_channel_count != out_req->channel_count
                    || !config_type_matches(config->main_output_port_type, out_req)))
        return false;
    return true;
}

int omx_clap_host_request_layout(struct omx_clap_instance *in, const clap_audio_port_configuration_request_t *reqs, uint32_t n,
                                 char why[OMX_CLAP_WHY_MAX])
{
    const clap_plugin_configurable_audio_ports_t *configurable;
    const clap_plugin_audio_ports_config_t *configs;
    int ret = OMX_CLAP_LAYOUT_DECLARED;
    const char *code;
    uint32_t i;

    // both extensions are [main-thread & !active]: an active plugin is asked nothing. No hosting code names a caller's
    // mistake, so `why` is emptied rather than left holding an older answer
    if (!in || in->active || (n && !reqs))
    {
        why_set(why, "");
        return -1;
    }
    for (i = 0; i < n; i++)
    {
        const char *refused = request_supported(in, &reqs[i]);

        if (refused)
        {
            // nothing applied, but the description is read again all the same: a rescan the caller is answering here
            // (omx_clap_host_ports_rescanned, a restart's RESCANNED) must not leave the old one to activate on. Its own
            // refusal's code wins only when the re-read fails
            code = ports_describe_again(in);
            why_set(why, code ? code : refused);
            return -1;
        }
    }
    configurable = in->configurable_audio_ports;
    configs = in->audio_ports_config;
    // configurable-audio-ports first; one that cannot apply the requests, or whose apply refuses them, falls back to the
    // config list
    if (n && configurable && configurable->can_apply_configuration && configurable->apply_configuration
        && configurable->can_apply_configuration(in->plugin, reqs, n) && configurable->apply_configuration(in->plugin, reqs, n))
        ret = 0;
    else if (n && configs && configs->count && configs->get && configs->select)
    {
        const uint32_t count = configs->count(in->plugin);

        for (i = 0; i < count; i++)
        {
            clap_audio_ports_config_t config;

            memset(&config, 0, sizeof(config));
            // a match the plugin refuses to select is passed over for the next one; none selected is DECLARED
            if (!configs->get(in->plugin, i, &config) || !config_matches(&config, reqs, n) || !configs->select(in->plugin, config.id))
                continue;
            ret = 0;
            break;
        }
    }
    code = ports_describe_again(in);
    if (code)
    {
        why_set(why, code);
        return -1;
    }
    return ret;
}

_Static_assert(sizeof(bool) == 1, "a flag member is one byte, read as it lies");

/* a flag member as the plugin wrote it, its byte read rather than its bool, which only 0 and 1 may be */
static unsigned flag_byte(const bool *flag)
{
    unsigned char byte;

    memcpy(&byte, flag, 1);
    return byte;
}

/* At create, on the opening thread: the plugin's org.openmixer.resource-hints.draft/0, whole or refused. No extension,
 * or a get that answers false, states none. A declaration that sets a bit RESOURCE_HINTS has no hint for, or a declared
 * flag whose byte is neither 0 nor 1, is refused with its code and reads as none: no hint of it is trusted. A member whose
 * bit is clear is unknown and kept 0, whatever the plugin left in it. */
static void resource_hints_read(struct omx_clap_instance *in)
{
    const omx_clap_plugin_resource_hints_t *ext = in->plugin->get_extension(in->plugin, OMX_CLAP_EXT_RESOURCE_HINTS_DRAFT);
    omx_resource_hints_t h, *out = &in->resource_hints;
    uint32_t d, i;

    memset(out, 0, sizeof(*out));
    in->resource_hints_why = NULL;
    memset(&h, 0, sizeof(h));
    if (!ext || !ext->get || !ext->get(in->plugin, &h))
        return;
    // refused by the codes of HOSTING_RESOURCE_HINTS_CODES (data/hosting/resource-hints.json)
    d = h.declared;
    if (d & ~RESOURCE_HINTS_ALL)
    {
        in->resource_hints_why = OMX_HOSTING_RESOURCE_HINTS_UNKNOWN_HINT;
        return;
    }
    if (((d & OMX_RESOURCE_HINT_STREAMS_FROM_DISK) && flag_byte(&h.streams_from_disk) > 1u) ||
        ((d & OMX_RESOURCE_HINT_OPENS_SOCKETS) && flag_byte(&h.opens_sockets) > 1u))
    {
        in->resource_hints_why = OMX_HOSTING_RESOURCE_HINTS_WRONG_UNIT;
        return;
    }
    // a flag's byte is 0 or 1 here, which its bool then holds as it is
    out->declared = d;
    for (i = 0; i < OMX_RESOURCE_HINT_COUNT; i++)
        if (d & resource_hints_table[i].bit)
            resource_hint_set(out, i, resource_hint_get(&h, i));
}

/* Create the plugin of `desc`, init it and read its extensions, with no layout judged. Takes over the caller's reference
 * on `bin`: a failure drops it. */
static int instance_create(struct omx_clap_binary *bin, const clap_plugin_descriptor_t *desc, struct omx_clap_instance **out)
{
    struct omx_clap_instance *in = calloc(1, sizeof(*in));

    in->bin = bin;
    in->desc = desc;
    in->main_thread = pthread_self();
    in->host.clap_version = (clap_version_t)CLAP_VERSION_INIT;
    in->host.host_data = in;
    in->host.name = g_config.name;
    in->host.vendor = g_config.vendor;
    in->host.url = g_config.url;
    in->host.version = g_config.version;
    in->host.get_extension = host_get_extension;
    in->host.request_restart = host_request_restart;
    in->host.request_process = host_request_process;
    in->host.request_callback = host_request_callback;
    for (uint32_t i = 0; i < OMX_CLAP_FDS; i++)
        in->fds[i].fd = -1;     // before init, which may register one
    in->plugin = bin->factory->create_plugin(bin->factory, &in->host, desc->id);
    if (!in->plugin || !in->plugin->init(in->plugin))
    {
        if (in->plugin)
            in->plugin->destroy(in->plugin);
        unoffered_forget(in);
        binary_unref(bin);
        free(in);
        *out = NULL;
        return -1;
    }
    in->params = in->plugin->get_extension(in->plugin, CLAP_EXT_PARAMS);
    in->latency = in->plugin->get_extension(in->plugin, CLAP_EXT_LATENCY);
    in->audio_ports = in->plugin->get_extension(in->plugin, CLAP_EXT_AUDIO_PORTS);
    in->note_ports = in->plugin->get_extension(in->plugin, CLAP_EXT_NOTE_PORTS);
    in->state = in->plugin->get_extension(in->plugin, CLAP_EXT_STATE);
    in->timer_support = in->plugin->get_extension(in->plugin, CLAP_EXT_TIMER_SUPPORT);
    in->fd_support = in->plugin->get_extension(in->plugin, CLAP_EXT_POSIX_FD_SUPPORT);
    in->thread_pool = in->plugin->get_extension(in->plugin, CLAP_EXT_THREAD_POOL);
    in->tail = in->plugin->get_extension(in->plugin, CLAP_EXT_TAIL);
    in->preset_load = in->plugin->get_extension(in->plugin, CLAP_EXT_PRESET_LOAD);
    if (!in->preset_load)
        in->preset_load = in->plugin->get_extension(in->plugin, CLAP_EXT_PRESET_LOAD_COMPAT);
    in->surround = in->plugin->get_extension(in->plugin, CLAP_EXT_SURROUND);
    if (!in->surround)
        in->surround = in->plugin->get_extension(in->plugin, CLAP_EXT_SURROUND_COMPAT);
    in->ambisonic = in->plugin->get_extension(in->plugin, CLAP_EXT_AMBISONIC);
    if (!in->ambisonic)
        in->ambisonic = in->plugin->get_extension(in->plugin, CLAP_EXT_AMBISONIC_COMPAT);
    in->configurable_audio_ports = in->plugin->get_extension(in->plugin, CLAP_EXT_CONFIGURABLE_AUDIO_PORTS);
    if (!in->configurable_audio_ports)
        in->configurable_audio_ports = in->plugin->get_extension(in->plugin, CLAP_EXT_CONFIGURABLE_AUDIO_PORTS_COMPAT);
    in->audio_ports_config = in->plugin->get_extension(in->plugin, CLAP_EXT_AUDIO_PORTS_CONFIG);
    if (g_config.track_info)
    {
        in->track_info = in->plugin->get_extension(in->plugin, CLAP_EXT_TRACK_INFO);
        if (!in->track_info)
            in->track_info = in->plugin->get_extension(in->plugin, CLAP_EXT_TRACK_INFO_COMPAT);
    }
    if (g_config.remote_controls)
    {
        in->remote_controls = in->plugin->get_extension(in->plugin, CLAP_EXT_REMOTE_CONTROLS);
        if (!in->remote_controls)
            in->remote_controls = in->plugin->get_extension(in->plugin, CLAP_EXT_REMOTE_CONTROLS_COMPAT);
    }
    resource_hints_read(in);
    *out = in;
    return 0;
}

int omx_clap_host_has_feature(const clap_plugin_descriptor_t *desc, const char *feature)
{
    const char *const *f;

    if (!desc->features)
        return 0;
    for (f = desc->features; *f; f++)
        if (strcmp(*f, feature) == 0)
            return 1;
    return 0;
}

/* params->count(), capped at CLAP_HOST_PARAM_COUNT_MAX: the count is the plugin's own answer, so it sizes no table and
 * bounds no scan past the cap. Every read of the count goes through here; `raw`, when not NULL, gets the uncapped one. */
static uint32_t param_count_of(struct omx_clap_instance *in, uint32_t *raw)
{
    uint32_t n;

    n = in->params ? in->params->count(in->plugin) : 0;
    if (raw)
        *raw = n;
    return n > CLAP_HOST_PARAM_COUNT_MAX ? CLAP_HOST_PARAM_COUNT_MAX : n;
}

/* At open, on the opening thread: the plugin's clap.meters/1 declarations, whole or not at all (docs/meters.md, In
 * process). A meter is drawn, never judged: an extension that cannot be read whole leaves the instance with no meters,
 * and the plugin is opened as before. */
_Static_assert(OMX_CLAP_METER_KIND_OTHER == CLAP_METER_KIND_OTHER && OMX_CLAP_METER_KIND_GAIN_REDUCTION == CLAP_METER_KIND_GAIN_REDUCTION
                   && OMX_CLAP_METER_KIND_LEVEL == CLAP_METER_KIND_LEVEL,
               "the public kinds are clap.meters/1's");

static void meters_read(struct omx_clap_instance *in)
{
    const clap_plugin_meters_t *meters = in->plugin->get_extension(in->plugin, CLAP_EXT_METERS);
    clap_meter_info_t info;
    uint32_t n, i;

    in->meters = NULL;
    in->meter_count = 0;
    if (!meters || !meters->count || !meters->get_info || !meters->get_values)
        return;
    n = meters->count(in->plugin);
    if (n > OMX_CLAP_METER_COUNT_MAX)
        return;
    for (i = 0; i < n; i++)
    {
        struct omx_clap_meter *m = &in->meter[i];

        memset(&info, 0, sizeof(info));
        if (!meters->get_info(in->plugin, i, &info))
            return;
        info.name[sizeof(info.name) - 1] = '\0';
        if (info.name[0] == '\0' || info.channel_count == 0 || info.channel_count > OMX_CLAP_METER_CHANNELS_MAX)
            return;
        m->id = info.id;
        memcpy(m->name, info.name, sizeof(m->name));
        m->kind = info.kind;
        m->flags = info.flags;
        m->channel_count = info.channel_count;
        m->min_value = info.min_value;
        m->max_value = info.max_value;
    }
    in->meter_count = n;
    in->meters = n ? meters : NULL;
}

/* the plugin's org.openmixer.param-contract/1, checked once: the entries, or the reason it is uncontracted, said once in
 * the log unless the plugin simply offers none, or the open has said already that it serves only the first parameters */
static void contract_read(struct omx_clap_instance *in, uint32_t rows, uint32_t reported)
{
    struct pc_contract c;

    memset(&c, 0, sizeof(c));
    if (reported > rows)
        snprintf(c.why, sizeof(c.why), "R1 more parameters than the host serves");
    else
        pc_read(&c, in->plugin, in->params, rows);
    in->contract = c.entry;
    in->contract_count = c.entry ? c.count : 0;
    in->contract_version = c.entry ? c.version : 0;
    snprintf(in->contract_why, sizeof(in->contract_why), "%s", c.entry ? "" : c.why);
    if (!c.entry && strcmp(c.why, "absent") != 0 && reported <= rows)
    {
        char text[CLAP_HOST_LOG_BYTES];

        snprintf(text, sizeof(text), "the parameter contract is not trusted: %s", c.why);
        log_to_ring(in, text);
    }
}

/* R3, on the control thread after the plugin's params.rescan: the contract read again must be the one the open trusted;
 * else the instance is uncontracted from then on, as an open that broke a rule is, and the log says why. A rescan that
 * keeps it takes the fresh entries, whose display text may have changed. An instance that was not contracted stays so. */
static void contract_recheck(struct omx_clap_instance *in)
{
    struct pc_contract c;
    uint32_t rows, reported;
    char text[CLAP_HOST_LOG_BYTES];

    if (!in->contract)
        return;
    memset(&c, 0, sizeof(c));
    rows = param_count_of(in, &reported);
    if (reported > rows)
        snprintf(c.why, sizeof(c.why), "more parameters than the host serves");
    else
        pc_read(&c, in->plugin, in->params, rows);
    if (pc_same(&c, in->contract, in->contract_count, in->contract_version))
    {
        free(in->contract);
        in->contract = c.entry;
        return;
    }
    if (c.entry)
        snprintf(in->contract_why, sizeof(in->contract_why), "R3 params.rescan changed the contract");
    else
        snprintf(in->contract_why, sizeof(in->contract_why), "R3 after params.rescan: %.100s", c.why);
    free(c.entry);
    free(in->contract);
    in->contract = NULL;
    in->contract_count = 0;
    in->contract_version = 0;
    snprintf(text, sizeof(text), "the parameter contract is not trusted: %s", in->contract_why);
    log_to_ring(in, text);
}

/* The one judgment every door shares: descriptor, create and init, extensions, ports. Takes over the caller's
 * reference on `bin`. */
static int open_from(struct omx_clap_binary *bin, const char *id, struct omx_clap_instance **out, char why[OMX_CLAP_WHY_MAX])
{
    const clap_plugin_descriptor_t *desc = NULL;
    const uint32_t n = bin->factory->get_plugin_count(bin->factory);
    struct omx_clap_instance *in;
    const char *refused;
    uint32_t rows, reported, i;

    for (i = 0; i < n; i++)
    {
        const clap_plugin_descriptor_t *d = bin->factory->get_plugin_descriptor(bin->factory, i);

        if (d && (id == NULL || (d->id && strcmp(d->id, id) == 0)))
        {
            desc = d;
            break;
        }
    }
    if (!desc)
    {
        binary_unref(bin);
        why_set(why, CLAP_HOST_CODE_HEADLESS_FAILED);
        return -1;
    }
    // the feature list is a label and refuses nothing (#257): what the plugin declares on its ports decides, in
    // describe_ports, as the LV2 adapter decides by a plugin's ports
    if (instance_create(bin, desc, &in) != 0)
    {
        why_set(why, CLAP_HOST_CODE_HEADLESS_FAILED);
        return -1;
    }
    refused = describe_ports(in);
    if (refused)
    {
        in->plugin->destroy(in->plugin);
        unoffered_forget(in);
        binary_unref(bin);
        free(in);
        why_set(why, refused);
        return -1;
    }
    rows = param_count_of(in, &reported);
    in->shadow = rows ? calloc(rows, sizeof(*in->shadow)) : NULL;
    if (rows && !in->shadow)
    {
        // whole or refused: never an instance whose read-back table is missing
        in->plugin->destroy(in->plugin);
        unoffered_forget(in);
        binary_unref(bin);
        free(in);
        why_set(why, CLAP_HOST_CODE_HEADLESS_FAILED);
        return -1;
    }
    in->shadow_cap = rows;
    if (reported > rows)
    {
        // said once, at the open; every later read is capped the same, silently
        char text[CLAP_HOST_LOG_BYTES];

        snprintf(text, sizeof(text), "the plugin reports %u parameters; the host serves the first %u", reported, rows);
        log_to_ring(in, text);
    }
    meters_read(in);
    contract_read(in, rows, reported);
    *out = in;
    if (why)
        why[0] = '\0';
    return 0;
}

static void *timed_open_run(void *arg)
{
    struct timed_open *t = arg;

    t->rc = omx_clap_host_open(t->path, t->id, &t->out, t->why);
    return NULL;
}


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS: THE BOUNCE AND THE ROLE
************************************************************************************************************************
*/

/* The instance's output event sink, in the stage's place: still counts every event and raises plugin_changed on a value,
 * as the stage's own sink does, keeps the plugin's values and gestures in the instance's ring for the control
 * thread, and its note output in the note output ring. Runs in process() on the audio role or in params.flush on the control thread, never both at once: bounded,
 * no allocation, no lock, no call out. A full ring drops the event and counts it. */
static bool param_out_push(const struct clap_output_events *list, const clap_event_header_t *event)
{
    struct omx_clap_param_out *o = (struct omx_clap_param_out *)list->ctx;
    struct omx_clap_instance *in = (struct omx_clap_instance *)((char *)o - offsetof(struct omx_clap_instance, param_out));
    struct omx_clap_param_event *slot;
    uint32_t kind, head, tail;
    clap_id id;
    double value = 0.0;

    omx_hosted_count(&o->stage->out_events_seen, 1);
    if (event == NULL || event->space_id != CLAP_CORE_EVENT_SPACE_ID)
        return true;
    switch (event->type)
    {
        case CLAP_EVENT_PARAM_VALUE:
            if (event->size < sizeof(clap_event_param_value_t))
                return true;
            atomic_store_explicit(&o->stage->plugin_changed, 1u, memory_order_relaxed);
            kind = OMX_CLAP_PARAM_EVENT_VALUE;
            id = ((const clap_event_param_value_t *)event)->param_id;
            value = ((const clap_event_param_value_t *)event)->value;
            break;
        case CLAP_EVENT_PARAM_GESTURE_BEGIN:
        case CLAP_EVENT_PARAM_GESTURE_END:
            if (event->size < sizeof(clap_event_param_gesture_t))
                return true;
            kind = event->type == CLAP_EVENT_PARAM_GESTURE_BEGIN ? OMX_CLAP_PARAM_EVENT_GESTURE_BEGIN : OMX_CLAP_PARAM_EVENT_GESTURE_END;
            id = ((const clap_event_param_gesture_t *)event)->param_id;
            break;
        default:
            // a note or MIDI event of the plugin's note output: kept for the reader only inside a processing block
            // of the audio role, never from the warm-up or a flush on the control thread
            if (in->note_outputs && atomic_load_explicit(&o->stage->in_cycle, memory_order_relaxed)
                && atomic_load_explicit(&o->stage->state, memory_order_relaxed) == OMX_CLAP_PROCESSING)
                omx_clap_note_out_push(&in->note_out, event, o->stage->steady_time);
            return true;
    }
    tail = atomic_load_explicit(&o->tail, memory_order_relaxed);
    head = atomic_load_explicit(&o->head, memory_order_acquire);
    if (tail - head >= OMX_CLAP_PARAM_OUT_DEPTH)
    {
        omx_hosted_count(&o->dropped, 1);
        return true;
    }
    slot = &o->ring[tail & (OMX_CLAP_PARAM_OUT_DEPTH - 1u)];
    slot->kind = kind;
    slot->id = id;
    slot->value = value;
    atomic_store_explicit(&o->tail, tail + 1u, memory_order_release);
    return true;
}

_Static_assert((OMX_CLAP_PARAM_OUT_DEPTH & (OMX_CLAP_PARAM_OUT_DEPTH - 1u)) == 0, "the output event ring is a power of two");
_Static_assert(sizeof(clap_event_param_mod_t) == sizeof(clap_event_param_value_t)
               && offsetof(clap_event_param_mod_t, amount) == offsetof(clap_event_param_value_t, value)
               && offsetof(clap_event_param_mod_t, cookie) == offsetof(clap_event_param_value_t, cookie)
               && offsetof(clap_event_param_mod_t, param_id) == offsetof(clap_event_param_value_t, param_id)
               && offsetof(clap_event_param_gesture_t, param_id) == offsetof(clap_event_param_value_t, param_id)
               && sizeof(clap_event_param_gesture_t) <= sizeof(clap_event_param_value_t),
               "a modulation and a gesture take a value event's slot");
// a tag is written again two laps of the ring later: by then the block that read it has ended, since one block drains
// at most CLAP_HOST_EVENTS_PER_BLOCK records
_Static_assert(CLAP_HOST_EVENTS_PER_BLOCK <= CLAP_HOST_PARAM_QUEUE_DEPTH, "a block drains at most one lap of the ring");

/* The instance's input event list: the stage's, with a record whose cookie is one of the instance's tags turned, in its
 * slot of the block's array, into what the tag says (a modulation, its amount where the value was, or a gesture) and
 * given the plugin's own cookie back. The first read of an event turns it; a later read finds a plain cookie and leaves
 * it. Runs where the plugin reads its events: process() on the audio role, params.flush on the control thread. Bounded,
 * no allocation, no lock, no call out. */
static uint32_t param_in_size(const struct clap_input_events *list)
{
    const struct omx_clap_param_in *p = (const struct omx_clap_param_in *)list->ctx;

    return p->stage_in.size(&p->stage_in);
}

static const clap_event_header_t *param_in_get(const struct clap_input_events *list, uint32_t index)
{
    const struct omx_clap_param_in *p = (const struct omx_clap_param_in *)list->ctx;
    const clap_event_header_t *h = p->stage_in.get(&p->stage_in, index);
    clap_event_param_value_t *e;
    const struct omx_clap_param_tag *tag;

    if (!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE)
        return h;
    e = (clap_event_param_value_t *)h;
    tag = (const struct omx_clap_param_tag *)e->cookie;
    if (tag < &p->tag[0] || tag >= &p->tag[2 * CLAP_HOST_PARAM_QUEUE_DEPTH])
        return h;
    e->cookie = tag->cookie;
    e->header.type = (uint16_t)tag->type;
    if (tag->type != CLAP_EVENT_PARAM_MOD)
        e->header.size = sizeof(clap_event_param_gesture_t);
    return h;
}

/* Blocks of max_block floats in one mapping with a guard page at each end, as many as the port description needs: one
 * silence block every unbound auxiliary input channel reads, one scratch block per auxiliary output channel and per main
 * output channel past the two legs, one block per main input channel past the two legs, then the stage's four, the last
 * of them flush against the guard page above, so the first sample written past a block faults here, in this mapping,
 * and never lands in a foreign buffer. A main port wider than the legs binds the wide bounce over the legs and those
 * blocks (omx_clap_bind_wide), which omx_clap_host_run_ports carries. A two-leg call on such an instance feeds the legs
 * only: past them the plugin reads the main input blocks as the last omx_clap_host_run_ports left them (silence until
 * one fills them) and writes its output blocks, which nothing reads. */
static int take_bounce(struct omx_clap_instance *in, uint32_t max_block)
{
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    const uint32_t aux_out = in->aux_outputs ? in->aux_out_base[in->aux_outputs - 1] + in->aux_out[in->aux_outputs - 1].channels : 0;
    const uint32_t main_out = in->main_out.channels > 2 ? in->main_out.channels - 2 : 0;
    const uint32_t main_in = in->main_in.channels > 2 ? in->main_in.channels - 2 : 0;
    const uint32_t blocks = 1 + aux_out + main_out + main_in + 4;
    const size_t used = (size_t)blocks * max_block * sizeof(float);
    const size_t body = (used + page - 1) / page * page;
    const size_t length = body + 2 * page;
    uint8_t *map = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    struct omx_hosted_bounce bounce;
    float *wide_in[CLAP_HOST_PORT_CHANNELS], *wide_out[CLAP_HOST_PORT_CHANNELS];
    float **scratch;
    uint32_t k;
    float *b;

    if (map == MAP_FAILED)
        return -1;
    mprotect(map, page, PROT_NONE);
    mprotect(map + page + body, page, PROT_NONE);
    in->bounce_map = (float *)map;
    in->bounce_map_len = length;
    in->bounce_blocks = blocks;
    b = (float *)(map + page + (body - used));
    in->rec_cap = CLAP_HOST_PARAM_QUEUE_DEPTH;
    in->recs = calloc(in->rec_cap, sizeof(*in->recs));
    in->max_block = max_block;
    bounce.in_l = b + (size_t)(blocks - 4) * max_block;
    bounce.in_r = b + (size_t)(blocks - 3) * max_block;
    bounce.out_l = b + (size_t)(blocks - 2) * max_block;
    bounce.out_r = b + (size_t)(blocks - 1) * max_block;
    bounce.max_block = max_block;
    if (omx_clap_stage_init(&in->stage, &bounce, in->recs, in->rec_cap) != 0)
        return -1;
    // the plugin's output events go to the instance's sink, which keeps its values and gestures
    in->param_out.stage = &in->stage;
    in->stage.out_events.ctx = &in->param_out;
    in->stage.out_events.try_push = param_out_push;
    // and the plugin reads its input events through the instance's list, which turns a tagged record into its kind
    in->param_in.stage_in = in->stage.in_events;
    in->stage.in_events.ctx = &in->param_in;
    in->stage.in_events.size = param_in_size;
    in->stage.in_events.get = param_in_get;
    in->stage.h.guards = (g_config.clamp ? OMX_HOSTED_GUARD_CLAMP : 0u) | (g_config.nonfinite ? OMX_HOSTED_GUARD_NONFINITE : 0u);
    in->stage.note_inputs = in->note_inputs;
    in->stage.note_dialect = in->note_dialect;
    in->stage.note_dialects = in->note_dialects;
    // the stage keeps the scratch blocks' addresses: the list handing them over lives for this call only
    scratch = aux_out ? malloc(aux_out * sizeof(*scratch)) : NULL;
    if (aux_out && !scratch)
        return -1;
    for (k = 0; k < aux_out; k++)
        scratch[k] = b + (size_t)(1 + k) * max_block;
    k = (uint32_t)omx_clap_bind_aux(&in->stage, scratch, in->aux_outputs, in->aux_out);
    free(scratch);
    if (k != 0)
        return -1;
    for (k = 0; k < in->aux_outputs; k++)
        omx_clap_set_aux_out(&in->stage, k, in->aux_out[k].channels, &in->aux_out_bound[in->aux_out_base[k]]);
    // the main channels past the legs: the inputs read the silence, each output writes a scratch block of its own
    for (k = 2; k < in->main_in.channels; k++)
        in->stage.in_ptrs[k] = b;
    for (k = 2; k < in->main_out.channels; k++)
        in->stage.out_ptrs[k] = b + (size_t)(1 + aux_out + k - 2) * max_block;
    // wider than the legs: the wide bounce, the legs then a block of its own for each main channel past them
    if (main_in || main_out)
    {
        for (k = 0; k < in->main_in.channels; k++)
            wide_in[k] = k == 0 ? bounce.in_l : k == 1 ? bounce.in_r : b + (size_t)(1 + aux_out + main_out + k - 2) * max_block;
        for (k = 0; k < in->main_out.channels; k++)
            wide_out[k] = k == 0 ? bounce.out_l : k == 1 ? bounce.out_r : in->stage.out_ptrs[k];
        if (omx_clap_bind_wide(&in->stage, wide_in, in->main_in.channels, wide_out, in->main_out.channels) != 0)
            return -1;
    }
    return omx_clap_bind_aux_in(&in->aux_list, b, in->aux_inputs, in->aux_in, in->aux_in_bound, in->aux_in_base);
}

static void drop_bounce(struct omx_clap_instance *in)
{
    if (in->bounce_map)
        munmap(in->bounce_map, in->bounce_map_len);
    in->bounce_map = NULL;
    free(in->recs);
    in->recs = NULL;
}

/* the ports, then the note ids over the stage's sink: every id free, held OMX_CLAP_NOTE_ID_HOLD_MS at the rate */
static int bind_stage(struct omx_clap_instance *in)
{
    in->stage.passthrough = in->passthrough;
    if (omx_clap_bind_ports_aux(&in->stage, in->plugin, in->in_channels, in->channels, &in->aux_list) != 0)
        return -1;
    omx_clap_note_ids_bind(&in->note_ids, &in->stage, (uint32_t)lround(in->rate * OMX_CLAP_NOTE_ID_HOLD_MS / 1000.0));
    return 0;
}

static void publish_latency(struct omx_clap_instance *in)
{
    // a pass-through tool's latency applies to its notes alone: the audio it does not touch has none
    omx_clap_publish_latency(&in->stage, in->latency && !in->passthrough ? in->latency->get(in->plugin) : 0u);
}

/* no cycle came to stop the stage: the control thread stops it in the cycle's place */
static int wait_stopped(struct omx_clap_instance *in)
{
    uint32_t expected = OMX_CLAP_PROCESSING;
    unsigned waited = 0;

    if (!atomic_compare_exchange_strong(&in->stage.state, &expected, OMX_CLAP_STOPPING))
    {
        expected = OMX_CLAP_ARMED;
        atomic_compare_exchange_strong(&in->stage.state, &expected, OMX_CLAP_STOPPED);
    }

    while (atomic_load(&in->stage.state) == OMX_CLAP_STOPPING)
    {
        if (waited >= CLAP_HOST_ROLE_TIMEOUT_US)
        {
            struct omx_clap_role role;

            omx_clap_host_take_role(in, &role);
            if (role.state == OMX_CLAP_STOPPING && in->plugin->stop_processing)
                in->plugin->stop_processing(in->plugin);
            omx_clap_host_release_role(in, &role, OMX_CLAP_STOPPED);
            return 0;
        }
        usleep(CLAP_HOST_ROLE_POLL_US);
        waited += CLAP_HOST_ROLE_POLL_US;
    }
    return 0;
}


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS: PARAMETERS AND STATE
************************************************************************************************************************
*/

static int is_row(const clap_param_info_t *info)
{
    return !(info->flags & (CLAP_PARAM_IS_HIDDEN | CLAP_PARAM_IS_READONLY | CLAP_PARAM_IS_BYPASS));
}

static int info_of(struct omx_clap_instance *in, clap_id id, clap_param_info_t *info)
{
    uint32_t n, i;

    if (!in->params)
        return -1;
    n = param_count_of(in, NULL);
    for (i = 0; i < n; i++)
    {
        memset(info, 0, sizeof(*info));
        if (in->params->get_info(in->plugin, i, info) && info->id == id)
            return 0;
    }
    return -1;
}

static void row_of(const clap_param_info_t *info, struct omx_clap_param_row *row)
{
    memset(row, 0, sizeof(*row));
    row->id = info->id;
    // the plugin's own buffer, which it need not have terminated: read no further than its size
    snprintf(row->name, sizeof(row->name), "%.*s", (int)sizeof(info->name), info->name);
    row->min = info->min_value;
    row->max = info->max_value;
    row->def = info->default_value;
    row->stepped = (info->flags & CLAP_PARAM_IS_STEPPED) != 0;
    row->enumerated = (info->flags & CLAP_PARAM_IS_ENUM) != 0;
    row->cookie = info->cookie;
}

static struct omx_clap_shadow *shadow_of(struct omx_clap_instance *in, clap_id id, int make)
{
    struct omx_clap_shadow *s;
    uint32_t i;

    for (i = 0; i < in->shadow_n; i++)
        if (in->shadow[i].id == id)
            return &in->shadow[i];
    if (!make || in->shadow_n >= in->shadow_cap)
        return NULL;
    s = &in->shadow[in->shadow_n++];
    memset(s, 0, sizeof(*s));
    s->id = id;
    return s;
}

static int64_t ostream_write(const clap_ostream_t *stream, const void *data, uint64_t size)
{
    struct bounded_stream *b = stream->ctx;

    if (b->len + size > b->cap)
    {
        b->overflow = 1;
        return -1;
    }
    memcpy(b->buf + b->len, data, size);
    b->len += size;
    return (int64_t)size;
}

static int64_t istream_read(const clap_istream_t *stream, void *data, uint64_t size)
{
    struct bounded_stream *b = stream->ctx;
    const size_t left = b->len - b->pos;
    const size_t n = size < left ? (size_t)size : left;

    memcpy(data, b->in + b->pos, n);
    b->pos += n;
    return (int64_t)n;
}


/*
************************************************************************************************************************
*           GLOBAL FUNCTIONS: CONFIGURATION
************************************************************************************************************************
*/

void omx_clap_host_config_default_sized(struct omx_clap_host_config *config, uint32_t caller_size)
{
    static const struct omx_clap_host_config defaults =
    {
        OMX_CLAP_CORE_ABI, sizeof(struct omx_clap_host_config), 1, 1, 1, 0, 0, "omx-clap-core", "Pau Aliagas", "https://github.com/FreeMixer/omx-clap-core", "0", 0, 0, 2, 0
    };
    uint32_t n;

    // too short to even carry a size that names a host: refused whole, as omx_clap_host_configure refuses it (#301)
    if (caller_size < offsetof(struct omx_clap_host_config, version) + sizeof(config->version))
        return;
    n = caller_size < sizeof(defaults) ? caller_size : sizeof(defaults);
    memcpy(config, &defaults, n);
    config->size = n;
    if (n >= offsetof(struct omx_clap_host_config, idle_hold_us) + sizeof(config->idle_hold_us))
        config->idle_hold_us = idle_hold_target_default();
}

void omx_clap_host_config_default(struct omx_clap_host_config *config)
{
    omx_clap_host_config_default_sized(config, sizeof(struct omx_clap_host_config));
}

int omx_clap_host_configure(const struct omx_clap_host_config *config)
{
    struct omx_clap_host_config merged;

    if (!config || config->abi < OMX_CLAP_CORE_ABI_OLDEST || config->abi > OMX_CLAP_CORE_ABI || config->size < offsetof(struct omx_clap_host_config, version) + sizeof(config->version))
        return -1;
    if (g_configured || g_sealed)
        return -1;
    // a caller built against an older header has fewer fields: the rest are the defaults'
    omx_clap_host_config_default(&merged);
    memcpy(&merged, config, config->size < sizeof(merged) ? config->size : sizeof(merged));
    merged.size = sizeof(merged);
    if (!merged.name || !merged.vendor || !merged.url || !merged.version)
        return -1;
    g_config = merged;
    g_configured = 1;
    idle_hold_configure(merged.idle_hold_us);
    return 0;
}

int omx_clap_host_track_info_set(struct omx_clap_instance *in, const char *name, const clap_color_t *color, uint64_t flags)
{
    if (!g_config.track_info)
        return -1;
    memset(&in->track, 0, sizeof(in->track));
    in->track.flags = flags & (CLAP_TRACK_INFO_IS_FOR_RETURN_TRACK | CLAP_TRACK_INFO_IS_FOR_BUS | CLAP_TRACK_INFO_IS_FOR_MASTER);
    if (name && *name)
    {
        snprintf(in->track.name, sizeof(in->track.name), "%s", name);
        in->track.flags |= CLAP_TRACK_INFO_HAS_TRACK_NAME;
    }
    if (color)
    {
        in->track.color = *color;
        in->track.flags |= CLAP_TRACK_INFO_HAS_TRACK_COLOR;
    }
    in->track_set = 1;
    if (in->track_info)
        in->track_info->changed(in->plugin);
    return 0;
}

uint32_t omx_clap_host_meter_count(const struct omx_clap_instance *in)
{
    return in ? in->meter_count : 0;
}

int omx_clap_host_meter_info(const struct omx_clap_instance *in, uint32_t index, struct omx_clap_meter *meter)
{
    if (!in || !meter || index >= in->meter_count)
        return -1;
    *meter = in->meter[index];
    return 0;
}

/* At activate, on the control thread: the plugin's clap.voice-info, which it answers only while active. A declaration
 * outside 1 <= count <= capacity is no declaration: a host prices an instrument at its capacity, and an answer that
 * breaks the extension's own rule names no ceiling. */
static void voice_info_ask(struct omx_clap_instance *in)
{
    const clap_plugin_voice_info_t *ext = in->plugin->get_extension(in->plugin, CLAP_EXT_VOICE_INFO);
    clap_voice_info_t info;

    in->voice_info_read = 0;
    in->voice_count = 0;
    in->voice_capacity = 0;
    in->voice_flags = 0;
    if (!ext || !ext->get)
        return;
    memset(&info, 0, sizeof(info));
    if (!ext->get(in->plugin, &info) || info.voice_count < 1 || info.voice_count > info.voice_capacity)
        return;
    in->voice_count = info.voice_count;
    in->voice_capacity = info.voice_capacity;
    in->voice_flags = info.flags;
    in->voice_info_read = 1;
}

int omx_clap_host_voice_info(const struct omx_clap_instance *in, uint32_t *count, uint32_t *capacity, uint64_t *flags)
{
    const int read = in && in->voice_info_read;

    if (count)
        *count = read ? in->voice_count : 0;
    if (capacity)
        *capacity = read ? in->voice_capacity : 0;
    if (flags)
        *flags = read ? in->voice_flags : 0;
    return read;
}

static const clap_plugin_note_name_t *note_name_of(struct omx_clap_instance *in)
{
    const clap_plugin_note_name_t *ext = in ? in->plugin->get_extension(in->plugin, CLAP_EXT_NOTE_NAME) : NULL;

    return ext && ext->count && ext->get ? ext : NULL;
}

uint32_t omx_clap_host_note_name_count(struct omx_clap_instance *in)
{
    const clap_plugin_note_name_t *ext = note_name_of(in);

    return ext ? ext->count(in->plugin) : 0;
}

int omx_clap_host_note_name(struct omx_clap_instance *in, uint32_t index, clap_note_name_t *name)
{
    const clap_plugin_note_name_t *ext = note_name_of(in);
    clap_note_name_t got;

    if (!ext || !name || index >= ext->count(in->plugin))
        return -1;
    memset(&got, 0, sizeof(got));
    if (!ext->get(in->plugin, index, &got))
        return -1;
    got.name[sizeof(got.name) - 1] = '\0';      // a plugin that fills every byte still reads as a string
    *name = got;
    return 0;
}

int omx_clap_host_note_names_changed(struct omx_clap_instance *in)
{
    return in && atomic_exchange_explicit(&in->note_names_changed, 0u, memory_order_relaxed) != 0;
}

uint32_t omx_clap_host_note_ports_rescanned(struct omx_clap_instance *in)
{
    return in ? atomic_exchange_explicit(&in->note_ports_rescanned, 0u, memory_order_relaxed) : 0;
}

int omx_clap_host_param_contract(struct omx_clap_instance *in, uint32_t *version, const char **why)
{
    if (version)
        *version = in->contract_version;
    if (why)
        *why = in->contract ? NULL : in->contract_why;
    return in->contract != NULL;
}

int omx_clap_host_param_meaning(struct omx_clap_instance *in, clap_id id, struct omx_clap_param_meaning *meaning)
{
    const struct pc_contract c = { in->contract, in->contract_count, in->contract_version, "" };
    const omx_clap_param_contract_entry_t *entry = pc_entry(&c, id);
    clap_param_info_t info;

    if (!entry || info_of(in, id, &info) != 0)
        return -1;
    memset(meaning, 0, sizeof(*meaning));
    meaning->id = id;
    memcpy(meaning->symbol, entry->symbol, sizeof(meaning->symbol));
    meaning->unit = entry->unit;
    meaning->scale = entry->scale;
    meaning->step = entry->step;
    meaning->min = info.min_value;
    meaning->max = info.max_value;
    meaning->def = info.default_value;
    meaning->flags = info.flags;
    return 0;
}

int omx_clap_host_resource_hints(const struct omx_clap_instance *in, omx_resource_hints_t *hints, const char **why)
{
    if (hints)
    {
        if (in)
            *hints = in->resource_hints;
        else
            memset(hints, 0, sizeof(*hints));
    }
    if (why)
        *why = in ? in->resource_hints_why : NULL;
    if (!in)
        return 0;
    if (in->resource_hints_why)
        return -1;
    return in->resource_hints.declared != 0;
}

const char *omx_clap_resource_hint_word(uint32_t index)
{
    return index < OMX_RESOURCE_HINT_COUNT ? resource_hints_table[index].word : NULL;
}

const char *omx_clap_resource_hint_unit(uint32_t index)
{
    return index < OMX_RESOURCE_HINT_COUNT ? resource_hints_table[index].unit : NULL;
}

int omx_clap_resource_hint_figure(const omx_resource_hints_t *hints, uint32_t index, uint64_t *figure)
{
    if (figure)
        *figure = 0;
    if (!hints || index >= OMX_RESOURCE_HINT_COUNT)
        return -1;
    if (!(hints->declared & resource_hints_table[index].bit))
        return 0;
    if (figure)
        *figure = resource_hint_get(hints, index);
    return 1;
}

const char *omx_clap_param_unit_name(uint32_t unit)
{
    return pc_unit_name(unit);
}

const char *omx_clap_param_scale_name(uint32_t scale)
{
    return pc_scale_name(scale);
}

int omx_clap_param_number(double value, char *out, size_t size)
{
    return pc_number(value, out, size);
}

uint32_t omx_clap_host_meter_values(struct omx_clap_instance *in, clap_id id, double *values, uint32_t capacity)
{
    double read[OMX_CLAP_METER_CHANNELS_MAX];
    uint32_t i, n, channels = 0;

    if (!in || !in->meters || !values || capacity == 0)
        return 0;
    for (i = 0; i < in->meter_count; i++)
        if (in->meter[i].id == id)
        {
            channels = in->meter[i].channel_count;
            break;
        }
    if (channels == 0)
        return 0;
    // the plugin writes into the library's own array, never past the meter's declared channels into the caller's
    n = in->meters->get_values(in->plugin, id, read, channels);
    if (n > channels)
        n = channels;
    if (n > capacity)
        n = capacity;
    memcpy(values, read, n * sizeof(*values));
    return n;
}

int omx_clap_host_offer_gui(const clap_host_gui_t *gui)
{
    if (g_sealed)
        return -1;
    g_gui = gui;
    return 0;
}

int omx_clap_host_remote_controls_changed(struct omx_clap_instance *in)
{
    return atomic_exchange(&in->remote_controls_changed, 0) != 0;
}

uint32_t omx_clap_core_version(void)
{
    return CORE_VERSION;
}


/*
************************************************************************************************************************
*           GLOBAL FUNCTIONS: BINARIES, OPEN AND CLOSE
************************************************************************************************************************
*/

/*
************************************************************************************************************************
*           LOCAL FUNCTIONS: PRESET DISCOVERY
************************************************************************************************************************
*/

/* the filetypes and locations one provider declares at init, kept for the crawl */
#define PRESET_FILETYPES    16u
#define PRESET_LOCATIONS    16u
#define PRESET_EXT_MAX      32u
#define PRESET_NAME_MAX     256u
#define PRESET_KEY_MAX      1024u
#define PRESET_TEXT_MAX     512u

struct preset_location
{
    uint32_t flags, kind;
    char *location;             // NULL for a PLUGIN location
};

/* one crawl: the indexer, its receiver, the preset being received and the caller's callback */
struct preset_crawl
{
    clap_preset_discovery_indexer_t indexer;
    clap_preset_discovery_metadata_receiver_t receiver;
    const char *provider_id;
    char extensions[PRESET_FILETYPES][PRESET_EXT_MAX];
    uint32_t n_extensions;
    int any_extension;          // a filetype with no extension: every file is the provider's to read
    struct preset_location locations[PRESET_LOCATIONS];
    uint32_t n_locations;

    // the preset under way: from begin_preset to the next one or the end of get_metadata
    int open;
    uint32_t kind, location_flags;
    const char *location;
    char name[PRESET_NAME_MAX], key[PRESET_KEY_MAX], description[PRESET_TEXT_MAX];
    int has_key, has_flags;
    uint32_t flags;
    char plugin_ids[OMX_CLAP_PRESET_PLUGINS][CLAP_NAME_SIZE * 4];
    uint32_t n_plugin_ids;

    omx_clap_preset_fn fn;
    void *ctx;
    int listed, stopped, files;
};

static struct preset_crawl *crawl_of_indexer(const clap_preset_discovery_indexer_t *indexer)
{
    return (struct preset_crawl *)indexer->indexer_data;
}

static struct preset_crawl *crawl_of_receiver(const clap_preset_discovery_metadata_receiver_t *receiver)
{
    return (struct preset_crawl *)receiver->receiver_data;
}

static void copy_text(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src ? src : "");
}

static bool indexer_declare_filetype(const clap_preset_discovery_indexer_t *indexer, const clap_preset_discovery_filetype_t *filetype)
{
    struct preset_crawl *c = crawl_of_indexer(indexer);

    if (!filetype)
        return false;
    if (!filetype->file_extension || !filetype->file_extension[0])
    {
        c->any_extension = 1;
        return true;
    }
    if (c->n_extensions >= PRESET_FILETYPES || strlen(filetype->file_extension) >= PRESET_EXT_MAX)
        return false;
    copy_text(c->extensions[c->n_extensions++], PRESET_EXT_MAX, filetype->file_extension[0] == '.' ? filetype->file_extension + 1
                                                                                                   : filetype->file_extension);
    return true;
}

static bool indexer_declare_location(const clap_preset_discovery_indexer_t *indexer, const clap_preset_discovery_location_t *location)
{
    struct preset_crawl *c = crawl_of_indexer(indexer);
    struct preset_location *l;

    if (!location || c->n_locations >= PRESET_LOCATIONS)
        return false;
    if (location->kind != CLAP_PRESET_DISCOVERY_LOCATION_FILE && location->kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN)
        return false;
    if (location->kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE && (!location->location || !location->location[0]))
        return false;
    l = &c->locations[c->n_locations];
    l->flags = location->flags;
    l->kind = location->kind;
    l->location = location->kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE ? strdup(location->location) : NULL;
    if (location->kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE && !l->location)
        return false;
    c->n_locations++;
    return true;
}

/* sound packs are not listed: a preset names its own */
static bool indexer_declare_soundpack(const clap_preset_discovery_indexer_t *indexer, const clap_preset_discovery_soundpack_t *soundpack)
{
    (void)indexer;
    return soundpack != NULL;
}

static const void *indexer_get_extension(const clap_preset_discovery_indexer_t *indexer, const char *id)
{
    (void)indexer;
    (void)id;
    return NULL;
}

/* the preset under way goes to the caller: its name, or for a file that holds one preset and names none, the file's */
static void preset_flush(struct preset_crawl *c)
{
    struct omx_clap_preset p;
    const char *base;
    uint32_t i;

    if (!c->open)
        return;
    c->open = 0;
    if (c->stopped)
        return;
    memset(&p, 0, sizeof(p));
    if (!c->name[0] && c->location)
    {
        base = strrchr(c->location, '/');
        copy_text(c->name, sizeof(c->name), base ? base + 1 : c->location);
    }
    p.provider_id = c->provider_id;
    p.name = c->name;
    p.location_kind = c->kind;
    p.location = c->location;
    p.load_key = c->has_key ? c->key : NULL;
    p.flags = c->has_flags ? c->flags : c->location_flags;
    p.description = c->description;
    p.plugin_count = c->n_plugin_ids;
    for (i = 0; i < c->n_plugin_ids; i++)
        p.plugin_ids[i] = c->plugin_ids[i];
    c->listed++;
    if (c->fn && c->fn(c->ctx, &p) != 0)
        c->stopped = 1;
}

static void receiver_on_error(const clap_preset_discovery_metadata_receiver_t *receiver, int32_t os_error, const char *message)
{
    (void)receiver;
    (void)os_error;
    (void)message;      // a file the provider cannot read lists no preset; the crawl goes on
}

static bool receiver_begin_preset(const clap_preset_discovery_metadata_receiver_t *receiver, const char *name, const char *load_key)
{
    struct preset_crawl *c = crawl_of_receiver(receiver);

    preset_flush(c);
    if (c->stopped || c->listed >= (int)OMX_CLAP_PRESETS_MAX)
        return false;
    c->open = 1;
    copy_text(c->name, sizeof(c->name), name);
    c->has_key = load_key != NULL;
    copy_text(c->key, sizeof(c->key), load_key);
    c->description[0] = '\0';
    c->has_flags = 0;
    c->flags = 0;
    c->n_plugin_ids = 0;
    return true;
}

/* only CLAP plugin ids: a preset for another format's plugin is no preset this host loads */
static void receiver_add_plugin_id(const clap_preset_discovery_metadata_receiver_t *receiver, const clap_universal_plugin_id_t *id)
{
    struct preset_crawl *c = crawl_of_receiver(receiver);

    if (!c->open || !id || !id->abi || strcmp(id->abi, "clap") != 0 || !id->id || c->n_plugin_ids >= OMX_CLAP_PRESET_PLUGINS)
        return;
    copy_text(c->plugin_ids[c->n_plugin_ids++], sizeof(c->plugin_ids[0]), id->id);
}

static void receiver_set_soundpack_id(const clap_preset_discovery_metadata_receiver_t *receiver, const char *id)
{
    (void)receiver;
    (void)id;
}

static void receiver_set_flags(const clap_preset_discovery_metadata_receiver_t *receiver, uint32_t flags)
{
    struct preset_crawl *c = crawl_of_receiver(receiver);

    if (!c->open)
        return;
    c->flags = flags;
    c->has_flags = 1;
}

static void receiver_add_creator(const clap_preset_discovery_metadata_receiver_t *receiver, const char *creator)
{
    (void)receiver;
    (void)creator;
}

static void receiver_set_description(const clap_preset_discovery_metadata_receiver_t *receiver, const char *description)
{
    struct preset_crawl *c = crawl_of_receiver(receiver);

    if (c->open)
        copy_text(c->description, sizeof(c->description), description);
}

static void receiver_set_timestamps(const clap_preset_discovery_metadata_receiver_t *receiver, clap_timestamp created, clap_timestamp modified)
{
    (void)receiver;
    (void)created;
    (void)modified;
}

static void receiver_add_feature(const clap_preset_discovery_metadata_receiver_t *receiver, const char *feature)
{
    (void)receiver;
    (void)feature;
}

static void receiver_add_extra_info(const clap_preset_discovery_metadata_receiver_t *receiver, const char *key, const char *value)
{
    (void)receiver;
    (void)key;
    (void)value;
}

/* one location's presets through the provider; the location's path, or NULL for a PLUGIN location */
static void crawl_metadata(struct preset_crawl *c, const clap_preset_discovery_provider_t *provider, uint32_t kind, uint32_t flags,
                           const char *location)
{
    if (c->stopped)
        return;
    c->kind = kind;
    c->location_flags = flags;
    c->location = location;
    c->open = 0;
    provider->get_metadata(provider, kind, location, &c->receiver);
    preset_flush(c);
}

static int crawl_wants(const struct preset_crawl *c, const char *name)
{
    const char *dot = strrchr(name, '.');
    uint32_t i;

    if (c->any_extension)
        return 1;
    if (!dot)
        return 0;
    for (i = 0; i < c->n_extensions; i++)
        if (!strcmp(dot + 1, c->extensions[i]))
            return 1;
    return 0;
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* a FILE location: a file is read as it is; a directory is walked in name order, OMX_CLAP_PRESET_DEPTH deep at most, its
 * files of a declared filetype read; a symbolic link to a directory inside it is not followed, so a loop cannot hold
 * the crawl */
static void crawl_path(struct preset_crawl *c, const clap_preset_discovery_provider_t *provider, uint32_t flags, const char *path,
                       uint32_t depth)
{
    struct stat st;
    DIR *dir;
    struct dirent *e;
    char **names = NULL, **grown;
    size_t n = 0, cap = 0, i;

    if (c->stopped || lstat(path, &st) != 0)
        return;
    if (S_ISLNK(st.st_mode) && (stat(path, &st) != 0 || (S_ISDIR(st.st_mode) && depth > 0)))
        return;     // the declared location itself may be a link; one met inside the walk is not followed
    if (S_ISREG(st.st_mode))
    {
        if (c->files++ < (int)OMX_CLAP_PRESET_FILES_MAX)
            crawl_metadata(c, provider, CLAP_PRESET_DISCOVERY_LOCATION_FILE, flags, path);
        return;
    }
    if (!S_ISDIR(st.st_mode) || depth >= OMX_CLAP_PRESET_DEPTH || !(dir = opendir(path)))
        return;
    while ((e = readdir(dir)))
    {
        char full[PATH_MAX];

        if (e->d_name[0] == '.')
            continue;
        if ((size_t)snprintf(full, sizeof(full), "%s/%s", path, e->d_name) >= sizeof(full))
            continue;
        if (n == cap)
        {
            cap = cap ? cap * 2 : 32;
            grown = realloc(names, cap * sizeof(*names));
            if (!grown)
                break;
            names = grown;
        }
        if (!(names[n] = strdup(full)))
            break;
        n++;
    }
    closedir(dir);
    if (n)
        qsort(names, n, sizeof(*names), compare_names);
    for (i = 0; i < n; i++)
    {
        if (lstat(names[i], &st) == 0 && (S_ISDIR(st.st_mode) || crawl_wants(c, names[i])))
            crawl_path(c, provider, flags, names[i], depth + 1);
        free(names[i]);
    }
    free(names);
}

/* every preset of one provider: init declares its filetypes and locations, then each location is crawled */
static void crawl_provider(struct preset_crawl *c, const clap_preset_discovery_factory_t *factory, const char *id)
{
    const clap_preset_discovery_provider_t *provider = factory->create(factory, &c->indexer, id);
    uint32_t i;

    if (!provider)
        return;
    c->provider_id = id;
    if (provider->init && provider->init(provider) && provider->get_metadata)
        for (i = 0; i < c->n_locations && !c->stopped; i++)
        {
            if (c->locations[i].kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN)
                crawl_metadata(c, provider, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, c->locations[i].flags, NULL);
            else
                crawl_path(c, provider, c->locations[i].flags, c->locations[i].location, 0);
        }
    if (provider->destroy)
        provider->destroy(provider);
    for (i = 0; i < c->n_locations; i++)
        free(c->locations[i].location);
    c->n_locations = 0;
    c->n_extensions = 0;
    c->any_extension = 0;
}

static int presets_of(const clap_plugin_entry_t *entry, omx_clap_preset_fn fn, void *ctx)
{
    const clap_preset_discovery_factory_t *factory;
    struct preset_crawl *c;
    uint32_t i, count;
    int listed;

    factory = entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID);
    if (!factory)
        factory = entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT);
    // a plugin that answers every factory id with its plugin factory (SpectMorph 0.6.1 does) hands back a table of the
    // same shape: its create makes a plugin, not a provider, and calling that plugin's init with an indexer crashes it
    if (!factory || factory == entry->get_factory(CLAP_PLUGIN_FACTORY_ID) || !factory->count || !factory->get_descriptor || !factory->create)
        return -1;
    c = calloc(1, sizeof(*c));
    if (!c)
        return -1;
    c->indexer = (clap_preset_discovery_indexer_t){ CLAP_VERSION_INIT, g_config.name, g_config.vendor, g_config.url,
                                                    g_config.version, c, indexer_declare_filetype, indexer_declare_location,
                                                    indexer_declare_soundpack, indexer_get_extension };
    c->receiver = (clap_preset_discovery_metadata_receiver_t){ c, receiver_on_error, receiver_begin_preset, receiver_add_plugin_id,
                                                               receiver_set_soundpack_id, receiver_set_flags, receiver_add_creator,
                                                               receiver_set_description, receiver_set_timestamps,
                                                               receiver_add_feature, receiver_add_extra_info };
    c->fn = fn;
    c->ctx = ctx;
    count = factory->count(factory);
    for (i = 0; i < count && !c->stopped; i++)
    {
        const clap_preset_discovery_provider_descriptor_t *desc = factory->get_descriptor(factory, i);

        if (desc && desc->id)
            crawl_provider(c, factory, desc->id);
    }
    listed = c->listed;
    free(c);
    return listed;
}

struct omx_clap_binary *omx_clap_host_binary_open(const char *path, char *reason, size_t reason_size)
{
    return binary_ref(path, reason, reason_size);
}

void omx_clap_host_binary_close(struct omx_clap_binary *binary)
{
    binary_unref(binary);
}

int omx_clap_host_presets(struct omx_clap_binary *binary, omx_clap_preset_fn fn, void *ctx)
{
    return binary && binary->entry ? presets_of(binary->entry, fn, ctx) : -1;
}

int omx_clap_host_entry_presets(const clap_plugin_entry_t *entry, omx_clap_preset_fn fn, void *ctx)
{
    struct omx_clap_binary *binary;
    char reason[OMX_CLAP_WHY_MAX];
    int listed;

    if (!entry)
        return -1;
    binary = binary_ref_entry(entry, NULL, reason, sizeof(reason));
    if (!binary)
        return -1;
    listed = presets_of(entry, fn, ctx);
    binary_unref(binary);
    return listed;
}

uint32_t omx_clap_host_binary_count(const struct omx_clap_binary *binary)
{
    return binary->factory->get_plugin_count(binary->factory);
}

const clap_plugin_descriptor_t *omx_clap_host_binary_descriptor(const struct omx_clap_binary *binary, uint32_t index)
{
    return binary->factory->get_plugin_descriptor(binary->factory, index);
}

uint32_t omx_clap_host_binaries_open(void)
{
    const struct omx_clap_binary *binary;
    uint32_t n = 0;

    for (binary = g_binaries; binary; binary = binary->next)
        n++;
    return n;
}

/* the instance holds a reference of its own on the binary until omx_clap_host_close */
int omx_clap_host_create(struct omx_clap_binary *binary, const clap_plugin_descriptor_t *desc, struct omx_clap_instance **out)
{
    *out = NULL;
    binary->refs++;
    return instance_create(binary, desc, out);
}

int omx_clap_host_open(const char *path, const char *id, struct omx_clap_instance **out, char why[OMX_CLAP_WHY_MAX])
{
    struct omx_clap_binary *bin;
    char reason[256];

    if (!path || !out)
        return -1;
    *out = NULL;
    bin = binary_ref(path, reason, sizeof(reason));
    if (!bin)
    {
        why_set(why, CLAP_HOST_CODE_HEADLESS_FAILED);
        return -1;
    }
    return open_from(bin, id, out, why);
}

int omx_clap_host_open_entry(const clap_plugin_entry_t *entry, const char *id, struct omx_clap_instance **out, char why[OMX_CLAP_WHY_MAX])
{
    struct omx_clap_binary *bin;
    char reason[256];

    if (!entry || !out)
        return -1;
    *out = NULL;
    bin = binary_ref_entry(entry, id, reason, sizeof(reason));
    if (!bin)
    {
        why_set(why, CLAP_HOST_CODE_HEADLESS_FAILED);
        return -1;
    }
    return open_from(bin, id, out, why);
}

int omx_clap_host_open_timed(const char *path, const char *id, unsigned timeout_ms, struct omx_clap_instance **out, char why[OMX_CLAP_WHY_MAX])
{
    struct timed_open *t = calloc(1, sizeof(*t));
    struct timespec until;
    pthread_t th;
    int rc;

    t->path = path;
    t->id = id;
    if (pthread_create(&th, NULL, timed_open_run, t) != 0)
    {
        free(t);
        why_set(why, CLAP_HOST_CODE_HEADLESS_FAILED);
        return -1;
    }
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += timeout_ms / 1000u;
    until.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (until.tv_nsec >= 1000000000L)
    {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    if (pthread_timedjoin_np(th, NULL, &until) == ETIMEDOUT)
    {
        pthread_detach(th);     // abandoned: the worker keeps its own record and frees nothing shared
        why_set(why, CLAP_HOST_CODE_CRASHED_LIVE);
        *out = NULL;
        return -1;
    }
    rc = t->rc;
    *out = t->out;
    if (t->out)
        t->out->main_thread = pthread_self();   // the opener's control thread is the main thread
    why_set(why, t->why);
    free(t);
    return rc;
}

void omx_clap_host_close(struct omx_clap_instance *in)
{
    if (!in)
        return;
    omx_clap_host_stop(in);
    if (in->active)
        omx_clap_host_deactivate(in);
    in->plugin->destroy(in->plugin);
    unoffered_forget(in);
    binary_unref(in->bin);
    free(in->shadow);
    free(in->contract);
    free(in);
}


/*
************************************************************************************************************************
*           GLOBAL FUNCTIONS: ACTIVATE, PUBLISH, UNPUBLISH, RESTART, RATE
************************************************************************************************************************
*/

void omx_clap_host_take_role(struct omx_clap_instance *in, struct omx_clap_role *role)
{
    uint32_t state = atomic_load(&in->stage.state);
    unsigned waited = 0;

    // a stage that is not running is the control thread's already; a running or armed one is held, so the cycle that
    // arrives meanwhile passes the lane through
    while (state != OMX_CLAP_IDLE && state != OMX_CLAP_STOPPED && state != OMX_CLAP_HELD
           && !atomic_compare_exchange_weak(&in->stage.state, &state, OMX_CLAP_HELD))
        ;
    while (atomic_load(&in->stage.in_cycle) && waited < CLAP_HOST_ROLE_TIMEOUT_US)
    {
        usleep(CLAP_HOST_ROLE_POLL_US);
        waited += CLAP_HOST_ROLE_POLL_US;
    }

    role->state = state;
    role->thread = in->audio_thread;
    role->held = in->audio_role_held;
    in->audio_thread = pthread_self();
    in->audio_role_held = 1;
}

void omx_clap_host_release_role(struct omx_clap_instance *in, const struct omx_clap_role *role, uint32_t state)
{
    uint32_t held = OMX_CLAP_HELD;

    in->audio_thread = role->thread;
    in->audio_role_held = role->held;
    atomic_compare_exchange_strong(&in->stage.state, &held, state);
}

static void tail_read(struct omx_clap_instance *in);

/* whether the caller bound any channel of auxiliary port `port`: `ports`, `base` and `bound` are one side's */
static bool aux_bound(const struct omx_clap_port *ports, const uint32_t *base, float *const *bound, uint32_t port)
{
    uint32_t c;

    for (c = 0; c < ports[port].channels; c++)
        if (bound[base[port] + c])
            return true;
    return false;
}

/* audio-ports-activation, while the plugin is inactive: an auxiliary port is active only while the caller's buffers are
 * bound to it (omx_clap_host_bind_aux_input, omx_clap_host_bind_aux_output), so the plugin may skip the work of a port
 * nobody reads or writes; the main ports stay active. A plugin with no extension keeps every port active. */
/* the plugin's audio-ports-activation, NULL when it has none to call */
static const clap_plugin_audio_ports_activation_t *ports_activation(struct omx_clap_instance *in)
{
    const clap_plugin_audio_ports_activation_t *ext = in->plugin->get_extension(in->plugin, CLAP_EXT_AUDIO_PORTS_ACTIVATION);

    if (!ext)
        ext = in->plugin->get_extension(in->plugin, CLAP_EXT_AUDIO_PORTS_ACTIVATION_COMPAT);
    return ext && ext->set_active && in->audio_ports ? ext : NULL;
}

/* the clap.audio-ports index of auxiliary port `aux` of a side, UINT32_MAX when the plugin no longer lists it */
static uint32_t aux_port_index(struct omx_clap_instance *in, bool is_input, uint32_t aux)
{
    const uint32_t count = in->audio_ports->count(in->plugin, is_input);

    for (uint32_t i = 0, k = 0; i < count; i++)
    {
        clap_audio_port_info_t info;

        memset(&info, 0, sizeof(info));
        if (!in->audio_ports->get(in->plugin, i, is_input, &info) || (info.flags & CLAP_AUDIO_PORT_IS_MAIN))
            continue;
        if (k++ == aux)
            return i;
    }
    return UINT32_MAX;
}

static void ports_activate(struct omx_clap_instance *in)
{
    const clap_plugin_audio_ports_activation_t *ext = ports_activation(in);
    uint32_t aux = 0, aux_out = 0;

    if (!ext)
        return;
    for (int dir = 0; dir < 2; dir++)
    {
        const bool is_input = dir == 0;
        const uint32_t count = in->audio_ports->count(in->plugin, is_input);

        for (uint32_t i = 0; i < count; i++)
        {
            clap_audio_port_info_t info;

            memset(&info, 0, sizeof(info));
            if (!in->audio_ports->get(in->plugin, i, is_input, &info) || (info.flags & CLAP_AUDIO_PORT_IS_MAIN))
                continue;
            if (is_input)
            {
                const bool bound = aux < in->aux_inputs && aux_bound(in->aux_in, in->aux_in_base, in->aux_in_bound, aux);

                aux++;
                ext->set_active(in->plugin, true, i, bound, 32);
            }
            else
            {
                const bool bound = aux_out < in->aux_outputs && aux_bound(in->aux_out, in->aux_out_base, in->aux_out_bound, aux_out);

                aux_out++;
                ext->set_active(in->plugin, false, i, bound, 32);
            }
        }
    }
}

int omx_clap_host_activate(struct omx_clap_instance *in, double rate, uint32_t max_block, char why[OMX_CLAP_WHY_MAX])
{
    struct omx_clap_role role;
    uint32_t bad, pending;

    if (!in || in->active || max_block == 0)
        return -1;
    if (in->main_out.channels == 0)
    {
        why_set(why, CLAP_HOST_CODE_NO_AUDIO_OUTPUT);           // a description a layout request left empty
        return -1;
    }
    for (bad = 0; bad < in->aux_inputs; bad++)
        if (aux_bound(in->aux_in, in->aux_in_base, in->aux_in_bound, bad) && in->aux_in_frames[bad] < max_block)
        {
            why_set(why, CLAP_HOST_CODE_HEADLESS_FAILED);       // bound buffers shorter than the block: rebind them first
            return -1;
        }
    for (bad = 0; bad < in->aux_outputs; bad++)
        if (aux_bound(in->aux_out, in->aux_out_base, in->aux_out_bound, bad) && in->aux_out_frames[bad] < max_block)
        {
            why_set(why, CLAP_HOST_CODE_HEADLESS_FAILED);
            return -1;
        }
    if (g_config.lane_channels && (in->main_in.channels > g_config.lane_channels || in->main_out.channels > g_config.lane_channels))
    {
        why_set(why, CLAP_HOST_CODE_WIDER_THAN_STRIP);          // a main port wider than the caller's lane
        return -1;
    }
    if (in->note_ports_stale)
    {
        const char *code = reread_note_input(in);

        if (code)
        {
            why_set(why, code);
            return -1;
        }
    }
    if (take_bounce(in, max_block) != 0)
    {
        drop_bounce(in);
        why_set(why, CLAP_HOST_CODE_HEADLESS_FAILED);
        return -1;
    }
    omx_clap_set_bypass(&in->stage, in->bypass_wanted);
    ports_activate(in);
    // a ports rescan the plugin made before is not this activate's to answer: a re-read of the description is
    // (request_layout); what stands here is what the warm-up's own cycle below must leave as it found it
    pending = atomic_load_explicit(&in->ports_rescan_requested, memory_order_relaxed);
    if (!in->plugin->activate(in->plugin, rate, 1, max_block))
    {
        drop_bounce(in);
        why_set(why, CLAP_HOST_CODE_HEADLESS_FAILED);
        return -1;
    }
    in->active = 1;
    in->rate = rate;
    if (bind_stage(in) != 0)
    {
        omx_clap_host_deactivate(in);
        why_set(why, CLAP_HOST_CODE_HEADLESS_FAILED);
        return -1;
    }
    if (g_config.warmup)
    {
        omx_clap_host_take_role(in, &role);
        // at the block size the host activated: a plugin whose state is counted in frames leaves the warm-up in the
        // state a host running that quantum would leave it in
        bad = omx_clap_prime(&in->stage, max_block);
        omx_clap_host_release_role(in, &role, role.state);
        if (bad != 0)
        {
            omx_clap_host_deactivate(in);
            why_set(why, bad == UINT32_MAX ? CLAP_HOST_CODE_HEADLESS_FAILED : CLAP_HOST_CODE_OUTPUT_NON_FINITE);
            return -1;
        }
        /*
         * The restart after the warm-up. Its job is to fault in every page the first live block would touch: done. Its
         * side effect is a plugin holding the warm-up's tail (the level half sits in a delay line longer than the silent
         * half), and reset() is not enough to drop it: CLAP lets a plugin answer reset() with request_restart, so the
         * one state every plugin defines as fresh is the one after activate. Deactivate and activate again, off the RT,
         * and the first live block starts from it; a restart the plugin asked for during the warm-up is answered by this
         * same cycle.
         */
        // what stood before and what the first activate raised stay; only the warm-up's own deactivate is dropped
        pending |= atomic_load_explicit(&in->ports_rescan_requested, memory_order_relaxed);
        in->plugin->deactivate(in->plugin);
        atomic_store_explicit(&in->ports_rescan_requested, pending, memory_order_relaxed);
        if (!in->plugin->activate(in->plugin, rate, 1, max_block))
        {
            in->active = 0;
            drop_bounce(in);
            why_set(why, CLAP_HOST_CODE_HEADLESS_FAILED);
            return -1;
        }
        if (bind_stage(in) != 0)
        {
            omx_clap_host_deactivate(in);
            why_set(why, CLAP_HOST_CODE_HEADLESS_FAILED);
            return -1;
        }
        atomic_store_explicit(&in->restart_requested, 0u, memory_order_relaxed);
    }
    voice_info_ask(in);
    tail_read(in);
    in->sleep.asleep = 0;
    in->sleep.have_last = 0;
    in->sleep.quiet_frames = 0;
    in->sleep.quiet_out_frames = 0;
    in->sleep.quiet_min_frames = (uint32_t)(rate * OMX_CLAP_SLEEP_QUIET_MS / 1000.0);
    publish_latency(in);
    if (in->passthrough)
    {
        char line[256];

        snprintf(line, sizeof(line), "%s: passthrough: host (no audio port: the strip's audio passes through untouched; the plugin's %u frames of latency apply to its notes alone)",
                 in->desc->id, in->latency ? in->latency->get(in->plugin) : 0u);
        log_to_ring(in, line);
    }
    // active for good: the process's idle hold counts it (#296)
    in->idle_held = 1;
    idle_hold_acquire();
    // granted the thread pool and asking for it: the process's pool counts it, or it runs serial and says why (#395)
    if (in->pool_use && in->thread_pool && in->thread_pool->exec)
    {
        char why_serial[CLAP_HOST_LOG_BYTES], line[CLAP_HOST_LOG_BYTES + 128];

        if (thread_pool_acquire(in->pool_priority, why_serial, sizeof(why_serial)))
            in->pool_held = 1;
        else
        {
            snprintf(line, sizeof(line), "%s: %s", in->desc->id, why_serial);
            log_to_ring(in, line);
        }
    }
    if (why)
        why[0] = '\0';
    return 0;
}

/* One writer at a time: the sequence is odd while the pair changes. */
static void set_role_predicate(struct omx_clap_instance *in, omx_clap_audio_role_fn is_audio, void *ctx)
{
    atomic_fetch_add_explicit(&in->audio_role_seq, 1u, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&in->audio_role_is, is_audio, memory_order_relaxed);
    atomic_store_explicit(&in->audio_role_ctx, is_audio ? ctx : NULL, memory_order_relaxed);
    atomic_fetch_add_explicit(&in->audio_role_seq, 1u, memory_order_release);
}

void omx_clap_host_set_audio_role(struct omx_clap_instance *in, pthread_t thread, omx_clap_audio_role_fn is_audio, void *ctx)
{
    set_role_predicate(in, is_audio, ctx);
    in->audio_thread = thread;
    in->audio_role_held = 1;
}

void omx_clap_host_publish_role(struct omx_clap_instance *in, pthread_t rt, omx_clap_audio_role_fn is_audio, void *ctx)
{
    omx_clap_host_set_audio_role(in, rt, is_audio, ctx);
    omx_clap_arm(&in->stage);
}

void omx_clap_host_publish(struct omx_clap_instance *in, pthread_t rt)
{
    omx_clap_host_publish_role(in, rt, NULL, NULL);
}

void omx_clap_host_set_audio_thread(struct omx_clap_instance *in, pthread_t thread)
{
    omx_clap_host_set_audio_role(in, thread, NULL, NULL);
}

void omx_clap_host_name_slot(struct omx_clap_instance *in, const _Atomic(pthread_t) *slot)
{
    atomic_store_explicit(&in->audio_slot, slot, memory_order_release);
}

void omx_clap_host_arm(struct omx_clap_instance *in)
{
    omx_clap_arm(&in->stage);
}

int omx_clap_host_unpublish(struct omx_clap_instance *in, unsigned poll_us, unsigned timeout_us)
{
    unsigned waited = 0;

    omx_clap_request_stop(&in->stage);
    while (!omx_clap_stopped(&in->stage))
    {
        if (waited >= timeout_us)
            return -1;
        usleep(poll_us);
        waited += poll_us;
    }
    in->audio_role_held = 0;
    set_role_predicate(in, NULL, NULL);
    atomic_store_explicit(&in->stage.state, OMX_CLAP_IDLE, memory_order_release);
    return 0;
}

void omx_clap_host_stop(struct omx_clap_instance *in)
{
    const uint32_t state = atomic_load(&in->stage.state);

    if (!in->active)
        return;
    if (state == OMX_CLAP_PROCESSING || state == OMX_CLAP_STOPPING)
    {
        in->audio_thread = pthread_self();
        in->audio_role_held = 1;
        if (in->plugin->stop_processing)
            in->plugin->stop_processing(in->plugin);
    }
    in->audio_role_held = 0;
    atomic_store_explicit(&in->stage.h.rendered_wet, 0, memory_order_relaxed);
    atomic_store(&in->stage.state, OMX_CLAP_IDLE);
}

/* After a deactivate of the host's own (a restart, a rate change, an auxiliary port's activation): when the plugin said
 * its ports changed in it (CLAP's request_restart, deactivate, rescan), the description is the caller's to read again
 * before an activate, so the instance stays inactive with the rescan reported and this answers
 * OMX_CLAP_RESTART_RESCANNED; otherwise activate at `rate` and `max_block`, 0 or -1 with `why`. */
static int activate_after_deactivate(struct omx_clap_instance *in, double rate, uint32_t max_block, char why[OMX_CLAP_WHY_MAX])
{
    if (atomic_exchange_explicit(&in->ports_rescan_requested, 0u, memory_order_relaxed))
    {
        atomic_store_explicit(&in->ports_rescanned, 1u, memory_order_relaxed);
        return OMX_CLAP_RESTART_RESCANNED;
    }
    return omx_clap_host_activate(in, rate, max_block, why) == 0 ? 0 : -1;
}

int omx_clap_host_restart(struct omx_clap_instance *in)
{
    int ret;

    const double rate = in->rate;
    const uint32_t max_block = in->max_block;
    const pthread_t thread = in->audio_thread;
    const int held = in->audio_role_held;

    if (!in->active)
        return -1;
    atomic_store_explicit(&in->restart_requested, 0u, memory_order_relaxed);
    if (wait_stopped(in) != 0)
        return -1;
    omx_clap_host_stop(in);
    omx_clap_host_deactivate(in);
    // a rescan in that deactivate leaves it inactive, its audio role kept for the activate after the re-read
    ret = activate_after_deactivate(in, rate, max_block, NULL);
    if (ret < 0)
        return -1;
    in->audio_thread = thread;
    in->audio_role_held = held;
    if (ret == 0)
        omx_clap_arm(&in->stage);
    return ret;
}

void omx_clap_host_deactivate(struct omx_clap_instance *in)
{
    uint32_t i;

    if (!in || !in->active)
        return;
    in->plugin->deactivate(in->plugin);
    in->active = 0;
    if (in->idle_held)
    {
        in->idle_held = 0;
        idle_hold_release();
    }
    if (in->pool_held)
    {
        in->pool_held = 0;
        thread_pool_release();
    }
    drop_bounce(in);
    memset(&in->stage, 0, sizeof(in->stage));
    for (i = 0; i < in->shadow_n; i++)
        in->shadow[i].valid = 0;
    in->shadow_n = 0;
}

int omx_clap_host_thread_pool_use(struct omx_clap_instance *in, int use, int rt_priority)
{
    if (!in || in->active || rt_priority < 0 || rt_priority > 99)
        return -1;
    in->pool_use = use ? 1u : 0u;
    in->pool_priority = rt_priority;
    return 0;
}

void omx_clap_host_thread_pool(struct omx_clap_thread_pool_info *out)
{
    if (out)
        thread_pool_info(out);
}

uint32_t omx_clap_host_thread_pool_late(const struct omx_clap_instance *in)
{
    return in ? atomic_load_explicit(&in->pool_late, memory_order_relaxed) : 0u;
}

int omx_clap_host_set_rate(struct omx_clap_instance *in, double rate, uint32_t max_block, char why[OMX_CLAP_WHY_MAX])
{
    if (!in)
        return -1;
    if (in->active && !omx_clap_stopped(&in->stage))
        return -1;      // unpublish first
    omx_clap_host_deactivate(in);
    return activate_after_deactivate(in, rate, max_block, why);
}

uint32_t omx_clap_host_latency(const struct omx_clap_instance *in)
{
    return atomic_load_explicit(&in->stage.latency_frames, memory_order_relaxed);
}

void omx_clap_host_bypass(struct omx_clap_instance *in, int on)
{
    struct omx_clap_role role;
    const uint32_t was = in->bypass_wanted;

    in->bypass_wanted = on ? 1u : 0u;
    // a bypass, an inactive instance, or an off that was already off: the flag alone. A live engaged stage is never held
    // for a redundant off, which would pass one block dry and fade back in
    if (on || !in->active || !was)
    {
        omx_clap_set_bypass(&in->stage, on);
        return;
    }
    // re-engage: a plugin that sat idle through a steady bypass may hold a frozen tail. It is reset here, on the control
    // thread holding the audio role, before the stage resumes; the RT body never calls reset()
    omx_clap_host_take_role(in, &role);
    if (in->stage.need_reset)
    {
        if (in->plugin->reset)
            in->plugin->reset(in->plugin);
        omx_clap_note_ids_reset(&in->note_ids);     // the plugin's voices are gone, and with them every id
        omx_hosted_count(&in->stage.resets, 1);
        in->stage.need_reset = 0;
    }
    omx_clap_set_bypass(&in->stage, 0);
    omx_clap_host_release_role(in, &role, role.state);
}

void omx_clap_host_set_tempo(struct omx_clap_instance *in, const struct omx_clap_tempo *tempo)
{
    in->tempo = tempo;
}

void omx_clap_host_set_transport(struct omx_clap_instance *in, const struct omx_clap_transport_src *src)
{
    in->transport_src = src;
}

/* the caller's buffers into the flat list `bound` from `base`, or none: the rules both directions share */
static int bind_aux(struct omx_clap_instance *in, const struct omx_clap_port *port, float **bound, uint32_t *bound_frames,
                    float *const *bufs, uint32_t channels, uint32_t frames)
{
    uint32_t c;

    if (bufs && channels != port->channels)
        return -1;
    if (in->active && !omx_clap_stopped(&in->stage))
        return -1;      // unpublish first
    if (bufs && in->active && frames < in->max_block)
        return -1;      // a block would run past the buffers
    for (c = 0; c < port->channels; c++)
        bound[c] = bufs ? bufs[c] : NULL;
    *bound_frames = bufs ? frames : 0;
    return 0;
}

/* An active instance whose auxiliary port `aux` of a side changed between bound and unbound tells the plugin's
 * audio-ports-activation, on the control thread while the stage is stopped: through set_active holding the audio role
 * when the plugin says it can activate a port while processing, else by a deactivate and an activate, which set every
 * port again. Returns 0, or -1 when the activate refuses (the instance is then inactive). */
static int aux_activation_refresh(struct omx_clap_instance *in, bool is_input, uint32_t aux, bool was, bool now)
{
    const clap_plugin_audio_ports_activation_t *ext = in->active && was != now ? ports_activation(in) : NULL;
    const double rate = in->rate;
    const uint32_t max_block = in->max_block;
    struct omx_clap_role role;
    uint32_t index;
    bool done;

    if (!ext)
        return 0;
    index = aux_port_index(in, is_input, aux);
    if (index != UINT32_MAX && ext->can_activate_while_processing && ext->can_activate_while_processing(in->plugin))
    {
        omx_clap_host_take_role(in, &role);
        done = ext->set_active(in->plugin, is_input, index, now, 32);
        omx_clap_host_release_role(in, &role, role.state);
        if (done)
            return 0;
    }
    omx_clap_host_deactivate(in);
    return activate_after_deactivate(in, rate, max_block, NULL);
}

int omx_clap_host_bind_aux_input(struct omx_clap_instance *in, uint32_t port, float *const *bufs, uint32_t channels, uint32_t frames)
{
    bool was;

    if (!in || port >= in->aux_inputs)
        return -1;
    was = aux_bound(in->aux_in, in->aux_in_base, in->aux_in_bound, port);
    if (bind_aux(in, &in->aux_in[port], &in->aux_in_bound[in->aux_in_base[port]], &in->aux_in_frames[port], bufs, channels, frames) != 0)
        return -1;
    if (in->active)
        omx_clap_set_aux_in(&in->aux_list, port, in->aux_in[port].channels, &in->aux_in_bound[in->aux_in_base[port]]);
    return aux_activation_refresh(in, true, port, was, aux_bound(in->aux_in, in->aux_in_base, in->aux_in_bound, port));
}

int omx_clap_host_bind_aux_output(struct omx_clap_instance *in, uint32_t port, float *const *bufs, uint32_t channels, uint32_t frames)
{
    bool was;

    if (!in || port >= in->aux_outputs)
        return -1;
    was = aux_bound(in->aux_out, in->aux_out_base, in->aux_out_bound, port);
    if (bind_aux(in, &in->aux_out[port], &in->aux_out_bound[in->aux_out_base[port]], &in->aux_out_frames[port], bufs, channels, frames) != 0)
        return -1;
    if (in->active)
        omx_clap_set_aux_out(&in->stage, port, in->aux_out[port].channels, &in->aux_out_bound[in->aux_out_base[port]]);
    return aux_activation_refresh(in, false, port, was, aux_bound(in->aux_out, in->aux_out_base, in->aux_out_bound, port));
}

int omx_clap_host_bypassed(const struct omx_clap_instance *in)
{
    return in->bypass_wanted != 0;
}


/*
************************************************************************************************************************
*           GLOBAL FUNCTIONS: PARAMETERS
************************************************************************************************************************
*/

/*
 * The roster, whole or refused: walk every index below count() once, -1 when one does not answer get_info (a roster
 * with a hole is never served short). Returns the number of rows; with `want` < UINT32_MAX, the row of index `want`
 * lands in `row` and `*found` says whether it existed. Each call re-reads the plugin: O(n), and a caller walking the
 * rows by index is O(n^2) - omx_clap_host_param_roster reads them all in one pass.
 */
static int roster_walk(struct omx_clap_instance *in, uint32_t want, struct omx_clap_param_row *row, int *found)
{
    uint32_t n, i, rows = 0;

    if (found)
        *found = 0;
    if (!in->params)
        return 0;
    n = param_count_of(in, NULL);
    for (i = 0; i < n; i++)
    {
        clap_param_info_t info;

        memset(&info, 0, sizeof(info));
        if (!in->params->get_info(in->plugin, i, &info))
            return -1;
        if (!is_row(&info))
            continue;
        if (rows == want && row)
        {
            row_of(&info, row);
            *found = 1;
        }
        rows++;
    }
    return (int)rows;
}

uint32_t omx_clap_host_param_count(struct omx_clap_instance *in)
{
    const int rows = roster_walk(in, UINT32_MAX, NULL, NULL);

    return rows < 0 ? 0u : (uint32_t)rows;
}

int omx_clap_host_param_row(struct omx_clap_instance *in, uint32_t index, struct omx_clap_param_row *row)
{
    struct omx_clap_param_row r;
    int found;

    if (index == UINT32_MAX || roster_walk(in, index, &r, &found) < 0 || !found)
        return -1;
    *row = r;
    return 0;
}

int omx_clap_host_param_roster(struct omx_clap_instance *in, struct omx_clap_param_row **rows, const char **why)
{
    struct omx_clap_param_row *out;
    uint32_t n, i, k = 0;

    *rows = NULL;
    if (!in->params)
        return 0;
    n = param_count_of(in, NULL);
    if (n == 0)
        return 0;
    out = calloc(n, sizeof(*out));
    if (!out)
    {
        if (why)
            *why = OMX_CLAP_PARAM_ROW_UNREADABLE;
        return -1;
    }
    for (i = 0; i < n; i++)
    {
        clap_param_info_t info;

        memset(&info, 0, sizeof(info));
        if (!in->params->get_info(in->plugin, i, &info))
        {
            free(out);
            if (why)
                *why = OMX_CLAP_PARAM_ROW_UNREADABLE;   // never the rows read so far
            return -1;
        }
        if (is_row(&info))
            row_of(&info, &out[k++]);
    }
    if (k == 0)
    {
        free(out);  // parameters, none of them a row: nothing to hand back
        return 0;
    }
    *rows = out;
    return (int)k;
}

int omx_clap_host_param_write(struct omx_clap_instance *in, clap_id id, double value)
{
    clap_param_info_t info;
    struct omx_clap_shadow *s;

    if (info_of(in, id, &info) != 0 || !is_row(&info))
        return -1;
    if (!in->active)
        return -1;
    if (omx_clap_param_push(&in->stage, id, value, info.cookie) != 0)
        return -1;
    s = shadow_of(in, id, 1);
    if (s)
    {
        s->delivered = value;
        s->cycle = atomic_load_explicit(&in->stage.h.runs, memory_order_relaxed);
        s->valid = 1;
    }
    return 0;
}

int omx_clap_host_param_flush(struct omx_clap_instance *in)
{
    uint32_t st;

    if (!in->active || !in->params)
        return -1;
    st = atomic_load_explicit(&in->stage.state, memory_order_acquire);
    if (st != OMX_CLAP_IDLE && st != OMX_CLAP_STOPPED)
        return -1;      // never while the RT drains
    while (omx_clap_queue_pending(&in->stage.queue) > 0)
    {
        omx_clap_drain(&in->stage);
        in->params->flush(in->plugin, &in->stage.in_events, &in->stage.out_events);
        in->stage.n_events = 0;
    }
    return 0;
}

void omx_clap_host_param_deliver(struct omx_clap_instance *in)
{
    struct omx_clap_role role;

    if (!in->active || !in->params || !in->params->flush)
        return;
    omx_clap_host_take_role(in, &role);
    while (omx_clap_queue_pending(&in->stage.queue) > 0)
    {
        omx_clap_drain(&in->stage);
        in->params->flush(in->plugin, &in->stage.in_events, &in->stage.out_events);
        in->stage.n_events = 0;
    }
    omx_clap_host_release_role(in, &role, role.state);
}

void omx_clap_host_settle(struct omx_clap_instance *in, unsigned timeout_us)
{
    const uint32_t runs = atomic_load(&in->stage.h.runs);
    unsigned waited = 0;

    if (!in->active)
        return;
    while (omx_clap_queue_pending(&in->stage.queue) > 0 && waited < timeout_us)
    {
        usleep(CLAP_HOST_ROLE_POLL_US);
        waited += CLAP_HOST_ROLE_POLL_US;
    }
    if (omx_clap_queue_pending(&in->stage.queue) > 0 && atomic_load(&in->stage.h.runs) == runs)
        omx_clap_host_param_deliver(in);
}

int omx_clap_host_param_read(struct omx_clap_instance *in, clap_id id, double *value)
{
    if (!in->params || !value)
        return -1;
    return in->params->get_value(in->plugin, id, value) ? 0 : -1;
}

int omx_clap_host_param_is_row(struct omx_clap_instance *in, clap_id id)
{
    clap_param_info_t info;

    return info_of(in, id, &info) == 0 && is_row(&info);
}

int omx_clap_host_param_readable(struct omx_clap_instance *in, clap_id id)
{
    clap_param_info_t info;

    return info_of(in, id, &info) == 0 && !(info.flags & CLAP_PARAM_IS_HIDDEN);
}

int omx_clap_host_param_row_of(struct omx_clap_instance *in, clap_id id, struct omx_clap_param_row *row)
{
    clap_param_info_t info;

    if (info_of(in, id, &info) != 0 || !is_row(&info))
        return -1;
    row_of(&info, row);
    return 0;
}

int omx_clap_host_param_value(struct omx_clap_instance *in, clap_id id, double *value)
{
    const struct omx_clap_shadow *s = shadow_of(in, id, 0);

    if (s && s->valid && atomic_load_explicit(&in->stage.h.runs, memory_order_relaxed) <= s->cycle)
    {
        *value = s->delivered;
        return 0;
    }
    return omx_clap_host_param_read(in, id, value);
}

int omx_clap_host_param_compare(struct omx_clap_instance *in, clap_id id, double *applied)
{
    const struct omx_clap_shadow *s = shadow_of(in, id, 0);
    double got;

    if (!s || !s->valid || omx_clap_host_param_read(in, id, &got) != 0)
        return -1;
    if (applied)
        *applied = got;
    return got == s->delivered ? 1 : 0;
}


/*
************************************************************************************************************************
*           GLOBAL FUNCTIONS: STATE, PRESETS, THE TICK
************************************************************************************************************************
*/

int omx_clap_host_state_save(struct omx_clap_instance *in, void *buf, size_t cap, size_t *len)
{
    struct bounded_stream b = { buf, NULL, cap, 0, 0, 0 };
    const clap_ostream_t os = { &b, ostream_write };

    if (!in->state || !buf || !len)
        return -1;
    if (!in->state->save(in->plugin, &os) || b.overflow)
        return -1;
    *len = b.len;
    return 0;
}

int omx_clap_host_state_load(struct omx_clap_instance *in, const void *buf, size_t len)
{
    struct bounded_stream b = { NULL, buf, len, len, 0, 0 };
    const clap_istream_t is = { &b, istream_read };

    if (!in->state || !buf)
        return -1;
    return in->state->load(in->plugin, &is) ? 0 : -1;
}

int omx_clap_host_state_save_context(struct omx_clap_instance *in, uint32_t context, void *buf, size_t cap, size_t *len)
{
    struct bounded_stream b = { buf, NULL, cap, 0, 0, 0 };
    const clap_ostream_t os = { &b, ostream_write };
    const clap_plugin_state_context_t *ext = in ? in->plugin->get_extension(in->plugin, CLAP_EXT_STATE_CONTEXT) : NULL;

    if (!in || !buf || !len || context < CLAP_STATE_CONTEXT_FOR_PRESET || context > CLAP_STATE_CONTEXT_FOR_PROJECT)
        return -1;
    if (!ext || !ext->save)
        return omx_clap_host_state_save(in, buf, cap, len);
    if (!ext->save(in->plugin, &os, context) || b.overflow)
        return -1;
    *len = b.len;
    return 0;
}

int omx_clap_host_state_load_context(struct omx_clap_instance *in, uint32_t context, const void *buf, size_t len)
{
    struct bounded_stream b = { NULL, buf, len, len, 0, 0 };
    const clap_istream_t is = { &b, istream_read };
    const clap_plugin_state_context_t *ext = in ? in->plugin->get_extension(in->plugin, CLAP_EXT_STATE_CONTEXT) : NULL;

    if (!in || !buf || context < CLAP_STATE_CONTEXT_FOR_PRESET || context > CLAP_STATE_CONTEXT_FOR_PROJECT)
        return -1;
    if (!ext || !ext->load)
        return omx_clap_host_state_load(in, buf, len);
    return ext->load(in->plugin, &is, context) ? 0 : -1;
}

int omx_clap_host_set_render(struct omx_clap_instance *in, int32_t mode)
{
    const clap_plugin_render_t *ext = in ? in->plugin->get_extension(in->plugin, CLAP_EXT_RENDER) : NULL;

    if (!ext || !ext->set || (mode != CLAP_RENDER_REALTIME && mode != CLAP_RENDER_OFFLINE))
        return -1;
    if (mode == CLAP_RENDER_OFFLINE && ext->has_hard_realtime_requirement && ext->has_hard_realtime_requirement(in->plugin))
        return -1;
    return ext->set(in->plugin, mode) ? 0 : -1;
}

uint32_t omx_clap_host_tail(const struct omx_clap_instance *in)
{
    return in ? atomic_load_explicit(&in->tail_frames, memory_order_relaxed) : 0;
}

/* from_location answers true for a load a plugin refuses later or says it could not do through the host's on_error
 * (Surge XT does for a file it cannot open): the load is done only when it answered true and reported no error */
static int preset_from_location(struct omx_clap_instance *in, uint32_t location_kind, const char *location, const char *load_key)
{
    int ok;

    g_preset_failed = 0;
    g_preset_loading = in;
    ok = in->preset_load->from_location(in->plugin, location_kind, location, load_key);
    g_preset_loading = NULL;
    return ok && !g_preset_failed ? 0 : -1;
}

int omx_clap_host_preset_load(struct omx_clap_instance *in, const char *location)
{
    if (!in->preset_load)
        return -1;
    return preset_from_location(in, CLAP_PRESET_DISCOVERY_LOCATION_FILE, location, NULL);
}

int omx_clap_host_preset_load_key(struct omx_clap_instance *in, uint32_t location_kind, const char *location, const char *load_key)
{
    if (!in || !in->preset_load)
        return -1;
    if (location_kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE ? !location : location_kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN)
        return -1;
    return preset_from_location(in, location_kind, location_kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN ? NULL : location, load_key);
}

uint32_t omx_clap_host_param_events_take(struct omx_clap_instance *in, struct omx_clap_param_event *events, uint32_t capacity)
{
    struct omx_clap_param_out *o = &in->param_out;
    uint32_t head = atomic_load_explicit(&o->head, memory_order_relaxed);
    const uint32_t tail = atomic_load_explicit(&o->tail, memory_order_acquire);
    uint32_t n = 0;

    while (head != tail && n < capacity)
        events[n++] = o->ring[head++ & (OMX_CLAP_PARAM_OUT_DEPTH - 1u)];
    atomic_store_explicit(&o->head, head, memory_order_release);
    return n;
}

uint32_t omx_clap_host_param_events_dropped(const struct omx_clap_instance *in)
{
    return atomic_load_explicit(&in->param_out.dropped, memory_order_relaxed);
}

/* a modulation or a gesture of the host: queued like a write, after the same checks, its cookie the tag of the ring
 * position it takes (the control thread is the ring's one producer, so the position is the tail it reads), and no
 * shadow: the set value is what it was */
static int param_push_tagged(struct omx_clap_instance *in, clap_id id, uint32_t type, double value, uint32_t needs)
{
    clap_param_info_t info;
    struct omx_clap_param_tag *tag;
    uint32_t tail;

    if (info_of(in, id, &info) != 0 || !is_row(&info) || (info.flags & needs) != needs || !in->active)
        return -1;
    tail = atomic_load_explicit(&in->stage.queue.tail, memory_order_relaxed);
    tag = &in->param_in.tag[tail & (2 * CLAP_HOST_PARAM_QUEUE_DEPTH - 1u)];
    tag->type = type;
    tag->cookie = info.cookie;
    return omx_clap_param_push(&in->stage, id, value, tag);
}

int omx_clap_host_param_modulate(struct omx_clap_instance *in, clap_id id, double amount)
{
    return isfinite(amount) ? param_push_tagged(in, id, CLAP_EVENT_PARAM_MOD, amount, CLAP_PARAM_IS_MODULATABLE) : -1;
}

int omx_clap_host_param_gesture(struct omx_clap_instance *in, clap_id id, int begin)
{
    return param_push_tagged(in, id, begin ? CLAP_EVENT_PARAM_GESTURE_BEGIN : CLAP_EVENT_PARAM_GESTURE_END, 0.0, 0u);
}

int omx_clap_host_ports_rescanned(struct omx_clap_instance *in)
{
    return in && atomic_exchange_explicit(&in->ports_rescanned, 0u, memory_order_relaxed) ? 1 : 0;
}

uint32_t omx_clap_host_params_rescanned(struct omx_clap_instance *in)
{
    return atomic_exchange_explicit(&in->params_rescanned, 0u, memory_order_relaxed);
}

int omx_clap_host_param_text(struct omx_clap_instance *in, clap_id id, double value, char *out, uint32_t size)
{
    clap_param_info_t info;

    if (!out || size == 0 || !in->params || !in->params->value_to_text || info_of(in, id, &info) != 0)
        return -1;
    out[0] = '\0';
    if (!in->params->value_to_text(in->plugin, id, value, out, size))
        return -1;
    out[size - 1] = '\0';  // the plugin's own buffer use: read no further than what it was given
    return 0;
}

int omx_clap_host_param_parse(struct omx_clap_instance *in, clap_id id, const char *text, double *value)
{
    clap_param_info_t info;
    double v;

    if (!text || !value || !in->params || !in->params->text_to_value || info_of(in, id, &info) != 0)
        return -1;
    if (!in->params->text_to_value(in->plugin, id, text, &v) || !isfinite(v))
        return -1;
    *value = v;
    return 0;
}

/* the plugin's tail, as the RT's sleep reads it: 0 with no extension */
static void tail_read(struct omx_clap_instance *in)
{
    atomic_store_explicit(&in->tail_frames, in->tail && in->tail->get ? in->tail->get(in->plugin) : 0u, memory_order_relaxed);
}

/* every timer that is due gets its on_timer, once, and its next due time a period on (from now when the tick ran late) */
static void timers_serve(struct omx_clap_instance *in)
{
    uint64_t now = 0;

    for (uint32_t i = 0; i < OMX_CLAP_TIMERS; i++)
    {
        struct omx_clap_timer *t = &in->timers[i];

        if (!t->used)
            continue;
        if (!now)
            now = now_ns();
        if (t->due_ns > now)
            continue;
        t->due_ns += (uint64_t)t->period_ms * 1000000u;
        if (t->due_ns <= now)
            t->due_ns = now + (uint64_t)t->period_ms * 1000000u;
        if (in->timer_support && in->timer_support->on_timer)
            in->timer_support->on_timer(in->plugin, t->id);
    }
}

/* the registered descriptors, polled without waiting; each that is ready gets its on_fd with what it is ready for */
static void fds_serve(struct omx_clap_instance *in)
{
    struct pollfd p[OMX_CLAP_FDS];
    nfds_t n = 0;

    for (uint32_t i = 0; i < OMX_CLAP_FDS; i++)
        if (in->fds[i].fd >= 0)
        {
            p[n].fd = in->fds[i].fd;
            p[n].events = (short)(((in->fds[i].flags & CLAP_POSIX_FD_READ) ? POLLIN : 0) |
                                  ((in->fds[i].flags & CLAP_POSIX_FD_WRITE) ? POLLOUT : 0));
            p[n].revents = 0;
            n++;
        }
    if (n == 0 || poll(p, n, 0) <= 0)
        return;
    for (nfds_t k = 0; k < n; k++)
    {
        uint32_t flags = 0;

        if (p[k].revents & POLLIN)
            flags |= CLAP_POSIX_FD_READ;
        if (p[k].revents & POLLOUT)
            flags |= CLAP_POSIX_FD_WRITE;
        if (p[k].revents & (POLLERR | POLLHUP | POLLNVAL))
            flags |= CLAP_POSIX_FD_ERROR;
        // a descriptor the plugin unregistered from an earlier on_fd of this same tick is not called
        if (flags && fd_slot(in, p[k].fd) >= 0 && in->fd_support && in->fd_support->on_fd)
            in->fd_support->on_fd(in->plugin, p[k].fd, flags);
    }
}

int omx_clap_host_tick(struct omx_clap_instance *in)
{
    const uint32_t rescan = atomic_exchange_explicit(&in->params_rescan_flags, 0u, memory_order_relaxed);
    const uint32_t note_rescan = atomic_exchange_explicit(&in->note_ports_rescan_flags, 0u, memory_order_relaxed);

    // a rescan of everything is only the plugin's to make while inactive: made while active, it is a restart
    if (rescan)
    {
        contract_recheck(in);
        atomic_fetch_or_explicit(&in->params_rescanned, rescan, memory_order_relaxed);
        if ((rescan & CLAP_PARAM_RESCAN_ALL) && in->active)
            atomic_store_explicit(&in->restart_requested, 1u, memory_order_relaxed);
    }
    // the same for the note ports: a rescan of every port is a restart while active, and the next activate reads them
    if (note_rescan)
    {
        atomic_fetch_or_explicit(&in->note_ports_rescanned, note_rescan, memory_order_relaxed);
        if (note_rescan & CLAP_NOTE_PORTS_RESCAN_ALL)
        {
            in->note_ports_stale = 1;
            if (in->active)
                atomic_store_explicit(&in->restart_requested, 1u, memory_order_relaxed);
        }
    }
    if (atomic_exchange_explicit(&in->voice_info_changed, 0u, memory_order_relaxed) && in->active)
        voice_info_ask(in);
    if (atomic_exchange_explicit(&in->tail_changed, 0u, memory_order_relaxed) && in->active)
        tail_read(in);
    timers_serve(in);
    fds_serve(in);
    if (atomic_exchange_explicit(&in->callback_requested, 0u, memory_order_relaxed) && in->plugin->on_main_thread)
        in->plugin->on_main_thread(in->plugin);
    // an unoffered id has its turn ahead of the plugin's line, which waits in log_ring for the next free slot: the ids
    // are each said once, so a plugin that logs every tick neither starves them nor is starved for long
    if (!atomic_load_explicit(&in->log_fresh, memory_order_acquire) && unoffered_say(in))
        atomic_store_explicit(&in->log_fresh, 1u, memory_order_release);
    else if (atomic_exchange_explicit(&in->log_pending, 0u, memory_order_acquire))
    {
        snprintf(in->last_log, sizeof(in->last_log), "%s", in->log_ring);
        atomic_store_explicit(&in->log_fresh, 1u, memory_order_release);
    }
    if (atomic_exchange_explicit(&in->latency_changed, 0u, memory_order_relaxed) && in->active && in->latency)
        publish_latency(in);
    // a ports rescan (audio-ports, the surround or ambisonic layout changed, or the configuration list changed while
    // inactive) is reported, once; no restart
    if (atomic_exchange_explicit(&in->ports_rescan_requested, 0u, memory_order_relaxed))
        atomic_store_explicit(&in->ports_rescanned, 1u, memory_order_relaxed);
    return atomic_load_explicit(&in->restart_requested, memory_order_relaxed) != 0;
}

const char *omx_clap_host_log_take(struct omx_clap_instance *in)
{
    return atomic_exchange_explicit(&in->log_fresh, 0u, memory_order_acquire) ? in->last_log : NULL;
}
