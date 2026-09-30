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
************************************************************************************************************************
*/


/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "clap_host.h"
#include "host-errors.h"


/*
************************************************************************************************************************
*           LOCAL DEFINES
************************************************************************************************************************
*/

#define QUEUE_SETTLE_US         50000


/*
************************************************************************************************************************
*           LOCAL DATA TYPES
************************************************************************************************************************
*/

/* one dlopen per file, shared by its instances */
struct CLAP_BINARY_T {
    char *path;
    void *handle;
    const clap_plugin_entry_t *entry;
    const clap_plugin_factory_t *factory;
    uint32_t refs;
    clap_binary_t *next;
};

/* what the control thread displaces while it holds the audio role */
typedef struct ROLE_T {
    uint32_t state;
    pthread_t thread;
    int held;
} role_t;

typedef struct STREAM_T {
    uint8_t *buffer;
    size_t capacity;
    size_t length;
    size_t position;
    int overflow;
} stream_t;


/*
************************************************************************************************************************
*           LOCAL GLOBAL VARIABLES
************************************************************************************************************************
*/

static clap_binary_t *g_binaries;


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS
************************************************************************************************************************
*/

static clap_binary_t *binary_ref(const char *path, char *reason, size_t reason_size)
{
    clap_binary_t *binary;
    void *handle;
    const clap_plugin_entry_t *entry;
    const clap_plugin_factory_t *factory;

    for (binary = g_binaries; binary; binary = binary->next)
    {
        if (strcmp(binary->path, path) == 0)
        {
            binary->refs++;
            return binary;
        }
    }

    handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle)
    {
        snprintf(reason, reason_size, "can't open %s: %s", path, dlerror());
        return NULL;
    }

    entry = dlsym(handle, "clap_entry");
    if (!entry || !clap_version_is_compatible(entry->clap_version) || !entry->init || !entry->init(path))
    {
        snprintf(reason, reason_size, "can't init %s", path);
        dlclose(handle);
        return NULL;
    }

    factory = entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (!factory)
    {
        snprintf(reason, reason_size, "no plugin factory in %s", path);
        entry->deinit();
        dlclose(handle);
        return NULL;
    }

    binary = calloc(1, sizeof(clap_binary_t));
    binary->path = strdup(path);
    binary->handle = handle;
    binary->entry = entry;
    binary->factory = factory;
    binary->refs = 1;
    binary->next = g_binaries;
    g_binaries = binary;
    return binary;
}

static void binary_unref(clap_binary_t *binary)
{
    clap_binary_t **link;

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
    dlclose(binary->handle);
    free(binary->path);
    free(binary);
}

static clap_instance_t *instance_of(const clap_host_t *host)
{
    return host->host_data;
}

static int on_audio_thread(const clap_instance_t *instance)
{
    return instance->audio_role_held && pthread_equal(pthread_self(), instance->audio_thread);
}

/* a [main-thread] host call made from the audio thread is counted and ignored */
static int main_thread_call(const clap_host_t *host)
{
    clap_instance_t *instance = instance_of(host);

    if (on_audio_thread(instance))
    {
        atomic_fetch_add(&instance->thread_violations, 1);
        return 0;
    }
    return 1;
}

static void host_log(const clap_host_t *host, clap_log_severity severity, const char *msg)
{
    clap_instance_t *instance = instance_of(host);
    size_t i;

    (void)severity;

    if (msg)
    {
        for (i = 0; i < sizeof(instance->log_slot) - 1 && msg[i]; i++)
            instance->log_slot[i] = msg[i];
        instance->log_slot[i] = '\0';
    }
    atomic_store(&instance->log_pending, 1);
}

static bool host_is_main_thread(const clap_host_t *host)
{
    clap_instance_t *instance = instance_of(host);

    if (on_audio_thread(instance))
        return false;
    return pthread_equal(pthread_self(), instance->main_thread);
}

static bool host_is_audio_thread(const clap_host_t *host)
{
    return on_audio_thread(instance_of(host));
}

static void host_latency_changed(const clap_host_t *host)
{
    if (main_thread_call(host))
        atomic_store(&instance_of(host)->latency_changed, 1);
}

static void host_params_rescan(const clap_host_t *host, clap_param_rescan_flags flags)
{
    (void)flags;
    main_thread_call(host);
}

static void host_params_clear(const clap_host_t *host, clap_id id, clap_param_clear_flags flags)
{
    (void)id;
    (void)flags;
    main_thread_call(host);
}

static void host_params_request_flush(const clap_host_t *host)
{
    atomic_store(&instance_of(host)->flush_requested, 1);
}

static bool host_ports_is_rescan_flag_supported(const clap_host_t *host, uint32_t flag)
{
    (void)host;
    (void)flag;
    return false;
}

static void host_ports_rescan(const clap_host_t *host, uint32_t flags)
{
    (void)flags;
    main_thread_call(host);
}

static void host_state_mark_dirty(const clap_host_t *host)
{
    if (main_thread_call(host))
        atomic_store(&instance_of(host)->state_dirty, 1);
}

