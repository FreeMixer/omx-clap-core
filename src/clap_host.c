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

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "clap_host.h"
#include "clap_meters.h"
#include "param_contract.h"


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
    OMX_CLAP_CORE_ABI, sizeof(struct omx_clap_host_config), 1, 1, 1, 0, 0, "omx-clap-core", "Pau Aliagas", "https://github.com/FreeMixer/omx-clap-core", "0", 0, 0
};
static int g_configured;
static int g_sealed;        // a binary was opened: the configuration can no longer change


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

/* A foreign .clap: one dlopen per path. */
static struct omx_clap_binary *binary_ref(const char *path, char *reason, size_t reason_size)
{
    struct omx_clap_binary *binary;
    void *so;

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

static int on_audio_role(const struct omx_clap_instance *in)
{
    const pthread_t self = pthread_self();

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
    return false;   // a layout change is a new judgment: a restart into it, never live
}

static void host_ports_rescan(const clap_host_t *host, uint32_t flags)
{
    (void)flags;
    if (main_thread_call(host))
        atomic_store_explicit(&instance_of(host)->ports_rescan_requested, 1u, memory_order_relaxed);
}

static void host_state_mark_dirty(const clap_host_t *host)
{
    if (main_thread_call(host))
        atomic_store_explicit(&instance_of(host)->state_dirty, 1u, memory_order_relaxed);
}

static void host_preset_on_error(const clap_host_t *host, uint32_t location_kind, const char *location, const char *load_key,
                                 int32_t os_error, const char *msg)
{
    char text[CLAP_HOST_LOG_BYTES];

    (void)location_kind;
    (void)load_key;
    (void)os_error;
    if (main_thread_call(host))
    {
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

static const clap_host_note_ports_t g_host_note_ports = { host_note_ports_supported_dialects, host_note_ports_rescan };
static const clap_host_note_name_t g_host_note_name = { host_note_name_changed };
static const clap_host_voice_info_t g_host_voice_info = { host_voice_info_changed };

static const clap_host_track_info_t g_host_track_info = { host_track_info_get };
static const clap_host_remote_controls_t g_host_remote_controls = { host_remote_controls_changed,
                                                                    host_remote_controls_suggest_page };

static const char *const g_extension_ids[] = CLAP_HOST_EXTENSIONS_INIT;

/* only what the declared list offers, and the preset-load extension where the configuration adds it: a verdict
 * promised no more */
static const void *host_get_extension(const clap_host_t *host, const char *id)
{
    size_t i;
    int declared = 0;

    (void)host;
    if (!id)
        return NULL;
    if (g_config.preset_load && (!strcmp(id, CLAP_EXT_PRESET_LOAD) || !strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT)))
        return &g_host_preset_load;
    if (g_config.track_info && (!strcmp(id, CLAP_EXT_TRACK_INFO) || !strcmp(id, CLAP_EXT_TRACK_INFO_COMPAT)))
        return &g_host_track_info;
    if (g_config.remote_controls && (!strcmp(id, CLAP_EXT_REMOTE_CONTROLS) || !strcmp(id, CLAP_EXT_REMOTE_CONTROLS_COMPAT)))
        return &g_host_remote_controls;
    for (i = 0; g_extension_ids[i]; i++)
        declared |= strcmp(g_extension_ids[i], id) == 0;
    if (!declared)
        return NULL;
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
    return NULL;
}

static void host_request_restart(const clap_host_t *host)
{
    atomic_store_explicit(&instance_of(host)->restart_requested, 1u, memory_order_relaxed);
}

static void host_request_process(const clap_host_t *host)
{
    (void)host;     // a published plugin is always processed
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

/*
 * The port layout, as the strip's topology reads it: one main output and one main input of the same width, 1 or 2
 * channels; an output besides them left unconnected (up to CLAP_HOST_AUX_OUTPUTS, each of 1 or 2 channels); an input
 * besides them a side chain, fed silence until the caller binds it (up to CLAP_HOST_AUX_INPUTS, each of 1 or 2 channels);
 * no main input at all is a source (an instrument with a note input, a generator without one, #184), its output its own;
 * where the configuration admits note inputs, one note input; otherwise no note input. Returns the hosting code that
 * refuses, or NULL.
 */
static const char *read_topology(struct omx_clap_instance *in)
{
    const clap_plugin_audio_ports_t *ports = in->audio_ports;
    uint32_t main_in = 0, main_out = 0, n_main_in = 0, n_main_out = 0;
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
            uint32_t i;

            for (i = 0; i < count; i++)
            {
                clap_audio_port_info_t info;

                memset(&info, 0, sizeof(info));
                if (!ports->get(in->plugin, i, is_input, &info))
                    continue;
                if (info.flags & CLAP_AUDIO_PORT_IS_MAIN)
                {
                    if (is_input)
                        main_in = info.channel_count, n_main_in++;
                    else
                        main_out = info.channel_count, n_main_out++;
                }
                else if (is_input)
                {
                    if (info.channel_count == 0 || info.channel_count > 2)
                        return CLAP_HOST_CODE_WIDER_THAN_STRIP;
                    if (in->aux_inputs >= CLAP_HOST_AUX_INPUTS)
                        return CLAP_HOST_CODE_EXTRA_INPUTS;
                    in->aux_in_channels[in->aux_inputs++] = info.channel_count;
                }
                else if (info.channel_count == 0 || info.channel_count > 2 || in->aux_outputs >= CLAP_HOST_AUX_OUTPUTS)
                {
                    return CLAP_HOST_CODE_WIDER_THAN_STRIP;
                }
                else
                {
                    in->aux_channels[in->aux_outputs++] = info.channel_count;
                }
            }
        }
    }
    if (n_main_out == 0 || main_out == 0)
        return CLAP_HOST_CODE_NO_AUDIO_OUTPUT;
    if (main_in > CLAP_HOST_MAIN_PORT_CHANNELS || main_out > CLAP_HOST_MAIN_PORT_CHANNELS || n_main_in > 1 || n_main_out > 1)
        return CLAP_HOST_CODE_WIDER_THAN_STRIP;
    if (n_main_in && main_in != main_out)
        return CLAP_HOST_CODE_WIDER_THAN_STRIP;
    if (!g_config.note_inputs && in->note_ports && in->note_ports->count(in->plugin, true) > 0)
        return CLAP_HOST_CODE_NOTE_INPUT;
    in->channels = main_out;
    in->in_channels = n_main_in ? main_in : 0;
    in->note_outputs = in->note_ports ? in->note_ports->count(in->plugin, false) : 0;
    return NULL;
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
    in->plugin = bin->factory->create_plugin(bin->factory, &in->host, desc->id);
    if (!in->plugin || !in->plugin->init(in->plugin))
    {
        if (in->plugin)
            in->plugin->destroy(in->plugin);
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
    in->preset_load = in->plugin->get_extension(in->plugin, CLAP_EXT_PRESET_LOAD);
    if (!in->preset_load)
        in->preset_load = in->plugin->get_extension(in->plugin, CLAP_EXT_PRESET_LOAD_COMPAT);
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

/* The one judgment every door shares: descriptor, feature, create and init, extensions, ports. Takes over the caller's
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
    if (!omx_clap_host_has_feature(desc, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT)
        && !(g_config.note_inputs && omx_clap_host_has_feature(desc, CLAP_PLUGIN_FEATURE_INSTRUMENT)))
    {
        binary_unref(bin);
        why_set(why, CLAP_HOST_CODE_NOT_AUDIO_EFFECT);
        return -1;
    }
    if (instance_create(bin, desc, &in) != 0)
    {
        why_set(why, CLAP_HOST_CODE_HEADLESS_FAILED);
        return -1;
    }
    refused = read_topology(in);
    if (refused)
    {
        in->plugin->destroy(in->plugin);
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

/* Eight buffers of max_block floats in one mapping with a guard page at each end: the silence pair the unbound auxiliary
 * inputs read, the scratch pair for the auxiliary outputs, then the stage's four, the last of them flush against the
 * guard page above, so the first sample written past a block faults here, in this mapping, and never lands in a foreign
 * buffer. */
static int take_bounce(struct omx_clap_instance *in, uint32_t max_block)
{
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    const size_t used = (size_t)8 * max_block * sizeof(float);
    const size_t body = (used + page - 1) / page * page;
    const size_t length = body + 2 * page;
    uint8_t *map = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    struct omx_hosted_bounce bounce;
    float *b;

    if (map == MAP_FAILED)
        return -1;
    mprotect(map, page, PROT_NONE);
    mprotect(map + page + body, page, PROT_NONE);
    in->bounce_map = (float *)map;
    in->bounce_map_len = length;
    b = (float *)(map + page + (body - used));
    in->rec_cap = CLAP_HOST_PARAM_QUEUE_DEPTH;
    in->recs = calloc(in->rec_cap, sizeof(*in->recs));
    in->max_block = max_block;
    bounce.in_l = b + 4 * (size_t)max_block;
    bounce.in_r = b + 5 * (size_t)max_block;
    bounce.out_l = b + 6 * (size_t)max_block;
    bounce.out_r = b + 7 * (size_t)max_block;
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
    if (omx_clap_bind_aux(&in->stage, b + 2 * (size_t)max_block, b + 3 * (size_t)max_block, in->aux_outputs, in->aux_channels) != 0)
        return -1;
    return omx_clap_bind_aux_in(&in->aux_in, b, b + max_block, in->aux_inputs, in->aux_in_channels, in->aux_in_bound);
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
    if (omx_clap_bind_ports_aux(&in->stage, in->plugin, in->in_channels, in->channels, &in->aux_in) != 0)
        return -1;
    omx_clap_note_ids_bind(&in->note_ids, &in->stage, (uint32_t)lround(in->rate * OMX_CLAP_NOTE_ID_HOLD_MS / 1000.0));
    return 0;
}

static void publish_latency(struct omx_clap_instance *in)
{
    omx_clap_publish_latency(&in->stage, in->latency ? in->latency->get(in->plugin) : 0u);
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

void omx_clap_host_config_default(struct omx_clap_host_config *config)
{
    static const struct omx_clap_host_config defaults =
    {
        OMX_CLAP_CORE_ABI, sizeof(struct omx_clap_host_config), 1, 1, 1, 0, 0, "omx-clap-core", "Pau Aliagas", "https://github.com/FreeMixer/omx-clap-core", "0", 0, 0
    };

    *config = defaults;
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

struct omx_clap_binary *omx_clap_host_binary_open(const char *path, char *reason, size_t reason_size)
{
    return binary_ref(path, reason, reason_size);
}

void omx_clap_host_binary_close(struct omx_clap_binary *binary)
{
    binary_unref(binary);
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

int omx_clap_host_activate(struct omx_clap_instance *in, double rate, uint32_t max_block, char why[OMX_CLAP_WHY_MAX])
{
    struct omx_clap_role role;
    uint32_t bad;

    if (!in || in->active || max_block == 0)
        return -1;
    for (bad = 0; bad < in->aux_inputs; bad++)
        if (in->aux_in_bound[bad][0] && in->aux_in_frames[bad] < max_block)
        {
            why_set(why, CLAP_HOST_CODE_HEADLESS_FAILED);       // a bound pair shorter than the block: rebind it first
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
        in->plugin->deactivate(in->plugin);
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
    publish_latency(in);
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

int omx_clap_host_restart(struct omx_clap_instance *in)
{
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
    if (omx_clap_host_activate(in, rate, max_block, NULL) != 0)
        return -1;
    in->audio_thread = thread;
    in->audio_role_held = held;
    omx_clap_arm(&in->stage);
    return 0;
}

void omx_clap_host_deactivate(struct omx_clap_instance *in)
{
    uint32_t i;

    if (!in || !in->active)
        return;
    in->plugin->deactivate(in->plugin);
    in->active = 0;
    drop_bounce(in);
    memset(&in->stage, 0, sizeof(in->stage));
    for (i = 0; i < in->shadow_n; i++)
        in->shadow[i].valid = 0;
    in->shadow_n = 0;
}

int omx_clap_host_set_rate(struct omx_clap_instance *in, double rate, uint32_t max_block, char why[OMX_CLAP_WHY_MAX])
{
    if (!in)
        return -1;
    if (in->active && !omx_clap_stopped(&in->stage))
        return -1;      // unpublish first
    omx_clap_host_deactivate(in);
    return omx_clap_host_activate(in, rate, max_block, why);
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

int omx_clap_host_bind_aux_input(struct omx_clap_instance *in, uint32_t port, float *l, float *r, uint32_t frames)
{
    if (!in || port >= in->aux_inputs)
        return -1;
    if (l && in->aux_in_channels[port] == 2 && r == NULL)
        return -1;
    if (in->active && !omx_clap_stopped(&in->stage))
        return -1;      // unpublish first
    if (l && in->active && frames < in->max_block)
        return -1;      // a block would read past the pair
    in->aux_in_bound[port][0] = l;
    in->aux_in_bound[port][1] = l ? r : NULL;
    in->aux_in_frames[port] = l ? frames : 0;
    if (in->active)
        omx_clap_set_aux_in(&in->aux_in, port, l, in->aux_in_bound[port][1]);
    return 0;
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

int omx_clap_host_preset_load(struct omx_clap_instance *in, const char *location)
{
    if (!in->preset_load)
        return -1;
    return in->preset_load->from_location(in->plugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE, location, NULL) ? 0 : -1;
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
    if (atomic_exchange_explicit(&in->callback_requested, 0u, memory_order_relaxed) && in->plugin->on_main_thread)
        in->plugin->on_main_thread(in->plugin);
    if (atomic_exchange_explicit(&in->log_pending, 0u, memory_order_acquire))
    {
        snprintf(in->last_log, sizeof(in->last_log), "%s", in->log_ring);
        atomic_store_explicit(&in->log_fresh, 1u, memory_order_release);
    }
    if (atomic_exchange_explicit(&in->latency_changed, 0u, memory_order_relaxed) && in->active && in->latency)
        publish_latency(in);
    return atomic_load_explicit(&in->restart_requested, memory_order_relaxed) != 0;
}

const char *omx_clap_host_log_take(struct omx_clap_instance *in)
{
    return atomic_exchange_explicit(&in->log_fresh, 0u, memory_order_acquire) ? in->last_log : NULL;
}
