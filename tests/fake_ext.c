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
 * A plugin built for the tests of what a full host offers besides the basics: org.omx-clap-host.test.ext, a stereo
 * passthrough with one auxiliary output it leaves silent. It
 *  - registers a timer of FAKE_EXT_TIMER_MS at init, and the descriptors named by FAKE_EXT_FD and FAKE_EXT_FD2 for
 *    reading, and counts
 *    on_timer and on_fd (it reads one byte per on_fd);
 *  - asks the host's thread pool from init and from fake_ext_ask_pool, outside process(), which a host refuses, and
 *    inside process() for
 *    `pool_request` tasks, recording whether the host ran every one before request_exec returned;
 *  - records render.set, and answers has_hard_realtime_requirement with `hard_rt`;
 *  - answers tail.get with `tail` (fake_ext_set_tail changes it and tells the host);
 *  - returns `status` from process() and counts its calls and the steady time it was handed; while `ring` counts down it
 *    adds 0.25 to its output, sounding on a silent input;
 *  - saves "plain" through clap.state and "preset", "duplicate" or "project" through clap.state-context, and records
 *    the context of a load;
 *  - records audio-ports-activation.set_active for its auxiliary output, and answers can_activate_while_processing with
 *    `while_processing`; counts its activate calls.
 * Everything it records is in the struct fake_ext_stats returns, for a test that dlsyms it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <clap/clap.h>

#define ID_EXT              "org.omx-clap-host.test.ext"
#define FAKE_EXT_TIMER_MS   50u

typedef struct FAKE_EXT_STATS_T {
    uint32_t timer_calls;       ///< on_timer calls
    clap_id timer_id;           ///< what register_timer answered
    uint32_t timer_registered;  ///< 1 when register_timer succeeded
    uint32_t fd_calls;          ///< on_fd calls
    uint32_t fd_flags;          ///< the flags of every on_fd, or'd
    uint32_t pool_refused_outside;  ///< 1 when request_exec from init answered false
    uint32_t pool_request;      ///< the tasks process() asks the pool for (the test sets it)
    uint32_t pool_ok;           ///< request_exec's last answer inside process()
    uint32_t pool_done;         ///< the tasks done when request_exec returned
    uint32_t pool_nested;       ///< 1: each task asks the pool again from inside exec (the test sets it)
    uint32_t pool_nested_ok;    ///< how many of those nested requests the host ran (it must refuse them: 0)
    uint32_t pool_nested_refused;   ///< how many it refused
    int32_t render_mode;        ///< the last render.set, -1 before any
    uint32_t hard_rt;           ///< what has_hard_realtime_requirement answers (the test sets it)
    uint32_t tail;              ///< what tail.get answers
    int32_t status;             ///< what process() returns (the test sets it)
    uint32_t process_calls;     ///< process() calls
    int64_t last_steady;        ///< the steady time of the last process()
    uint32_t loaded_context;    ///< the context of the last state-context load, 0 before any
    uint32_t plain_loads;       ///< state.load calls
    uint32_t aux_out_set;       ///< set_active calls for the auxiliary output
    uint32_t aux_out_active;    ///< its last is_active
    uint32_t ring;              ///< process() calls left that add 0.25 to the output, as a voice or a tail still sounding
    uint32_t while_processing;  ///< what can_activate_while_processing answers (the test sets it)
    uint32_t activations;       ///< activate calls
    uint32_t rescan_on_deactivate;  ///< deactivate calls the host's audio-ports rescan (the test sets it)
} fake_ext_stats_t;

static fake_ext_stats_t g_stats;
static const clap_host_t *g_host;
static const clap_plugin_t *g_plugin;
static uint32_t g_tasks;        ///< the bits of the tasks done in this request

CLAP_EXPORT fake_ext_stats_t *fake_ext_stats(void)
{
    return &g_stats;
}

/* the plugin says its tail changed */
CLAP_EXPORT int fake_ext_set_tail(uint32_t frames)
{
    const clap_host_tail_t *tail = g_host ? g_host->get_extension(g_host, CLAP_EXT_TAIL) : NULL;

    g_stats.tail = frames;
    if (!tail)
        return -1;
    tail->changed(g_host);
    return 0;
}

/* the plugin asks the host's pool for one task from the caller's thread: what request_exec answers */
CLAP_EXPORT int fake_ext_ask_pool(void)
{
    const clap_host_thread_pool_t *pool = g_host ? g_host->get_extension(g_host, CLAP_EXT_THREAD_POOL) : NULL;

    return pool && pool->request_exec(g_host, 1);
}