static void host_preset_on_error(const clap_host_t *host, uint32_t location_kind, const char *location,
                                 const char *load_key, int32_t os_error, const char *msg)
{
    (void)location_kind;
    (void)load_key;
    (void)os_error;

    if (main_thread_call(host))
        fprintf(stderr, "preset %s: %s\n", location ? location : "", msg ? msg : "error");
}

static void host_preset_loaded(const clap_host_t *host, uint32_t location_kind, const char *location,
                               const char *load_key)
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

static const void *host_get_extension(const clap_host_t *host, const char *id)
{
    (void)host;

    if (!id)
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
    if (!strcmp(id, CLAP_EXT_PRESET_LOAD) || !strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT))
        return &g_host_preset_load;
    return NULL;
}

static void host_request_restart(const clap_host_t *host)
{
    atomic_store(&instance_of(host)->restart_requested, 1);
}

static void host_request_process(const clap_host_t *host)
{
    (void)host;
}

static void host_request_callback(const clap_host_t *host)
{
    atomic_store(&instance_of(host)->callback_requested, 1);
}

static uint32_t notes_in_view(const clap_instance_t *instance)
{
    return atomic_load_explicit(&instance->notes_visible, memory_order_acquire) ? instance->notes_count : 0;
}

/* the parameter events, then the notes of the cycle when the plugin is inside process() */
static uint32_t in_events_size(const clap_input_events_t *list)
{
    const clap_instance_t *instance = list->ctx;

    return instance->events_count + notes_in_view(instance);
}

static const clap_event_header_t *in_events_get(const clap_input_events_t *list, uint32_t index)
{
    clap_instance_t *instance = list->ctx;

    if (index < instance->events_count)
        return &instance->events[index].header;
    index -= instance->events_count;
    if (index >= notes_in_view(instance))
        return NULL;
    return &instance->notes[index].header;
}

static bool out_events_try_push(const clap_output_events_t *list, const clap_event_header_t *event)
{
    (void)list;
    (void)event;
    return true;
}

static int descriptor_has_feature(const clap_plugin_descriptor_t *desc, const char *feature)
{
    const char *const *f;

    if (!desc->features)
        return 0;
    for (f = desc->features; *f; f++)
        if (strcmp(*f, feature) == 0)
            return 1;
    return 0;
}

/* the note input, when the plugin has one: how it is fed, the reason when it can't be */
static int read_note_input(clap_instance_t *instance, char *reason, size_t reason_size)
{
    const clap_plugin_note_ports_t *ports = instance->note_ports;
    clap_note_port_info_t info;
    uint32_t count = ports ? ports->count(instance->plugin, true) : 0;

    if (count == 0)
        return 0;
    if (count > 1)
    {
        snprintf(reason, reason_size, "%u note inputs", count);
        return -1;
    }
    memset(&info, 0, sizeof(info));
    if (!ports->get(instance->plugin, 0, true, &info))
    {
        snprintf(reason, reason_size, "note input 0 unreadable");
        return -1;
    }
    if (info.preferred_dialect == CLAP_NOTE_DIALECT_MIDI && (info.supported_dialects & CLAP_NOTE_DIALECT_MIDI))
        instance->note_dialect = CLAP_NOTE_DIALECT_MIDI;
    else if (info.supported_dialects & CLAP_NOTE_DIALECT_CLAP)
        instance->note_dialect = CLAP_NOTE_DIALECT_CLAP;
    else if (info.supported_dialects & CLAP_NOTE_DIALECT_MIDI)
        instance->note_dialect = CLAP_NOTE_DIALECT_MIDI;
    else
    {
        snprintf(reason, reason_size, "note input reads neither the CLAP nor the MIDI dialect");
        return -1;
    }
    instance->note_inputs = 1;
    return 0;
}

/* one main output, mono or stereo, and one main input of the same kind unless a note input feeds the plugin
 * instead; nothing else; the reason when not */
static int read_topology(clap_instance_t *instance, char *reason, size_t reason_size)
{
    const clap_plugin_audio_ports_t *ports = instance->audio_ports;
    uint32_t main_inputs = 0, main_outputs = 0, others = 0;
    clap_audio_port_info_t info;
    uint32_t count, i;
    int dir;

    if (!ports)
    {
        snprintf(reason, reason_size, "no audio-ports extension");
        return -1;
    }

    for (dir = 0; dir < 2; dir++)
    {
        const bool is_input = dir == 0;
        count = ports->count(instance->plugin, is_input);
        for (i = 0; i < count; i++)
        {
            memset(&info, 0, sizeof(info));
            if (!ports->get(instance->plugin, i, is_input, &info))
            {
                snprintf(reason, reason_size, "%s port %u unreadable", is_input ? "input" : "output", i);
                return -1;
            }
            if (!(info.flags & CLAP_AUDIO_PORT_IS_MAIN))
            {
                others++;
                continue;
            }
            if (info.channel_count == 0 || info.channel_count > CLAP_HOST_MAIN_PORT_CHANNELS)
            {
                snprintf(reason, reason_size, "main port has %u channels", info.channel_count);
                return -1;
            }
            if (is_input)
            {
                instance->input_channels = info.channel_count;
                main_inputs++;
            }
            else
            {
                instance->output_channels = info.channel_count;
                main_outputs++;
            }
        }
    }

    if (others != 0)
    {
        snprintf(reason, reason_size, "%u sidechain/aux ports", others);
        return -1;
    }
    if (read_note_input(instance, reason, reason_size) != 0)
        return -1;
    if (main_outputs != 1 || main_inputs > 1 || (main_inputs == 0 && !instance->note_inputs))
    {
        snprintf(reason, reason_size, "%u main inputs, %u main outputs", main_inputs, main_outputs);
        return -1;
    }
    return 0;
}

static int param_info(clap_instance_t *instance, clap_id id, clap_param_info_t *info)
{
    uint32_t count, i;

    if (!instance->params)
        return -1;

    count = instance->params->count(instance->plugin);
    for (i = 0; i < count; i++)
    {
        memset(info, 0, sizeof(*info));
        if (instance->params->get_info(instance->plugin, i, info) && info->id == id)
            return 0;
    }
    return -1;
}

static int queue_push(clap_param_queue_t *queue, clap_id id, double value, void *cookie)
{
    const uint32_t tail = atomic_load_explicit(&queue->tail, memory_order_relaxed);
    const uint32_t head = atomic_load_explicit(&queue->head, memory_order_acquire);
    clap_param_record_t *record;

    if (tail - head >= CLAP_HOST_PARAM_QUEUE_DEPTH)
        return -1;

    record = &queue->records[tail & (CLAP_HOST_PARAM_QUEUE_DEPTH - 1)];
    record->id = id;
    record->value = value;
    record->cookie = cookie;
    atomic_store_explicit(&queue->tail, tail + 1, memory_order_release);
    return 0;
}

static void drain_events(clap_instance_t *instance)
{
    clap_param_queue_t *queue = &instance->queue;
    uint32_t head = atomic_load_explicit(&queue->head, memory_order_relaxed);
    const uint32_t tail = atomic_load_explicit(&queue->tail, memory_order_acquire);
    uint32_t n = 0;

    while (head != tail && n < CLAP_HOST_EVENTS_PER_BLOCK)
    {
        const clap_param_record_t *record = &queue->records[head & (CLAP_HOST_PARAM_QUEUE_DEPTH - 1)];
        clap_event_param_value_t *event = &instance->events[n++].param;

        event->header.size = sizeof(*event);
        event->header.time = 0;
        event->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        event->header.type = CLAP_EVENT_PARAM_VALUE;
        event->header.flags = 0;
        event->param_id = record->id;
        event->cookie = record->cookie;
        event->note_id = -1;
        event->port_index = -1;
        event->channel = -1;
        event->key = -1;
        event->value = record->value;
        head++;
    }

    atomic_store_explicit(&queue->head, head, memory_order_release);
    instance->events_count = n;
    if (n)
        atomic_fetch_add_explicit(&instance->events_delivered, n, memory_order_relaxed);
}

/* the control thread stands in for the audio thread: a cycle that arrives meanwhile passes the input through,
 * one already running is waited for */
static role_t take_role(clap_instance_t *instance)
{
    role_t role;
    uint32_t state = atomic_load(&instance->run_state);
    unsigned waited = 0;

    while (state != CLAP_HOST_IDLE && state != CLAP_HOST_STOPPED && state != CLAP_HOST_HELD
           && !atomic_compare_exchange_weak(&instance->run_state, &state, CLAP_HOST_HELD))
        ;
    while (atomic_load(&instance->in_cycle) && waited < CLAP_HOST_ROLE_TIMEOUT_US)
    {
        usleep(CLAP_HOST_ROLE_POLL_US);
        waited += CLAP_HOST_ROLE_POLL_US;
    }

    role.state = state;
    role.thread = instance->audio_thread;
    role.held = instance->audio_role_held;
    instance->audio_thread = pthread_self();
    instance->audio_role_held = 1;
    return role;
}

static void release_role(clap_instance_t *instance, const role_t *role, uint32_t state)
{
    uint32_t held = CLAP_HOST_HELD;

    instance->audio_thread = role->thread;
    instance->audio_role_held = role->held;
    atomic_compare_exchange_strong(&instance->run_state, &held, state);
}

static int queue_pending(const clap_instance_t *instance)
{
    return atomic_load(&instance->queue.tail) != atomic_load(&instance->queue.head);
}

/* the queued writes reach the plugin through params.flush when no cycle runs them */
static void flush_events(clap_instance_t *instance)
{
    role_t role;

    if (!instance->params || !instance->params->flush)
        return;

    role = take_role(instance);
    while (queue_pending(instance))
    {
        drain_events(instance);
        instance->params->flush(instance->plugin, &instance->in_events, &instance->out_events);
        instance->events_count = 0;
    }
    release_role(instance, &role, role.state);
}

/* a write waits for the next cycle; when none comes (a client nothing drives), the control thread delivers it */
static void settle_queue(clap_instance_t *instance)
{
    const uint32_t runs = atomic_load(&instance->runs);
    unsigned waited = 0;

    while (queue_pending(instance) && waited < QUEUE_SETTLE_US)
    {
        usleep(CLAP_HOST_ROLE_POLL_US);
        waited += CLAP_HOST_ROLE_POLL_US;
    }
    if (queue_pending(instance) && atomic_load(&instance->runs) == runs)
        flush_events(instance);
}

static int push_or_flush(clap_instance_t *instance, clap_id id, double value, void *cookie)
{
    uint32_t state;

    if (queue_push(&instance->queue, id, value, cookie) != 0)
        return ERR_INVALID_OPERATION;
    state = atomic_load(&instance->run_state);
    if (state == CLAP_HOST_IDLE || state == CLAP_HOST_ARMED)
        flush_events(instance);
    return SUCCESS;
}

static void free_buffers(clap_instance_t *instance)
{
    uint32_t c;

    for (c = 0; c < CLAP_HOST_MAIN_PORT_CHANNELS; c++)
    {
        free(instance->input_buffers[c]);
        free(instance->output_buffers[c]);
        instance->input_buffers[c] = NULL;
        instance->output_buffers[c] = NULL;
    }
    free(instance->silence);
    instance->silence = NULL;
}

static int64_t stream_write(const clap_ostream_t *stream, const void *data, uint64_t size)
{
    stream_t *s = stream->ctx;

    if (s->length + size > s->capacity)
    {
        s->overflow = 1;
        return -1;
    }
    memcpy(s->buffer + s->length, data, size);
    s->length += size;
    return (int64_t)size;
}

static int64_t stream_read(const clap_istream_t *stream, void *data, uint64_t size)
{
    stream_t *s = stream->ctx;
    const size_t left = s->length - s->position;
    const size_t n = size < left ? (size_t)size : left;

    memcpy(data, s->buffer + s->position, n);
    s->position += n;
    return (int64_t)n;
}

static int process_cycle(clap_instance_t *instance, uint32_t nframes)
{
    clap_process_status status;
    uint32_t c, i;
    uint64_t mask;

    drain_events(instance);
    instance->audio_in.constant_mask = 0;
    instance->audio_out.constant_mask = 0;
    instance->process.steady_time = instance->steady_time;
    instance->process.frames_count = nframes;

    atomic_store_explicit(&instance->notes_visible, 1, memory_order_release);
    status = instance->plugin->process(instance->plugin, &instance->process);
    atomic_store_explicit(&instance->notes_visible, 0, memory_order_release);
    clap_host_denormals_off();
    atomic_fetch_add_explicit(&instance->notes_delivered, instance->notes_count, memory_order_relaxed);

    instance->steady_time += nframes;
    instance->events_count = 0;
    atomic_fetch_add_explicit(&instance->runs, 1, memory_order_relaxed);

    if (status == CLAP_PROCESS_ERROR)
    {
        atomic_fetch_add_explicit(&instance->process_errors, 1, memory_order_relaxed);
        return 0;
    }

    // a constant output channel holds its value in sample 0 only
    mask = instance->audio_out.constant_mask;
    for (c = 0; mask && c < instance->output_channels; c++)
    {
        float *buffer = instance->output_buffers[c];
        if (!(mask & ((uint64_t)1 << c)))
            continue;
        for (i = 1; i < nframes; i++)
            buffer[i] = buffer[0];
        atomic_fetch_add_explicit(&instance->constant_channels, 1, memory_order_relaxed);
    }
    return 1;
}

static void pass_dry(const clap_instance_t *instance, const float *const *inputs, float *const *outputs, uint32_t nframes)
{
    uint32_t c;

    for (c = 0; c < instance->output_channels; c++)
    {
        const uint32_t in = c < instance->input_channels ? c : instance->input_channels - 1;
        if (!instance->input_channels)
            memset(outputs[c], 0, sizeof(float) * nframes);
        else if (outputs[c] != inputs[in])
            memcpy(outputs[c], inputs[in], sizeof(float) * nframes);
    }
}

static void crossfade(float *dst, const float *from, const float *to, uint32_t nframes)
{
    uint32_t i;

    for (i = 0; i < nframes; i++)
    {
        const float gain = (1.0f / (float)nframes) * (float)i;
        dst[i] = from[i] + (to[i] - from[i]) * gain;
    }
}

static void deliver(clap_instance_t *instance, const float *const *inputs, float *const *outputs, uint32_t nframes, int want_wet)
{
    uint32_t c;

    for (c = 0; c < instance->output_channels; c++)
    {
        const uint32_t in = c < instance->input_channels ? c : instance->input_channels - 1;
        const float *wet = instance->output_buffers[c];
        const float *dry = instance->input_channels ? inputs[in] : instance->silence;

        if (want_wet && instance->rendered_wet)
            memcpy(outputs[c], wet, sizeof(float) * nframes);
        else if (want_wet)
            crossfade(outputs[c], dry, wet, nframes);
        else
            crossfade(outputs[c], wet, dry, nframes);
    }
    instance->rendered_wet = want_wet;
}

static int wait_stopped(clap_instance_t *instance)
{
    uint32_t expected = CLAP_HOST_PROCESSING;
    unsigned waited = 0;

    if (!atomic_compare_exchange_strong(&instance->run_state, &expected, CLAP_HOST_STOPPING))
    {
        expected = CLAP_HOST_ARMED;
        atomic_compare_exchange_strong(&instance->run_state, &expected, CLAP_HOST_STOPPED);
    }

    while (atomic_load(&instance->run_state) == CLAP_HOST_STOPPING)
    {
        if (waited >= CLAP_HOST_ROLE_TIMEOUT_US)
        {
            // no cycle came to stop it
            role_t role = take_role(instance);
            if (role.state == CLAP_HOST_STOPPING && instance->plugin->stop_processing)
                instance->plugin->stop_processing(instance->plugin);
            release_role(instance, &role, CLAP_HOST_STOPPED);
            return 0;
        }
        usleep(CLAP_HOST_ROLE_POLL_US);
        waited += CLAP_HOST_ROLE_POLL_US;
    }
    return 0;
}


/*
************************************************************************************************************************
*           GLOBAL FUNCTIONS
************************************************************************************************************************
*/

clap_binary_t *clap_host_binary_open(const char *path, char *reason, size_t reason_size)
{
    return binary_ref(path, reason, reason_size);
}

void clap_host_binary_close(clap_binary_t *binary)
{
    binary_unref(binary);
}

uint32_t clap_host_binary_count(const clap_binary_t *binary)
{
    return binary->factory->get_plugin_count(binary->factory);
}

const clap_plugin_descriptor_t *clap_host_binary_descriptor(const clap_binary_t *binary, uint32_t index)
{
    return binary->factory->get_plugin_descriptor(binary->factory, index);
}

/* the instance holds a reference of its own on the binary until clap_host_close */
int clap_host_create(clap_binary_t *binary, const clap_plugin_descriptor_t *desc, clap_instance_t **out)
{
    clap_instance_t *instance;

    *out = NULL;

    instance = calloc(1, sizeof(clap_instance_t));
    instance->binary = binary;
    instance->desc = desc;
    instance->main_thread = pthread_self();
    instance->host.clap_version = (clap_version_t)CLAP_VERSION_INIT;
    instance->host.host_data = instance;
    instance->host.name = "omx-clap-host";
    instance->host.vendor = "Pau Aliagas";
    instance->host.url = "";
    instance->host.version = "0";
    instance->host.get_extension = host_get_extension;
    instance->host.request_restart = host_request_restart;
    instance->host.request_process = host_request_process;
    instance->host.request_callback = host_request_callback;

    instance->plugin = binary->factory->create_plugin(binary->factory, &instance->host, desc->id);
    if (!instance->plugin || !instance->plugin->init(instance->plugin))
    {
        fprintf(stderr, "can't init plugin %s\n", desc->id);
        if (instance->plugin)
            instance->plugin->destroy(instance->plugin);
        free(instance);
        return ERR_LV2_INSTANTIATION;
    }
    binary->refs++;

    instance->params = instance->plugin->get_extension(instance->plugin, CLAP_EXT_PARAMS);
    instance->latency = instance->plugin->get_extension(instance->plugin, CLAP_EXT_LATENCY);
    instance->audio_ports = instance->plugin->get_extension(instance->plugin, CLAP_EXT_AUDIO_PORTS);
    instance->note_ports = instance->plugin->get_extension(instance->plugin, CLAP_EXT_NOTE_PORTS);
    instance->state = instance->plugin->get_extension(instance->plugin, CLAP_EXT_STATE);
    instance->preset_load = instance->plugin->get_extension(instance->plugin, CLAP_EXT_PRESET_LOAD);
    if (!instance->preset_load)
        instance->preset_load = instance->plugin->get_extension(instance->plugin, CLAP_EXT_PRESET_LOAD_COMPAT);

    *out = instance;
    return SUCCESS;
}

int clap_host_open(const char *path, const char *id, clap_instance_t **out)
{
    clap_binary_t *binary;
    const clap_plugin_descriptor_t *desc = NULL;
    clap_instance_t *instance;
    char reason[256];
    uint32_t count, i;
    int ret;

    *out = NULL;

    binary = binary_ref(path, reason, sizeof(reason));
    if (!binary)
    {
        fprintf(stderr, "%s\n", reason);
        return ERR_LV2_INVALID_URI;
    }

    count = binary->factory->get_plugin_count(binary->factory);
    for (i = 0; i < count; i++)
    {
        const clap_plugin_descriptor_t *d = binary->factory->get_plugin_descriptor(binary->factory, i);
        if (d && d->id && strcmp(d->id, id) == 0)
        {
            desc = d;
            break;
        }
    }
    if (!desc)
    {
        fprintf(stderr, "no plugin %s in %s\n", id, path);
        binary_unref(binary);
        return ERR_LV2_INVALID_URI;
    }
    if (!descriptor_has_feature(desc, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT) && !descriptor_has_feature(desc, CLAP_PLUGIN_FEATURE_INSTRUMENT))
    {
        fprintf(stderr, "%s is neither an audio effect nor an instrument\n", id);
        binary_unref(binary);
        return ERR_LV2_INSTANTIATION;
    }

    ret = clap_host_create(binary, desc, &instance);
    binary_unref(binary);
    if (ret != SUCCESS)
        return ret;

    if (read_topology(instance, reason, sizeof(reason)) != 0)
    {
        fprintf(stderr, "%s: unsupported port layout: %s\n", id, reason);
        clap_host_close(instance);
        return ERR_LV2_INSTANTIATION;
    }

    instance->in_events.ctx = instance;
    instance->in_events.size = in_events_size;
    instance->in_events.get = in_events_get;
    instance->out_events.ctx = instance;
    instance->out_events.try_push = out_events_try_push;

    *out = instance;
    return SUCCESS;
}

int clap_host_activate(clap_instance_t *instance, double sample_rate, uint32_t max_frames)
{
    uint32_t c;

    if (instance->active || max_frames == 0)
        return ERR_INVALID_OPERATION;

    for (c = 0; c < instance->input_channels; c++)
        instance->input_buffers[c] = calloc(max_frames, sizeof(float));
    for (c = 0; c < instance->output_channels; c++)
        instance->output_buffers[c] = calloc(max_frames, sizeof(float));
    instance->silence = calloc(max_frames, sizeof(float));

    if (!instance->plugin->activate(instance->plugin, sample_rate, 1, max_frames))
    {
        free_buffers(instance);
        return ERR_LV2_INSTANTIATION;
    }

    instance->active = 1;
    instance->sample_rate = sample_rate;
    instance->max_frames = max_frames;
    instance->steady_time = 0;
    instance->rendered_wet = 0;
    instance->events_count = 0;
    instance->notes_count = 0;

    instance->audio_in.data32 = instance->input_buffers;
    instance->audio_in.data64 = NULL;
    instance->audio_in.channel_count = instance->input_channels;
    instance->audio_in.latency = 0;
    instance->audio_in.constant_mask = 0;
    instance->audio_out.data32 = instance->output_buffers;
    instance->audio_out.data64 = NULL;
    instance->audio_out.channel_count = instance->output_channels;
    instance->audio_out.latency = 0;
    instance->audio_out.constant_mask = 0;

    memset(&instance->process, 0, sizeof(instance->process));
    instance->process.transport = NULL;
    instance->process.audio_inputs = instance->input_channels ? &instance->audio_in : NULL;
    instance->process.audio_outputs = &instance->audio_out;
    instance->process.audio_inputs_count = instance->input_channels ? 1 : 0;
    instance->process.audio_outputs_count = 1;
    instance->process.in_events = &instance->in_events;
    instance->process.out_events = &instance->out_events;

    instance->latency_frames = instance->latency ? instance->latency->get(instance->plugin) : 0;
    atomic_store(&instance->run_state, CLAP_HOST_IDLE);
    return SUCCESS;
}

void clap_host_set_audio_thread(clap_instance_t *instance, pthread_t thread)
{
    instance->audio_thread = thread;
    instance->audio_role_held = 1;
}

void clap_host_arm(clap_instance_t *instance)
{
    atomic_store(&instance->run_state, CLAP_HOST_ARMED);
}

/* control thread, once no audio cycle can run anymore */
void clap_host_stop(clap_instance_t *instance)
{
    const uint32_t state = atomic_load(&instance->run_state);

    if (state == CLAP_HOST_PROCESSING || state == CLAP_HOST_STOPPING)
    {
        instance->audio_thread = pthread_self();
        instance->audio_role_held = 1;
        if (instance->plugin->stop_processing)
            instance->plugin->stop_processing(instance->plugin);
    }
    instance->audio_role_held = 0;
    instance->rendered_wet = 0;
    atomic_store(&instance->run_state, CLAP_HOST_IDLE);
}

/* control thread, while audio cycles keep running: they pass the input through until the plugin is back */
int clap_host_restart(clap_instance_t *instance, uint32_t max_frames)
{
    const double sample_rate = instance->sample_rate;
    int ret;

    if (!instance->active)
        return ERR_INVALID_OPERATION;
    if (wait_stopped(instance) != 0)
        return ERR_INVALID_OPERATION;

    clap_host_stop(instance);
    clap_host_deactivate(instance);
    ret = clap_host_activate(instance, sample_rate, max_frames);
    if (ret != SUCCESS)
        return ret;
    clap_host_arm(instance);
    return SUCCESS;
}

void clap_host_deactivate(clap_instance_t *instance)
{
    if (!instance->active)
        return;
    instance->plugin->deactivate(instance->plugin);
    instance->active = 0;
    free_buffers(instance);
}

void clap_host_close(clap_instance_t *instance)
{
    if (!instance)
        return;
    clap_host_stop(instance);
    clap_host_deactivate(instance);
    instance->plugin->destroy(instance->plugin);
    binary_unref(instance->binary);
    free(instance);
}

uint32_t clap_host_binaries_open(void)
{
    const clap_binary_t *binary;
    uint32_t n = 0;

    for (binary = g_binaries; binary; binary = binary->next)
        n++;
    return n;
}

int clap_host_param_set(clap_instance_t *instance, clap_id id, double value)
{
    clap_param_info_t info;

    if (param_info(instance, id, &info) != 0)
        return ERR_LV2_INVALID_PARAM_SYMBOL;
    if (info.flags & (CLAP_PARAM_IS_HIDDEN | CLAP_PARAM_IS_READONLY | CLAP_PARAM_IS_BYPASS))
        return ERR_LV2_INVALID_PARAM_SYMBOL;

    if (value < info.min_value)
        value = info.min_value;
    else if (value > info.max_value)
        value = info.max_value;

    return push_or_flush(instance, id, value, info.cookie);
}

int clap_host_param_get(clap_instance_t *instance, clap_id id, double *value)
{
    clap_param_info_t info;

    if (param_info(instance, id, &info) != 0 || (info.flags & CLAP_PARAM_IS_HIDDEN))
        return ERR_LV2_INVALID_PARAM_SYMBOL;
    settle_queue(instance);
    if (!instance->params->get_value(instance->plugin, id, value))
        return ERR_LV2_INVALID_PARAM_SYMBOL;
    return SUCCESS;
}

/* the host's own bypass: one crossfade to the dry input on the next cycle, then the plugin idles */
int clap_host_bypass(clap_instance_t *instance, int value)
{
    atomic_store(&instance->bypass, value ? 1 : 0);
    return SUCCESS;
}

int clap_host_bypassed(clap_instance_t *instance)
{
    return atomic_load(&instance->bypass) != 0;
}

int clap_host_state_save(clap_instance_t *instance, const char *filename)
{
    stream_t stream = { NULL, CLAP_HOST_STATE_MAX_BYTES, 0, 0, 0 };
    const clap_ostream_t ostream = { &stream, stream_write };
    FILE *file;
    int ok;

    if (!instance->state)
        return ERR_INVALID_OPERATION;

    settle_queue(instance);
    stream.buffer = malloc(stream.capacity);
    if (!instance->state->save(instance->plugin, &ostream) || stream.overflow)
    {
        free(stream.buffer);
        return ERR_LV2_CANT_LOAD_STATE;
    }

    file = fopen(filename, "wb");
    if (!file)
    {
        free(stream.buffer);
        return ERR_LV2_CANT_LOAD_STATE;
    }
    ok = fwrite(stream.buffer, 1, stream.length, file) == stream.length;
    ok = fclose(file) == 0 && ok;
    free(stream.buffer);
    return ok ? SUCCESS : ERR_LV2_CANT_LOAD_STATE;
}

int clap_host_state_load(clap_instance_t *instance, const char *filename)
{
    stream_t stream = { NULL, CLAP_HOST_STATE_MAX_BYTES, 0, 0, 0 };
    const clap_istream_t istream = { &stream, stream_read };
    FILE *file;
    int ok;

    if (!instance->state)
        return ERR_INVALID_OPERATION;

    file = fopen(filename, "rb");
    if (!file)
        return ERR_LV2_CANT_LOAD_STATE;

    stream.buffer = malloc(stream.capacity);
    stream.length = fread(stream.buffer, 1, stream.capacity, file);
    ok = !ferror(file) && stream.length < stream.capacity;
    fclose(file);

    if (ok)
        ok = instance->state->load(instance->plugin, &istream);
    free(stream.buffer);
    return ok ? SUCCESS : ERR_LV2_CANT_LOAD_STATE;
}

int clap_host_preset_load(clap_instance_t *instance, const char *location)
{
    if (!instance->preset_load)
        return ERR_INVALID_OPERATION;
    if (!instance->preset_load->from_location(instance->plugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE, location, NULL))
        return ERR_LV2_INVALID_PRESET_URI;
    return SUCCESS;
}

void clap_host_idle(clap_instance_t *instance)
{
    if (atomic_exchange(&instance->callback_requested, 0) && instance->plugin->on_main_thread)
        instance->plugin->on_main_thread(instance->plugin);

    if (atomic_exchange(&instance->log_pending, 0))
        fprintf(stderr, "%s: %s\n", instance->desc->id, instance->log_slot);

    if (atomic_exchange(&instance->latency_changed, 0) && instance->active && instance->latency)
        instance->latency_frames = instance->latency->get(instance->plugin);

    if (atomic_exchange(&instance->flush_requested, 0) && atomic_load(&instance->run_state) == CLAP_HOST_IDLE)
        flush_events(instance);

    if (atomic_exchange(&instance->restart_requested, 0) && instance->active)
        clap_host_restart(instance, instance->max_frames);

    settle_queue(instance);
}

