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
* The LV2 to CLAP shim of libomx-clap-lv2 (see omx_clap_lv2.h): the entry and the factory of a bundle, and a
* clap_plugin_t over one lv2/core instance. Everything LV2 is lv2/core's; this unit maps it.
*
*   create_plugin   the URI found in THIS bundle, its ports and required features read, refused before any binary is
*                   opened: NULL, and the verdict's code on the host's clap.log
*   init            the binary opened, the instance made, the parameter table built; no LV2 instance yet
*   activate        lv2/core's activate: instantiate on the first one or a rate change, the default state, LV2
*                   activate, the worker; the latency the port last read is the one latency.get answers from here on
*   reset           the worker joined, LV2 deactivate, the rings emptied, LV2 activate, the worker again; libomx-clap-core
*                   calls it from the bypass-off verb on the control thread, never from process()
*   process         the host's input copied into the instance's own buffers, worker responses drained, the parameter
*                   events written into the control ports, run, end_run, the flush-to-zero bits, the output copied out
*   deactivate      the worker stopped and joined, LV2 deactivate
*
* Parameters. The clap_id of a parameter is its port index. An input control port is a parameter; the plugin's own
* bypass is one flagged IS_BYPASS and held at its processing value, never written; every other output control port is
* a read-only parameter whose value is what the last process() read. A parameter write is block-rate: every
* CLAP_EVENT_PARAM_VALUE of a process() call lands before that call's run(), whatever its time.
*
* Latency. latency.get answers the figure taken at the last activate. A different reading is held: only once it has
* read unchanged for latency_hold_ms of blocks does the shim call host->request_restart, and the restart's activate
* takes it. A value that moves and comes back inside the hold costs nothing.
*
* The entry. CLAP's get_factory names no entry, so each loaded bundle takes one of OMX_CLAP_LV2_BUNDLES slots, each with
* a get_factory of its own.
*
************************************************************************************************************************
*/


/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <clap/clap.h>
#include <clap/ext/preset-load.h>
#include <clap/ext/state.h>
#include <clap/factory/preset-discovery.h>

#include "lv2_core.h"
#include "omx_clap_lv2.h"


/*
************************************************************************************************************************
*           LOCAL DEFINES
************************************************************************************************************************
*/

// how many bundles may be loaded at once: one entry slot each
#define OMX_CLAP_LV2_BUNDLES            64u

#define SHIM_OF(plugin)                 ((struct shim *)(plugin)->plugin_data)


/*
************************************************************************************************************************
*           LOCAL DATA TYPES
************************************************************************************************************************
*/

/* one loaded bundle's entry and factory */
struct slot
{
    clap_plugin_entry_t entry;
    clap_plugin_factory_t factory;
    struct lv2_bundle *bundle;
    unsigned refs;
    clap_plugin_descriptor_t *descs;
};

/* one CLAP plugin over one LV2 instance */
struct shim
{
    clap_plugin_t plugin;
    clap_plugin_descriptor_t desc;
    char *name;
    const clap_host_t *host;
    const clap_host_log_t *host_log;
    const clap_host_thread_check_t *host_thread;
    struct lv2_plugin *lv2;
    struct lv2_instance *in;
    int32_t *control_of_port;           // port index -> control index, -1 for a port that is not a control
    int active, processing;
    uint32_t max_frames;
    double rate;

    // latency: the figure latency.get answers, and a different one being held
    uint32_t latency_active;
    uint32_t latency_pending;
    uint64_t latency_held_frames;
    uint64_t latency_hold_frames;
    int restart_requested;

    // the counters as the main thread last logged them
    struct lv2_counters logged;
    _Atomic uint32_t counters_moved;

    // the events of the process() under way, for the write before run()
    const clap_input_events_t *events;
};


/*
************************************************************************************************************************
*           LOCAL GLOBAL VARIABLES
************************************************************************************************************************
*/

static struct slot g_slots[OMX_CLAP_LV2_BUNDLES];
static uint32_t g_latency_hold_ms;
static int g_configured;

static const char *const g_features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL };


/*
************************************************************************************************************************
*           GLOBAL VARIABLES
************************************************************************************************************************
*/

const char *const omx_clap_lv2_provided[] = LV2_CORE_PROVIDED_INIT;


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS: THE HOST
************************************************************************************************************************
*/

static void why_set(char why[OMX_CLAP_LV2_WHY_MAX], const char *code)
{
    if (why)
        snprintf(why, OMX_CLAP_LV2_WHY_MAX, "%s", code);
}

/* a line on the host's clap.log, when it offers one */
static void host_log(const clap_host_t *host, clap_log_severity severity, const char *msg)
{
    const clap_host_log_t *log = host && host->get_extension ? host->get_extension(host, CLAP_EXT_LOG) : NULL;

    if (log && log->log)
        log->log(host, severity, msg);
}

/* the host's audio role, by its own predicate: lv2/core classifies map, log and schedule_work by it */
static int shim_is_audio(void *ctx)
{
    const struct shim *s = ctx;

    return s->host_thread->is_audio_thread(s->host) ? 1 : 0;
}


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS: clap.params
************************************************************************************************************************
*/

static int32_t control_of_id(const struct shim *s, clap_id id)
{
    return id < s->lv2->n_ports ? s->control_of_port[id] : -1;
}

static uint32_t params_count(const clap_plugin_t *plugin)
{
    return SHIM_OF(plugin)->lv2->n_controls;
}

static bool params_get_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info)
{
    const struct shim *s = SHIM_OF(plugin);
    const struct lv2_control *c;

    if (index >= s->lv2->n_controls)
        return false;
    c = &s->lv2->controls[index];
    memset(info, 0, sizeof(*info));
    info->id = c->port;
    snprintf(info->name, sizeof(info->name), "%s", c->name);
    info->min_value = c->min;
    info->max_value = c->max;
    info->default_value = c->def;
    switch (c->kind)
    {
    case LV2_CONTROL_BYPASS:
        info->flags = CLAP_PARAM_IS_BYPASS | CLAP_PARAM_IS_STEPPED;
        info->min_value = 0.0;
        info->max_value = 1.0;
        break;
    case LV2_CONTROL_OUTPUT:
        info->flags = CLAP_PARAM_IS_READONLY;
        break;
    case LV2_CONTROL_INPUT:
        info->flags = CLAP_PARAM_IS_AUTOMATABLE;
        if (c->props & (LV2_PROP_INTEGER | LV2_PROP_TOGGLED | LV2_PROP_ENUMERATION | LV2_PROP_TRIGGER))
            info->flags |= CLAP_PARAM_IS_STEPPED;
        if ((c->props & LV2_PROP_ENUMERATION) && (c->props & LV2_PROP_ENUM_COMPLETE))
            info->flags |= CLAP_PARAM_IS_ENUM;
        if (c->props & LV2_PROP_NOT_ON_GUI)
            info->flags |= CLAP_PARAM_IS_HIDDEN;
        if (c->props & LV2_PROP_TOGGLED)
        {
            info->min_value = 0.0;
            info->max_value = 1.0;
        }
        break;
    }
    return true;
}

static bool params_get_value(const clap_plugin_t *plugin, clap_id id, double *value)
{
    const struct shim *s = SHIM_OF(plugin);
    const int32_t k = control_of_id(s, id);

    if (k < 0)
        return false;
    *value = lv2_instance_control_get(s->in, (uint32_t)k);
    return true;
}

static bool params_value_to_text(const clap_plugin_t *plugin, clap_id id, double value, char *out, uint32_t size)
{
    const struct shim *s = SHIM_OF(plugin);
    const int32_t k = control_of_id(s, id);
    const struct lv2_control *c;
    uint32_t i;

    if (k < 0 || size == 0)
        return false;
    c = &s->lv2->controls[k];
    for (i = 0; i < c->n_points; i++)
    {
        if ((double)c->points[i].value == value)
        {
            snprintf(out, size, "%s", c->points[i].label);
            return true;
        }
    }
    snprintf(out, size, "%g", value);
    return true;
}

static bool params_text_to_value(const clap_plugin_t *plugin, clap_id id, const char *text, double *value)
{
    const struct shim *s = SHIM_OF(plugin);
    const int32_t k = control_of_id(s, id);
    const struct lv2_control *c;
    char *end;
    uint32_t i;

    if (k < 0 || !text)
        return false;
    c = &s->lv2->controls[k];
    for (i = 0; i < c->n_points; i++)
    {
        if (strcmp(c->points[i].label, text) == 0)
        {
            *value = c->points[i].value;
            return true;
        }
    }
    *value = strtod(text, &end);
    return end != text;
}

/* every CLAP_EVENT_PARAM_VALUE of the list into the control it names; any other event is not taken */
static void apply_events(struct shim *s, const clap_input_events_t *events)
{
    const uint32_t n = events ? events->size(events) : 0;
    uint32_t i;

    for (i = 0; i < n; i++)
    {
        const clap_event_header_t *h = events->get(events, i);
        const clap_event_param_value_t *e;
        int32_t k;

        if (!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE)
            continue;
        e = (const clap_event_param_value_t *)h;
        k = control_of_id(s, e->param_id);
        if (k >= 0)
            lv2_instance_control_set(s->in, (uint32_t)k, (float)e->value);
    }
}

static void params_flush(const clap_plugin_t *plugin, const clap_input_events_t *in, const clap_output_events_t *out)
{
    (void)out;
    apply_events(SHIM_OF(plugin), in);
}

static const clap_plugin_params_t g_params =
{
    params_count, params_get_info, params_get_value, params_value_to_text, params_text_to_value, params_flush
};


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS: clap.latency, clap.audio-ports
************************************************************************************************************************
*/

static uint32_t latency_get(const clap_plugin_t *plugin)
{
    return SHIM_OF(plugin)->latency_active;
}

static const clap_plugin_latency_t g_latency = { latency_get };

static uint32_t ports_count(const clap_plugin_t *plugin, bool is_input)
{
    (void)plugin;
    (void)is_input;
    return 1;
}

static bool ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_audio_port_info_t *info)
{
    const struct shim *s = SHIM_OF(plugin);

    if (index != 0)
        return false;
    memset(info, 0, sizeof(*info));
    info->id = 0;
    snprintf(info->name, sizeof(info->name), "%s", is_input ? "in" : "out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = s->lv2->legs;
    info->port_type = s->lv2->legs == 2 ? CLAP_PORT_STEREO : CLAP_PORT_MONO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t g_audio_ports = { ports_count, ports_get };


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS: THE PLUGIN
************************************************************************************************************************
*/

static bool plugin_init(const clap_plugin_t *plugin)
{
    struct shim *s = SHIM_OF(plugin);
    char why[LV2_CORE_WHY_MAX];
    uint32_t k;

    if (lv2_plugin_load(s->lv2, why) != 0)
    {
        host_log(s->host, CLAP_LOG_ERROR, why);
        return false;
    }
    s->control_of_port = malloc((s->lv2->n_ports ? s->lv2->n_ports : 1u) * sizeof(*s->control_of_port));
    s->in = lv2_instance_new(s->lv2);
    if (!s->control_of_port || !s->in)
    {
        host_log(s->host, CLAP_LOG_ERROR, LV2_CODE_NO_REALISATION);
        return false;
    }
    for (k = 0; k < s->lv2->n_ports; k++)
        s->control_of_port[k] = -1;
    for (k = 0; k < s->lv2->n_controls; k++)
        s->control_of_port[s->lv2->controls[k].port] = (int32_t)k;
    s->host_thread = s->host->get_extension ? s->host->get_extension(s->host, CLAP_EXT_THREAD_CHECK) : NULL;
    if (s->host_thread && s->host_thread->is_audio_thread)
        lv2_instance_set_role(s->in, shim_is_audio, s);
    return true;
}

static void plugin_destroy(const clap_plugin_t *plugin)
{
    struct shim *s = SHIM_OF(plugin);

    lv2_instance_free(s->in);
    lv2_plugin_close(s->lv2);
    free(s->control_of_port);
    free(s->name);
    free(s);
}

static bool plugin_activate(const clap_plugin_t *plugin, double rate, uint32_t min_frames, uint32_t max_frames)
{
    struct shim *s = SHIM_OF(plugin);
    char why[LV2_CORE_WHY_MAX];

    if (lv2_instance_activate(s->in, rate, min_frames, max_frames, why) != 0)
    {
        host_log(s->host, CLAP_LOG_ERROR, why);
        return false;
    }
    s->active = 1;
    s->rate = rate;
    s->max_frames = max_frames;
    // the restart a held latency asked for is this activation: it takes the reading
    s->latency_active = lv2_instance_latency(s->in);
    s->latency_pending = s->latency_active;
    s->latency_held_frames = 0;
    s->latency_hold_frames = (uint64_t)(rate * (double)g_latency_hold_ms / 1000.0);
    s->restart_requested = 0;
    return true;
}

static void plugin_deactivate(const clap_plugin_t *plugin)
{
    struct shim *s = SHIM_OF(plugin);

    lv2_instance_deactivate(s->in);
    s->active = 0;
}

static bool plugin_start_processing(const clap_plugin_t *plugin)
{
    SHIM_OF(plugin)->processing = 1;
    return true;
}

static void plugin_stop_processing(const clap_plugin_t *plugin)
{
    SHIM_OF(plugin)->processing = 0;
}

static void plugin_reset(const clap_plugin_t *plugin)
{
    lv2_instance_reset(SHIM_OF(plugin)->in);
}

static void write_controls(void *ctx)
{
    struct shim *s = ctx;

    apply_events(s, s->events);
}

/* [audio] the latency port's reading against the figure latency.get answers: a different one held for the hold, then
 * one restart asked for */
static void latency_watch(struct shim *s, uint32_t n)
{
    const uint32_t v = lv2_instance_latency(s->in);

    if (v == s->latency_active)
    {
        s->latency_pending = v;
        s->latency_held_frames = 0;
        return;
    }
    if (v != s->latency_pending)
    {
        s->latency_pending = v;
        s->latency_held_frames = 0;
    }
    s->latency_held_frames += n;
    if (!s->restart_requested && s->latency_held_frames >= s->latency_hold_frames)
    {
        s->restart_requested = 1;
        s->host->request_restart(s->host);
    }
}

static clap_process_status plugin_process(const clap_plugin_t *plugin, const clap_process_t *process)
{
    struct shim *s = SHIM_OF(plugin);
    const uint32_t n = process->frames_count, legs = s->lv2->legs;
    struct lv2_counters c;
    uint32_t leg;

    if (!s->active || n > s->max_frames)
        return CLAP_PROCESS_ERROR;
    for (leg = 0; leg < legs; leg++)
    {
        float *dst = lv2_instance_audio_in(s->in, leg);
        const clap_audio_buffer_t *b = process->audio_inputs_count ? &process->audio_inputs[0] : NULL;

        if (b && b->data32 && b->channel_count)
            memcpy(dst, b->data32[leg < b->channel_count ? leg : 0], n * sizeof(float));
        else
            memset(dst, 0, n * sizeof(float));
    }
    s->events = process->in_events;
    lv2_instance_run(s->in, n, write_controls, s);
    s->events = NULL;
    if (process->audio_outputs_count && process->audio_outputs[0].data32)
    {
        const clap_audio_buffer_t *b = &process->audio_outputs[0];

        for (leg = 0; leg < b->channel_count; leg++)
            memcpy(b->data32[leg], lv2_instance_audio_out(s->in, leg < legs ? leg : 0), n * sizeof(float));
    }
    latency_watch(s, n);
    lv2_instance_counters(s->in, &c);
    if (memcmp(&c, &s->logged, sizeof(c)) != 0 && !atomic_exchange_explicit(&s->counters_moved, 1u, memory_order_relaxed))
        s->host->request_callback(s->host);
    return CLAP_PROCESS_CONTINUE;
}

/* the whole stream, read until the host returns 0: NULL-terminated, malloc'd; false when the host fails or runs out of memory */
static bool read_all(const clap_istream_t *stream, char **out)
{
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap);

    if (!buf)
        return false;
    for (;;)
    {
        int64_t r;

        if (n + 1 == cap)
        {
            char *more = realloc(buf, cap * 2);

            if (!more)
            {
                free(buf);
                return false;
            }
            buf = more;
            cap *= 2;
        }
        r = stream->read(stream, buf + n, cap - 1 - n);
        if (r < 0)
        {
            free(buf);
            return false;
        }
        if (r == 0)
            break;
        n += (size_t)r;
    }
    buf[n] = '\0';
    *out = buf;
    return true;
}

/* the state as LV2 Turtle; the host's stream takes all of it or the save fails */
static bool state_save(const clap_plugin_t *plugin, const clap_ostream_t *stream)
{
    struct shim *s = SHIM_OF(plugin);
    char *text = lv2_instance_state_save(s->in);
    size_t n, done = 0;

    if (!text)
    {
        host_log(s->host, CLAP_LOG_ERROR, LV2_CODE_STATE_UNREADABLE);
        return false;
    }
    n = strlen(text);
    while (done < n)
    {
        int64_t w = stream->write(stream, text + done, n - done);

        if (w <= 0)
            break;
        done += (size_t)w;
    }
    free(text);
    return done == n;
}

/* a state is held and applied at the next activate: a load while active asks the host for the restart that is that activate */
static bool state_load(const clap_plugin_t *plugin, const clap_istream_t *stream)
{
    struct shim *s = SHIM_OF(plugin);
    char why[LV2_CORE_WHY_MAX], *text = NULL;
    bool ok;

    if (!read_all(stream, &text))
    {
        host_log(s->host, CLAP_LOG_ERROR, LV2_CODE_STATE_UNREADABLE);
        return false;
    }
    ok = lv2_instance_state_load(s->in, text, why) == 0;
    free(text);
    if (!ok)
    {
        host_log(s->host, CLAP_LOG_ERROR, why);
        return false;
    }
    if (s->active)
        s->host->request_restart(s->host);
    return true;
}

static const clap_plugin_state_t g_state = { state_save, state_load };

/* a preset of the bundle, named by its URI (the location is the bundle's own, so it is not read): held as a load is */
static bool preset_from_location(const clap_plugin_t *plugin, uint32_t kind, const char *location, const char *key)
{
    struct shim *s = SHIM_OF(plugin);
    char why[LV2_CORE_WHY_MAX];

    (void)location;
    if (kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || !key)
    {
        host_log(s->host, CLAP_LOG_ERROR, LV2_CODE_PRESET_NOT_FOUND);
        return false;
    }
    if (lv2_instance_preset_load(s->in, key, why) != 0)
    {
        host_log(s->host, CLAP_LOG_ERROR, why);
        return false;
    }
    if (s->active)
        s->host->request_restart(s->host);
    return true;
}

static const clap_plugin_preset_load_t g_preset_load = { preset_from_location };

static const void *plugin_get_extension(const clap_plugin_t *plugin, const char *id)
{
    (void)plugin;
    if (!strcmp(id, CLAP_EXT_PARAMS))
        return &g_params;
    if (!strcmp(id, CLAP_EXT_LATENCY))
        return &g_latency;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &g_audio_ports;
    if (!strcmp(id, CLAP_EXT_STATE))
        return &g_state;
    if (!strcmp(id, CLAP_EXT_PRESET_LOAD))
        return &g_preset_load;
    return NULL;
}

/* the worker's counters, as clap.log lines on the main thread: CLAP has no counter extension */
static void plugin_on_main_thread(const clap_plugin_t *plugin)
{
    struct shim *s = SHIM_OF(plugin);
    struct lv2_counters c;
    char line[256];

    if (!atomic_exchange_explicit(&s->counters_moved, 0u, memory_order_relaxed))
        return;
    lv2_instance_counters(s->in, &c);
    if (memcmp(&c, &s->logged, sizeof(c)) == 0)
        return;
    snprintf(line, sizeof(line),
             "lv2 counters: schedule_refused %u responses_refused %u respond_strikes %u map_on_audio %u schedule_off_audio %u log_on_audio %u",
             c.schedule_refused, c.responses_refused, c.respond_strikes, c.map_on_audio, c.schedule_off_audio, c.log_on_audio);
    host_log(s->host, CLAP_LOG_WARNING, line);
    s->logged = c;
}


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS: THE FACTORY AND THE ENTRY
************************************************************************************************************************
*/

static struct slot *slot_of_factory(const clap_plugin_factory_t *factory)
{
    return (struct slot *)((const char *)factory - offsetof(struct slot, factory));
}

static uint32_t factory_count(const clap_plugin_factory_t *factory)
{
    return lv2_bundle_count(slot_of_factory(factory)->bundle);
}

static const clap_plugin_descriptor_t *factory_descriptor(const clap_plugin_factory_t *factory, uint32_t index)
{
    struct slot *slot = slot_of_factory(factory);

    return index < lv2_bundle_count(slot->bundle) ? &slot->descs[index] : NULL;
}

static void descriptor_fill(clap_plugin_descriptor_t *d, const char *uri, const char *name)
{
    memset(d, 0, sizeof(*d));
    d->clap_version = (clap_version_t)CLAP_VERSION_INIT;
    d->id = uri;
    d->name = name;
    d->vendor = "";
    d->url = "";
    d->manual_url = "";
    d->support_url = "";
    d->version = "";
    d->description = "";
    d->features = g_features;
}

static const clap_plugin_t *factory_create(const clap_plugin_factory_t *factory, const clap_host_t *host, const char *id)
{
    struct slot *slot = slot_of_factory(factory);
    char why[LV2_CORE_WHY_MAX];
    struct lv2_plugin *lv2;
    struct shim *s;

    if (!host || !id || !clap_version_is_compatible(host->clap_version))
        return NULL;
    lv2 = lv2_plugin_open(slot->bundle, id, why);
    if (!lv2)
    {
        host_log(host, CLAP_LOG_ERROR, why);
        return NULL;
    }
    s = calloc(1, sizeof(*s));
    if (!s)
    {
        lv2_plugin_close(lv2);
        host_log(host, CLAP_LOG_ERROR, LV2_CODE_NO_REALISATION);
        return NULL;
    }
    s->lv2 = lv2;
    s->host = host;
    descriptor_fill(&s->desc, lv2->uri, lv2->uri);
    for (uint32_t i = 0; i < lv2_bundle_count(slot->bundle); i++)
        if (strcmp(lv2_bundle_uri(slot->bundle, i), id) == 0)
            s->name = strdup(slot->descs[i].name);
    if (s->name)
        s->desc.name = s->name;     // the plugin may outlive the entry: its descriptor is its own
    s->plugin.desc = &s->desc;
    s->plugin.plugin_data = s;
    s->plugin.init = plugin_init;
    s->plugin.destroy = plugin_destroy;
    s->plugin.activate = plugin_activate;
    s->plugin.deactivate = plugin_deactivate;
    s->plugin.start_processing = plugin_start_processing;
    s->plugin.stop_processing = plugin_stop_processing;
    s->plugin.reset = plugin_reset;
    s->plugin.process = plugin_process;
    s->plugin.get_extension = plugin_get_extension;
    s->plugin.on_main_thread = plugin_on_main_thread;
    return &s->plugin;
}

static bool entry_init(const char *path)
{
    (void)path;
    return true;
}

static void entry_deinit(void)
{
}

static const void *slot_factory(uint32_t index, const char *id)
{
    if (index >= OMX_CLAP_LV2_BUNDLES || !g_slots[index].refs || !id || strcmp(id, CLAP_PLUGIN_FACTORY_ID) != 0)
        return NULL;
    return &g_slots[index].factory;
}

/* one get_factory per slot: CLAP's names no entry */
#define GET_FACTORY(a, b) \
    static const void *get_factory_##a##_##b(const char *id) { return slot_factory(a * 8u + b, id); }
#define GET_FACTORY_ROW(a) \
    GET_FACTORY(a, 0) GET_FACTORY(a, 1) GET_FACTORY(a, 2) GET_FACTORY(a, 3) \
    GET_FACTORY(a, 4) GET_FACTORY(a, 5) GET_FACTORY(a, 6) GET_FACTORY(a, 7)
GET_FACTORY_ROW(0) GET_FACTORY_ROW(1) GET_FACTORY_ROW(2) GET_FACTORY_ROW(3)
GET_FACTORY_ROW(4) GET_FACTORY_ROW(5) GET_FACTORY_ROW(6) GET_FACTORY_ROW(7)
#define GET_FACTORY_REF(a) \
    get_factory_##a##_0, get_factory_##a##_1, get_factory_##a##_2, get_factory_##a##_3, \
    get_factory_##a##_4, get_factory_##a##_5, get_factory_##a##_6, get_factory_##a##_7
static const void *(*const g_get_factory[OMX_CLAP_LV2_BUNDLES])(const char *) =
{
    GET_FACTORY_REF(0), GET_FACTORY_REF(1), GET_FACTORY_REF(2), GET_FACTORY_REF(3),
    GET_FACTORY_REF(4), GET_FACTORY_REF(5), GET_FACTORY_REF(6), GET_FACTORY_REF(7),
};


/*
************************************************************************************************************************
*           GLOBAL FUNCTIONS
************************************************************************************************************************
*/

int omx_clap_lv2_configure(const omx_clap_lv2_config_t *cfg, char why[OMX_CLAP_LV2_WHY_MAX])
{
    struct lv2_core_config config;
    char core_why[LV2_CORE_WHY_MAX];

    if (!cfg)
    {
        why_set(why, LV2_CODE_NO_REALISATION);
        return -1;
    }
    if (g_configured && cfg->latency_hold_ms != g_latency_hold_ms)
    {
        why_set(why, "lv2.configure.once");
        return -1;
    }
    config.features = cfg->features;
    config.worker_poll_us = cfg->worker_poll_us;
    config.worker_ring_bytes = cfg->worker_ring_bytes;
    config.atom_buffer_bytes = cfg->atom_buffer_bytes;
    config.lilv_soname = cfg->lilv_soname;
    if (lv2_core_configure(&config, core_why) != 0)
    {
        why_set(why, core_why);
        return -1;
    }
    g_latency_hold_ms = cfg->latency_hold_ms;
    g_configured = 1;
    why_set(why, "");
    return 0;
}

const clap_plugin_entry_t *omx_clap_lv2_entry(const char *bundle_path, char why[OMX_CLAP_LV2_WHY_MAX])
{
    struct slot *slot = NULL;
    struct lv2_bundle *bundle;
    char core_why[LV2_CORE_WHY_MAX];
    uint32_t i, n;

    if (!g_configured)
    {
        why_set(why, LV2_CODE_NO_REALISATION);
        return NULL;
    }
    bundle = lv2_bundle_ref(bundle_path, core_why);
    if (!bundle)
    {
        why_set(why, core_why);
        return NULL;
    }
    for (i = 0; i < OMX_CLAP_LV2_BUNDLES; i++)
    {
        if (g_slots[i].refs && g_slots[i].bundle == bundle)
        {
            lv2_bundle_unref(bundle);       // the slot holds the bundle once
            g_slots[i].refs++;
            why_set(why, "");
            return &g_slots[i].entry;
        }
        if (!slot && !g_slots[i].refs)
            slot = &g_slots[i];
    }
    n = lv2_bundle_count(bundle);
    if (!slot || !(slot->descs = calloc(n, sizeof(*slot->descs))))
    {
        lv2_bundle_unref(bundle);
        why_set(why, LV2_CODE_NO_REALISATION);
        return NULL;
    }
    for (i = 0; i < n; i++)
        descriptor_fill(&slot->descs[i], lv2_bundle_uri(bundle, i), lv2_bundle_name(bundle, i));
    slot->bundle = bundle;
    slot->refs = 1;
    slot->entry.clap_version = (clap_version_t)CLAP_VERSION_INIT;
    slot->entry.init = entry_init;
    slot->entry.deinit = entry_deinit;
    slot->entry.get_factory = g_get_factory[slot - g_slots];
    slot->factory.get_plugin_count = factory_count;
    slot->factory.get_plugin_descriptor = factory_descriptor;
    slot->factory.create_plugin = factory_create;
    why_set(why, "");
    return &slot->entry;
}

void omx_clap_lv2_entry_release(const clap_plugin_entry_t *entry)
{
    uint32_t i;

    for (i = 0; entry && i < OMX_CLAP_LV2_BUNDLES; i++)
    {
        struct slot *slot = &g_slots[i];

        if (!slot->refs || &slot->entry != entry || --slot->refs > 0)
            continue;
        free(slot->descs);
        lv2_bundle_unref(slot->bundle);     // a plugin still open holds its own reference
        memset(slot, 0, sizeof(*slot));
        return;
    }
}

void omx_clap_lv2_worker_quiesce(const clap_plugin_t *plugin)
{
    if (plugin && plugin->plugin_data)
        lv2_instance_worker_quiesce(SHIM_OF(plugin)->in);
}