/* the plugin asks to be processed */
CLAP_EXPORT void fake_ext_wake(void)
{
    if (g_host)
        g_host->request_process(g_host);
}

static const char *const g_features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL };
static const clap_plugin_descriptor_t g_desc = { CLAP_VERSION_INIT, ID_EXT, "ext", "omx-clap-host", "", "", "", "0", "", g_features };

static bool plugin_init(const clap_plugin_t *plugin)
{
    const clap_host_timer_support_t *timers = g_host->get_extension(g_host, CLAP_EXT_TIMER_SUPPORT);
    const clap_host_posix_fd_support_t *fds = g_host->get_extension(g_host, CLAP_EXT_POSIX_FD_SUPPORT);
    const clap_host_thread_pool_t *pool = g_host->get_extension(g_host, CLAP_EXT_THREAD_POOL);
    const char *fd = getenv("FAKE_EXT_FD"), *fd2 = getenv("FAKE_EXT_FD2");

    (void)plugin;
    if (timers && timers->register_timer(g_host, FAKE_EXT_TIMER_MS, &g_stats.timer_id))
        g_stats.timer_registered = 1;
    if (fds && fd)
        fds->register_fd(g_host, atoi(fd), CLAP_POSIX_FD_READ);
    if (fds && fd2)
        fds->register_fd(g_host, atoi(fd2), CLAP_POSIX_FD_READ);
    g_stats.pool_refused_outside = pool && !pool->request_exec(g_host, 1);
    return true;
}

static void plugin_destroy(const clap_plugin_t *plugin)
{
    (void)plugin;
    g_host = NULL;
    g_plugin = NULL;
}

static bool plugin_activate(const clap_plugin_t *plugin, double rate, uint32_t min_frames, uint32_t max_frames)
{
    g_stats.activations++;
    (void)plugin;
    (void)rate;
    (void)min_frames;
    (void)max_frames;
    return true;
}

static void plugin_deactivate(const clap_plugin_t *plugin)
{
    const clap_host_audio_ports_t *ports = g_host && g_stats.rescan_on_deactivate ? g_host->get_extension(g_host, CLAP_EXT_AUDIO_PORTS) : NULL;

    (void)plugin;
    if (ports)
        ports->rescan(g_host, CLAP_AUDIO_PORTS_RESCAN_LIST);
}
static bool plugin_start_processing(const clap_plugin_t *plugin) { (void)plugin; return true; }
static void plugin_stop_processing(const clap_plugin_t *plugin) { (void)plugin; }
static void plugin_reset(const clap_plugin_t *plugin) { (void)plugin; }

static clap_process_status plugin_process(const clap_plugin_t *plugin, const clap_process_t *process)
{
    const clap_host_thread_pool_t *pool = g_host->get_extension(g_host, CLAP_EXT_THREAD_POOL);
    uint32_t c, i;

    (void)plugin;
    for (c = 0; c < 2; c++)
        for (i = 0; i < process->frames_count; i++)
            process->audio_outputs[0].data32[c][i] = process->audio_inputs[0].data32[c][i] + (g_stats.ring ? 0.25f : 0.0f);
    if (g_stats.ring)
        g_stats.ring--;
    for (i = 1; i < process->audio_outputs_count; i++)
        for (c = 0; c < process->audio_outputs[i].channel_count; c++)
            memset(process->audio_outputs[i].data32[c], 0, process->frames_count * sizeof(float));
    g_stats.process_calls++;
    g_stats.last_steady = process->steady_time;
    if (g_stats.pool_request && pool)
    {
        g_tasks = 0;
        g_stats.pool_ok = pool->request_exec(g_host, g_stats.pool_request);
        g_stats.pool_done = (uint32_t)__builtin_popcount(g_tasks);
    }
    return g_stats.status;
}

static void pool_exec(const clap_plugin_t *plugin, uint32_t task)
{
    (void)plugin;
    if (task < 32)
        g_tasks |= 1u << task;
    if (g_stats.pool_nested && g_host && ((const clap_host_thread_pool_t *)g_host->get_extension(g_host, CLAP_EXT_THREAD_POOL))->request_exec(g_host, 1))
        g_stats.pool_nested_ok++;
    else if (g_stats.pool_nested)
        g_stats.pool_nested_refused++;
}

static void on_timer(const clap_plugin_t *plugin, clap_id id)
{
    (void)plugin;
    if (id == g_stats.timer_id)
        g_stats.timer_calls++;
}

static void on_fd(const clap_plugin_t *plugin, int fd, clap_posix_fd_flags_t flags)
{
    char byte;

    (void)plugin;
    g_stats.fd_calls++;
    g_stats.fd_flags |= flags;
    if (flags & CLAP_POSIX_FD_READ)
        (void)!read(fd, &byte, 1);
}