static void run_cycle(clap_instance_t *instance, const float *const *inputs, float *const *outputs, uint32_t nframes)
{
    uint32_t state = atomic_load_explicit(&instance->run_state, memory_order_acquire);
    uint32_t c;
    int want_wet;

    if (state == CLAP_HOST_ARMED && atomic_compare_exchange_strong(&instance->run_state, &state, CLAP_HOST_PROCESSING))
    {
        if (!instance->plugin->start_processing || instance->plugin->start_processing(instance->plugin))
            state = CLAP_HOST_PROCESSING;
        else
        {
            state = CLAP_HOST_STOPPED;
            atomic_store_explicit(&instance->run_state, state, memory_order_release);
        }
    }

    if (state == CLAP_HOST_STOPPING && atomic_compare_exchange_strong(&instance->run_state, &state, CLAP_HOST_STOPPED))
    {
        if (instance->plugin->stop_processing)
            instance->plugin->stop_processing(instance->plugin);
        state = CLAP_HOST_STOPPED;
    }

    if (state != CLAP_HOST_PROCESSING || nframes > instance->max_frames)
    {
        if (state == CLAP_HOST_PROCESSING)
            atomic_fetch_add_explicit(&instance->oversize_cycles, 1, memory_order_relaxed);
        pass_dry(instance, inputs, outputs, nframes);
        instance->rendered_wet = 0;
        return;
    }

    want_wet = atomic_load_explicit(&instance->bypass, memory_order_acquire) == 0;

    if (!want_wet && !instance->rendered_wet)
    {
        // steady bypass: the plugin idles, the input passes through untouched
        pass_dry(instance, inputs, outputs, nframes);
        return;
    }

    for (c = 0; c < instance->input_channels; c++)
        memcpy(instance->input_buffers[c], inputs[c], sizeof(float) * nframes);

    if (!process_cycle(instance, nframes))
    {
        pass_dry(instance, inputs, outputs, nframes);
        instance->rendered_wet = 0;
        return;
    }

    deliver(instance, inputs, outputs, nframes, want_wet);
}

static clap_host_event_t *note_slot(clap_instance_t *instance, uint32_t time, uint16_t type, uint32_t size)
{
    clap_host_event_t *slot;

    if (instance->notes_count >= CLAP_HOST_NOTES_PER_BLOCK)
    {
        atomic_fetch_add_explicit(&instance->notes_dropped, 1, memory_order_relaxed);
        return NULL;
    }
    slot = &instance->notes[instance->notes_count++];
    memset(slot, 0, sizeof(*slot));
    slot->header.size = size;
    slot->header.time = time;
    slot->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    slot->header.type = type;
    return slot;
}

/* audio thread, ahead of the cycle they belong to, in the order they arrived: one MIDI message of the note input
 * as the event the input's dialect wants; a plugin that reads only the CLAP dialect gets notes, and only notes */
void clap_host_midi_in(clap_instance_t *instance, uint32_t time, const uint8_t *data, size_t size)
{
    const uint8_t type = size ? data[0] & 0xf0 : 0;
    const int16_t channel = size ? data[0] & 0x0f : 0;
    clap_host_event_t *slot;

    if (!instance->note_inputs || size == 0 || size > 3 || data[0] < 0x80 || data[0] >= 0xf0)
        return;

    if (instance->note_dialect == CLAP_NOTE_DIALECT_MIDI)
    {
        slot = note_slot(instance, time, CLAP_EVENT_MIDI, sizeof(clap_event_midi_t));
        if (!slot)
            return;
        slot->midi.data[0] = data[0];
        slot->midi.data[1] = size > 1 ? data[1] : 0;
        slot->midi.data[2] = size > 2 ? data[2] : 0;
        return;
    }

    if ((type != 0x80 && type != 0x90) || size != 3)
        return;
    slot = note_slot(instance, time, type == 0x90 && data[2] ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF, sizeof(clap_event_note_t));
    if (!slot)
        return;
    slot->note.note_id = -1;
    slot->note.port_index = 0;
    slot->note.channel = channel;
    slot->note.key = data[1];
    slot->note.velocity = (double)data[2] / 127.0;
}

/* a cycle marks itself so the control thread taking the audio role waits for it */
void clap_host_run(clap_instance_t *instance, const float *const *inputs, float *const *outputs, uint32_t nframes)
{
    atomic_store(&instance->in_cycle, 1);
    run_cycle(instance, inputs, outputs, nframes);
    instance->notes_count = 0;
    atomic_store(&instance->in_cycle, 0);
}

void clap_host_denormals_off(void)
{
#if defined(__i386__) || defined(__x86_64__)
    unsigned int mxcsr = __builtin_ia32_stmxcsr();
    __builtin_ia32_ldmxcsr(mxcsr | 0x8040);
#elif defined(__aarch64__)
    uint64_t cw;
    __asm__ __volatile__ (
        "mrs    %0, fpcr                            \n"
        "orr    %0, %0, #0x1000000                  \n"
        "msr    fpcr, %0                            \n"
        "isb                                        \n"
        : "=r"(cw) :: "memory");
#elif defined(__arm__)
    uint32_t cw;
    __asm__ __volatile__ (
        "vmrs   %0, fpscr                           \n"
        "orr    %0, %0, #0x1000000                  \n"
        "vmsr   fpscr, %0                           \n"
        : "=r"(cw) :: "memory");
#endif
}