static bool render_hard_rt(const clap_plugin_t *plugin) { (void)plugin; return g_stats.hard_rt != 0; }
static bool render_set(const clap_plugin_t *plugin, clap_plugin_render_mode mode) { (void)plugin; g_stats.render_mode = mode; return true; }
static uint32_t tail_get(const clap_plugin_t *plugin) { (void)plugin; return g_stats.tail; }

static bool write_text(const clap_ostream_t *stream, const char *text)
{
    const int64_t n = (int64_t)strlen(text);

    return stream->write(stream, text, (uint64_t)n) == n;
}

static bool state_save(const clap_plugin_t *plugin, const clap_ostream_t *stream) { (void)plugin; return write_text(stream, "plain"); }
static bool state_load(const clap_plugin_t *plugin, const clap_istream_t *stream) { (void)plugin; (void)stream; g_stats.plain_loads++; return true; }

static bool context_save(const clap_plugin_t *plugin, const clap_ostream_t *stream, uint32_t context)
{
    (void)plugin;
    return write_text(stream, context == CLAP_STATE_CONTEXT_FOR_PRESET ? "preset" : context == CLAP_STATE_CONTEXT_FOR_DUPLICATE ? "duplicate" : "project");
}

static bool context_load(const clap_plugin_t *plugin, const clap_istream_t *stream, uint32_t context)
{
    (void)plugin;
    (void)stream;
    g_stats.loaded_context = context;
    return true;
}

static uint32_t ports_count(const clap_plugin_t *plugin, bool is_input) { (void)plugin; return is_input ? 1 : 2; }

static bool ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_audio_port_info_t *info)
{
    (void)plugin;
    if (index >= ports_count(plugin, is_input))
        return false;
    memset(info, 0, sizeof(*info));
    info->id = index;
    snprintf(info->name, sizeof(info->name), "%s %u", is_input ? "in" : "out", index);
    info->flags = index == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static bool activation_while_processing(const clap_plugin_t *plugin) { (void)plugin; return g_stats.while_processing != 0; }

static bool activation_set(const clap_plugin_t *plugin, bool is_input, uint32_t index, bool is_active, uint32_t sample_size)
{
    (void)plugin;
    (void)sample_size;
    if (is_input || index != 1)
        return false;
    g_stats.aux_out_set++;
    g_stats.aux_out_active = is_active;
    return true;
}

static const clap_plugin_audio_ports_t g_ports = { ports_count, ports_get };
static const clap_plugin_timer_support_t g_timers = { on_timer };
static const clap_plugin_posix_fd_support_t g_fds = { on_fd };
static const clap_plugin_thread_pool_t g_pool = { pool_exec };
static const clap_plugin_render_t g_render = { render_hard_rt, render_set };
static const clap_plugin_tail_t g_tail = { tail_get };
static const clap_plugin_state_t g_state = { state_save, state_load };
static const clap_plugin_state_context_t g_context = { context_save, context_load };
static const clap_plugin_audio_ports_activation_t g_activation = { activation_while_processing, activation_set };

static const void *plugin_get_extension(const clap_plugin_t *plugin, const char *id)
{
    (void)plugin;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &g_ports;
    if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT))
        return &g_timers;
    if (!strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT))
        return &g_fds;
    if (!strcmp(id, CLAP_EXT_THREAD_POOL))
        return &g_pool;
    if (!strcmp(id, CLAP_EXT_RENDER))
        return &g_render;
    if (!strcmp(id, CLAP_EXT_TAIL))
        return &g_tail;
    if (!strcmp(id, CLAP_EXT_STATE))
        return &g_state;
    if (!strcmp(id, CLAP_EXT_STATE_CONTEXT))
        return &g_context;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS_ACTIVATION))
        return &g_activation;
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

/* one instance at a time: its stats start over at each create */
static const clap_plugin_t *factory_create(const clap_plugin_factory_t *factory, const clap_host_t *host, const char *id)
{
    (void)factory;
    if (!id || strcmp(id, ID_EXT) != 0 || g_plugin)
        return NULL;
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.render_mode = -1;
    g_stats.status = CLAP_PROCESS_CONTINUE;
    g_host = host;
    g_plugin = &g_the_plugin;
    return g_plugin;
}

static const clap_plugin_factory_t g_factory = { factory_count, factory_descriptor, factory_create };

static bool entry_init(const char *path) { (void)path; return true; }
static void entry_deinit(void) {}
static const void *entry_get_factory(const char *id) { return !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &g_factory : NULL; }

CLAP_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory };
