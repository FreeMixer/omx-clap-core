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
* Parameters. The clap_id of a control parameter is its port index. An input control port is a parameter; the plugin's
* own bypass is one flagged IS_BYPASS and held at its processing value, never written; every other output control port
* is a read-only parameter whose value is what the last process() read. A numeric patch:writable property is a
* parameter too, its clap_id its port count plus its index in the order of the properties' URIs, so the two never collide. A parameter
* write is block-rate: every CLAP_EVENT_PARAM_VALUE of a process() call lands before that call's run(), whatever its
* time. A patch:Set or patch:Put the plugin emits is an output CLAP_EVENT_PARAM_VALUE of that call, at time 0.
*
* CV. Each lv2:CVPort is a mono CLAP audio port, not main, named by its lv2:name: the CV inputs after the side chains, the
* CV outputs after the main output. Every frame is copied both ways, so a CV signal reaches the plugin as it reaches an
* audio port; an input the host hands no buffer reads silence. docs/lv2-compatibility.md says why not parameter
* modulation.
*
* MIDI. An LV2 MIDI input is one clap.note-ports input, dialects MIDI (preferred) and CLAP, and an LV2 MIDI output one
* output, dialect MIDI. Each CLAP_EVENT_MIDI and CLAP_EVENT_MIDI_SYSEX of a process() call is written into the input before
* that call's run(), at its frame, as a midi:MidiEvent of the message's length: a channel program change or channel
* pressure carries two bytes, every other channel message three, a system message the length its status says, a sysex its
* own bytes. A CLAP note on or off is the MIDI note on or off of its channel and key, a choke a note off of velocity 0; one
* with a wildcard key or channel names no MIDI message and is not taken. When the input also takes time:Position, the
* transport's object comes first, at frame 0 (lv2/core). Each midi:MidiEvent the plugin wrote is a CLAP_EVENT_MIDI
* (a message of up to three bytes, the rest zero) or a CLAP_EVENT_MIDI_SYSEX (a sysex), at its frame; a frame past the
* block is held at its last frame, and a message of more than three bytes that is not a sysex is not passed on.
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

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <clap/clap.h>
#include <clap/ext/preset-load.h>
#include <clap/ext/state.h>
#include <clap/factory/preset-discovery.h>
#include <clap/ext/note-ports.h>

#include "lv2_core.h"
#include "omx_clap_midi.h"
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
    clap_preset_discovery_factory_t presets;    ///< the bundle's pset:Preset, through one provider
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
    int broken;                         // a reset whose worker could not start again: process() fails until activate
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

    // the events of the process() under way, for the write before run(), and the block's length and output
    const clap_input_events_t *events;
    const clap_output_events_t *out_events;
    uint32_t frames;
};


/*
************************************************************************************************************************
*           LOCAL GLOBAL VARIABLES
************************************************************************************************************************
*/

static struct slot g_slots[OMX_CLAP_LV2_BUNDLES];
static uint32_t g_latency_hold_ms;
static int g_configured;

// the code of the last refusal of create_plugin, init or activate on this thread, "" after one that did not refuse
static __thread char g_last_refusal[OMX_CLAP_LV2_WHY_MAX];

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

/* a refusal of create_plugin, init or activate: the code on the host's clap.log, and kept for omx_clap_lv2_last_refusal */
static void refuse(const clap_host_t *host, const char *code)
{
    why_set(g_last_refusal, code);
    host_log(host, CLAP_LOG_ERROR, code);
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

/* the patch parameter of a clap_id, or -1: the ids above the port indices, one per numeric patch:writable property */
static int32_t patch_of_id(const struct shim *s, clap_id id)
{
    uint32_t j;

    for (j = 0; j < s->lv2->n_patches; j++)
        if (s->lv2->patches[j].id == id)
            return (int32_t)j;
    return -1;
}

/* the control parameters first, then the patch parameters, in the order the parameter list reads them */
static uint32_t params_count(const clap_plugin_t *plugin)
{
    return SHIM_OF(plugin)->lv2->n_controls + SHIM_OF(plugin)->lv2->n_patches;
}

static void patch_info(const struct lv2_patch *pp, clap_param_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->id = pp->id;
    snprintf(info->name, sizeof(info->name), "%s", pp->name);
    info->min_value = pp->min;
    info->max_value = pp->max;
    info->default_value = pp->def;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    if (pp->props & (LV2_PROP_INTEGER | LV2_PROP_TOGGLED))
        info->flags |= CLAP_PARAM_IS_STEPPED;
}

static bool params_get_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info)
{
    const struct shim *s = SHIM_OF(plugin);
    const struct lv2_control *c;

    if (index >= s->lv2->n_controls)
    {
        if (index - s->lv2->n_controls >= s->lv2->n_patches)
            return false;
        patch_info(&s->lv2->patches[index - s->lv2->n_controls], info);
        return true;
    }
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
    const int32_t k = control_of_id(s, id), j = k < 0 ? patch_of_id(s, id) : -1;

    if (k < 0 && j < 0)
        return false;
    *value = k >= 0 ? lv2_instance_control_get(s->in, (uint32_t)k) : lv2_instance_patch_get(s->in, (uint32_t)j);
    return true;
}

static bool params_value_to_text(const clap_plugin_t *plugin, clap_id id, double value, char *out, uint32_t size)
{
    const struct shim *s = SHIM_OF(plugin);
    const int32_t k = control_of_id(s, id);
    const struct lv2_control *c;
    uint32_t i;

    if (size == 0)
        return false;
    if (k < 0)
    {
        if (patch_of_id(s, id) < 0)
            return false;
        snprintf(out, size, "%g", value);
        return true;
    }
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

    if (!text)
        return false;
    if (k < 0)
    {
        if (patch_of_id(s, id) < 0)
            return false;
        *value = strtod(text, &end);
        return end != text;
    }
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

/* every CLAP_EVENT_PARAM_VALUE of the list into the control or patch parameter it names; any other event is not taken */
static void apply_events(struct shim *s, const clap_input_events_t *events)
{
    const uint32_t n = events ? events->size(events) : 0;
    uint32_t i;

    for (i = 0; i < n; i++)
    {
        const clap_event_header_t *h = events->get(events, i);
        const clap_event_param_value_t *e;
        int32_t k, j;

        if (!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE)
            continue;
        e = (const clap_event_param_value_t *)h;
        k = control_of_id(s, e->param_id);
        j = k < 0 ? patch_of_id(s, e->param_id) : -1;
        if (k >= 0)
            lv2_instance_control_set(s->in, (uint32_t)k, (float)e->value);
        else if (j >= 0)
            lv2_instance_patch_write(s->in, (uint32_t)j, (float)e->value);
    }
}

/* every patch:Set or patch:Put the last run's output carried, as a CLAP_EVENT_PARAM_VALUE at time 0 */
static void emit_patch_changes(struct shim *s, const clap_output_events_t *out)
{
    uint32_t j;

    for (j = 0; out && j < s->lv2->n_patches; j++)
    {
        clap_event_param_value_t e;
        float v;

        if (!lv2_instance_patch_changed(s->in, j, &v))
            continue;
        memset(&e, 0, sizeof(e));
        e.header.size = sizeof(e);
        e.header.time = 0;
        e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.header.type = CLAP_EVENT_PARAM_VALUE;
        e.param_id = s->lv2->patches[j].id;
        e.value = (double)v;
        out->try_push(out, &e.header);
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

/* the inputs before the side chains and the CV inputs: the main one, none for an instrument */
static uint32_t main_inputs(const struct lv2_plugin *l)
{
    return l->in_legs ? 1u : 0u;
}

/* the main pair (none in for an instrument), then one input per side chain (LV2 port groups and lv2:isSideChain,
 * lv2/core), then one per CV input; the output is the main pair, then one per CV output */
static uint32_t ports_count(const clap_plugin_t *plugin, bool is_input)
{
    const struct lv2_plugin *l = SHIM_OF(plugin)->lv2;

    return is_input ? main_inputs(l) + l->n_side + l->n_cv_in : 1u + l->n_cv_out;
}

static bool ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_audio_port_info_t *info)
{
    const struct shim *s = SHIM_OF(plugin);

    if (index >= ports_count(plugin, is_input))
        return false;
    memset(info, 0, sizeof(*info));
    info->id = index;
    if (is_input)
        index += 1u - main_inputs(s->lv2);     // an instrument's first input is a side chain's or a CV input's
    if (index == 0)
    {
        snprintf(info->name, sizeof(info->name), "%s", is_input ? "in" : "out");
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = s->lv2->legs;
        info->port_type = s->lv2->legs == 2 ? CLAP_PORT_STEREO : CLAP_PORT_MONO;
    }
    else if (is_input && index - 1 < s->lv2->n_side)
    {
        snprintf(info->name, sizeof(info->name), "%s", s->lv2->side_name[index - 1]);
        info->channel_count = s->lv2->side_channels[index - 1];
        info->port_type = info->channel_count == 2 ? CLAP_PORT_STEREO : CLAP_PORT_MONO;
    }
    else
    {
        const uint32_t k = is_input ? index - 1 - s->lv2->n_side : index - 1;

        snprintf(info->name, sizeof(info->name), "%s", is_input ? s->lv2->cv_in_name[k] : s->lv2->cv_out_name[k]);
        info->channel_count = 1;
        info->port_type = CLAP_PORT_MONO;
    }
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t g_audio_ports = { ports_count, ports_get };


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS: clap.note-ports, MIDI
************************************************************************************************************************
*/

static uint32_t note_ports_count(const clap_plugin_t *plugin, bool is_input)
{
    const struct lv2_plugin *l = SHIM_OF(plugin)->lv2;

    return is_input ? l->midi_in_port >= 0 : l->midi_out_port >= 0;
}

static bool note_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_note_port_info_t *info)
{
    if (index != 0 || !note_ports_count(plugin, is_input))
        return false;
    memset(info, 0, sizeof(*info));
    info->id = 0;
    // the input takes CLAP's own note events too, translated to MIDI; the output speaks MIDI only
    info->supported_dialects = is_input ? CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_CLAP : CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
    snprintf(info->name, sizeof(info->name), "%s", is_input ? "MIDI in" : "MIDI out");
    return true;
}

static const clap_plugin_note_ports_t g_note_ports = { note_ports_count, note_ports_get };

/* a frame of the block for an event's time: a time past the block is held at its last frame */
static uint32_t frame_in(const struct shim *s, uint32_t time)
{
    if (time < s->frames)
        return time;
    return s->frames ? s->frames - 1 : 0;
}

/* [audio] the MIDI of the events, in the order they came, into the MIDI input of the next run. A message the input cannot
 * carry is not written; one past the buffer is dropped and counted by the core. */
static void midi_in_events(struct shim *s, const clap_input_events_t *events)
{
    const uint32_t n = events ? events->size(events) : 0;
    uint32_t i;

    for (i = 0; i < n; i++)
    {
        const clap_event_header_t *h = events->get(events, i);

        if (!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID)
            continue;
        if (h->type == CLAP_EVENT_MIDI)
        {
            const clap_event_midi_t *m = (const clap_event_midi_t *)h;
            const uint32_t size = omx_clap_midi_size(m->data[0]);

            if (size)
                lv2_instance_midi_in_add(s->in, frame_in(s, h->time), m->data, size);
        }
        else if (h->type == CLAP_EVENT_MIDI_SYSEX)
        {
            const clap_event_midi_sysex_t *x = (const clap_event_midi_sysex_t *)h;

            if (x->buffer && x->size)
                lv2_instance_midi_in_add(s->in, frame_in(s, h->time), x->buffer, x->size);
        }
        else if (h->type == CLAP_EVENT_NOTE_ON || h->type == CLAP_EVENT_NOTE_OFF || h->type == CLAP_EVENT_NOTE_CHOKE)
        {
            uint8_t m[3];

            if (omx_clap_note_midi(h, m))
                lv2_instance_midi_in_add(s->in, frame_in(s, h->time), m, 3);
        }
    }
}

/* [audio] one midi:MidiEvent the plugin wrote, as a CLAP event at its frame. A message of up to three bytes is a
 * CLAP_EVENT_MIDI, a sysex a CLAP_EVENT_MIDI_SYSEX, anything else is not a message a CLAP port carries and is not passed. */
static void midi_out_event(void *ctx, int64_t frame, const uint8_t *data, uint32_t size)
{
    struct shim *s = ctx;
    const uint32_t time = frame < 0 ? 0u : frame_in(s, (uint32_t)(frame > UINT32_MAX ? UINT32_MAX : frame));
    clap_event_midi_t m;
    clap_event_midi_sysex_t x;

    if (!size || data[0] < 0x80)
        return;
    if (data[0] == 0xf0)
    {
        memset(&x, 0, sizeof(x));
        x.header.size = sizeof(x);
        x.header.time = time;
        x.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        x.header.type = CLAP_EVENT_MIDI_SYSEX;
        // the bytes stay in the plugin's output buffer: valid for this process() call only, as CLAP gives an output
        // event's buffer; the next run() writes over them
        x.buffer = data;
        x.size = size;
        s->out_events->try_push(s->out_events, &x.header);
    }
    else if (size <= 3)
    {
        memset(&m, 0, sizeof(m));
        m.header.size = sizeof(m);
        m.header.time = time;
        m.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        m.header.type = CLAP_EVENT_MIDI;
        memcpy(m.data, data, size);
        s->out_events->try_push(s->out_events, &m.header);
    }
}


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

    g_last_refusal[0] = '\0';
    if (lv2_plugin_load(s->lv2, why) != 0)
    {
        refuse(s->host, why);
        return false;
    }
    s->control_of_port = malloc((s->lv2->n_ports ? s->lv2->n_ports : 1u) * sizeof(*s->control_of_port));
    s->in = lv2_instance_new(s->lv2);
    if (!s->control_of_port || !s->in)
    {
        refuse(s->host, LV2_CODE_NO_REALISATION);
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

    g_last_refusal[0] = '\0';
    if (lv2_instance_activate(s->in, rate, min_frames, max_frames, why) != 0)
    {
        refuse(s->host, why);
        return false;
    }
    s->active = 1;
    s->broken = 0;
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
    struct shim *s = SHIM_OF(plugin);

    if (lv2_instance_reset(s->in) != 0)
    {
        s->broken = 1;
        host_log(s->host, CLAP_LOG_ERROR, "reset: the LV2 worker could not start again; process fails until the next activate");
    }
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

/* The host's transport as the time:Position fields (§6.6), the frame from steady_time: 0 when there is no transport */
static int time_of(const clap_process_t *process, struct lv2_time *out)
{
    const clap_event_transport_t *t = process->transport;

    if (!t)
        return 0;
    memset(out, 0, sizeof(*out));
    out->frame = process->steady_time;
    out->playing = (t->flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
    if (t->flags & CLAP_TRANSPORT_HAS_TEMPO)
    {
        out->has |= LV2_TIME_HAS_BPM;
        out->bpm = t->tempo;
    }
    if (t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE)
    {
        out->has |= LV2_TIME_HAS_BEATS;
        out->bar = t->bar_number;
        out->bar_beat = (double)(t->song_pos_beats - t->bar_start) / (double)CLAP_BEATTIME_FACTOR;
    }
    if (t->flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE)
    {
        out->has |= LV2_TIME_HAS_METER;
        out->beat_unit = t->tsig_denom;
        out->beats_per_bar = t->tsig_num;
    }
    return 1;
}

static clap_process_status plugin_process(const clap_plugin_t *plugin, const clap_process_t *process)
{
    struct shim *s = SHIM_OF(plugin);
    const uint32_t n = process->frames_count, legs = s->lv2->legs, m = main_inputs(s->lv2);
    struct lv2_counters c;
    uint32_t leg;

    if (!s->active || s->broken || n > s->max_frames)
        return CLAP_PROCESS_ERROR;
    for (leg = 0; leg < s->lv2->in_legs; leg++)
    {
        float *dst = lv2_instance_audio_in(s->in, leg);
        const clap_audio_buffer_t *b = process->audio_inputs_count ? &process->audio_inputs[0] : NULL;

        if (b && b->data32 && b->channel_count)
            memcpy(dst, b->data32[leg < b->channel_count ? leg : 0], n * sizeof(float));
        else
            memset(dst, 0, n * sizeof(float));
    }
    for (leg = 0; leg < s->lv2->n_side; leg++)
    {
        const clap_audio_buffer_t *b = process->audio_inputs_count > m + leg ? &process->audio_inputs[m + leg] : NULL;
        uint32_t c;

        for (c = 0; c < s->lv2->side_channels[leg]; c++)
        {
            float *dst = lv2_instance_side_in(s->in, s->lv2->side_base[leg] + c);

            if (b && b->data32 && c < b->channel_count && b->data32[c])
                memcpy(dst, b->data32[c], n * sizeof(float));
            else
                memset(dst, 0, n * sizeof(float));
        }
    }
    for (leg = 0; leg < s->lv2->n_cv_in; leg++)
    {
        const uint32_t port = m + s->lv2->n_side + leg;
        const clap_audio_buffer_t *b = process->audio_inputs_count > port ? &process->audio_inputs[port] : NULL;
        float *dst = lv2_instance_cv_in(s->in, leg);

        if (b && b->data32 && b->channel_count && b->data32[0])
            memcpy(dst, b->data32[0], n * sizeof(float));
        else
            memset(dst, 0, n * sizeof(float));
    }
    {
        struct lv2_time tm;

        lv2_instance_time(s->in, time_of(process, &tm) ? &tm : NULL);
    }
    s->events = process->in_events;
    s->out_events = process->out_events;
    s->frames = n;
    if (s->lv2->midi_in_port >= 0)
        midi_in_events(s, process->in_events);
    lv2_instance_run(s->in, n, write_controls, s);
    s->events = NULL;
    emit_patch_changes(s, process->out_events);
    if (s->lv2->midi_out_port >= 0 && s->out_events)
        lv2_instance_midi_out_each(s->in, midi_out_event, s);
    s->out_events = NULL;
    if (process->audio_outputs_count && process->audio_outputs[0].data32)
    {
        const clap_audio_buffer_t *b = &process->audio_outputs[0];

        for (leg = 0; leg < b->channel_count; leg++)
            memcpy(b->data32[leg], lv2_instance_audio_out(s->in, leg < legs ? leg : 0), n * sizeof(float));
    }
    for (leg = 0; leg < s->lv2->n_cv_out; leg++)
    {
        const clap_audio_buffer_t *b = process->audio_outputs_count > 1u + leg ? &process->audio_outputs[1u + leg] : NULL;

        if (b && b->data32 && b->channel_count && b->data32[0])
            memcpy(b->data32[0], lv2_instance_cv_out(s->in, leg), n * sizeof(float));
    }
    latency_watch(s, n);
    lv2_instance_counters(s->in, &c);
    if (memcmp(&c, &s->logged, sizeof(c)) != 0 && !atomic_exchange_explicit(&s->counters_moved, 1u, memory_order_relaxed))
        s->host->request_callback(s->host);
    return CLAP_PROCESS_CONTINUE;
}

/* the longest state a load reads: the same bound as the core's CLAP_HOST_STATE_MAX_BYTES */
#define LV2_CLAP_STATE_MAX_BYTES 1048576u

/* the whole stream, read until the host returns 0: NULL-terminated, malloc'd; false when the host fails, the state runs
 * past LV2_CLAP_STATE_MAX_BYTES, or memory runs out */
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
        if (n > LV2_CLAP_STATE_MAX_BYTES)
        {
            free(buf);
            return false;
        }
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
    if (!strcmp(id, CLAP_EXT_NOTE_PORTS))
        return &g_note_ports;
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
             "lv2 counters: schedule_refused %u responses_refused %u respond_strikes %u map_on_audio %u schedule_off_audio %u log_on_audio %u patch_dropped %u midi_in_dropped %u make_path_refused %u",
             c.schedule_refused, c.responses_refused, c.respond_strikes, c.map_on_audio, c.schedule_off_audio, c.log_on_audio,
             c.patch_dropped, c.midi_in_dropped, c.make_path_refused);
    host_log(s->host, CLAP_LOG_WARNING, line);
    // a state:makePath refusal names its code, the last one's, as a refusal of its own
    if (c.make_path_refused != s->logged.make_path_refused)
        host_log(s->host, CLAP_LOG_ERROR, lv2_instance_make_path_why(s->in));
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

    g_last_refusal[0] = '\0';
    if (!host || !id || !clap_version_is_compatible(host->clap_version))
        return NULL;
    lv2 = lv2_plugin_open(slot->bundle, id, why);
    if (!lv2)
    {
        refuse(host, why);
        return NULL;
    }
    s = calloc(1, sizeof(*s));
    if (!s)
    {
        lv2_plugin_close(lv2);
        refuse(host, LV2_CODE_NO_REALISATION);
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

/*
************************************************************************************************************************
*           LOCAL FUNCTIONS: THE PRESET-DISCOVERY FACTORY
************************************************************************************************************************
*/

/*
 * A bundle's presets as CLAP lists them: one provider whose one location is the bundle itself (a PLUGIN location, so
 * nothing is read from a path), and each pset:Preset applying to one of the bundle's plugins a preset of it, named by
 * its rdfs:label, its URI the load key clap.preset-load takes (preset_from_location), for the plugin whose descriptor id
 * is the plugin's URI.
 */
#define LV2_PRESET_PROVIDER "org.openmixer.lv2.pset"

static const clap_preset_discovery_provider_descriptor_t g_presets_desc = { CLAP_VERSION_INIT, LV2_PRESET_PROVIDER, "LV2 presets",
                                                                            "omx-clap-host" };

struct preset_provider
{
    clap_preset_discovery_provider_t provider;
    const clap_preset_discovery_indexer_t *indexer;
    struct slot *slot;
};

static struct slot *slot_of_presets(const clap_preset_discovery_factory_t *factory)
{
    return (struct slot *)((const char *)factory - offsetof(struct slot, presets));
}

static bool provider_init(const clap_preset_discovery_provider_t *provider)
{
    const struct preset_provider *p = provider->provider_data;
    const clap_preset_discovery_location_t bundle = { CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT, "bundle",
                                                      CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, NULL };

    return p->indexer->declare_location(p->indexer, &bundle);
}

static void provider_destroy(const clap_preset_discovery_provider_t *provider)
{
    free(provider->provider_data);
}

static int provider_preset(void *ctx, const char *plugin_uri, const char *preset_uri, const char *label)
{
    const clap_preset_discovery_metadata_receiver_t *r = ctx;
    const clap_universal_plugin_id_t id = { "clap", plugin_uri };

    if (!r->begin_preset(r, label, preset_uri))
        return 1;
    r->add_plugin_id(r, &id);
    return 0;
}

static bool provider_get_metadata(const clap_preset_discovery_provider_t *provider, uint32_t kind, const char *location,
                                  const clap_preset_discovery_metadata_receiver_t *receiver)
{
    const struct preset_provider *p = provider->provider_data;

    (void)location;
    if (kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || !receiver)
        return false;
    return lv2_bundle_presets(p->slot->bundle, provider_preset, (void *)receiver) >= 0;
}

static const void *provider_get_extension(const clap_preset_discovery_provider_t *provider, const char *id)
{
    (void)provider;
    (void)id;
    return NULL;
}

static uint32_t presets_count(const clap_preset_discovery_factory_t *factory)
{
    (void)factory;
    return 1;
}

static const clap_preset_discovery_provider_descriptor_t *presets_descriptor(const clap_preset_discovery_factory_t *factory, uint32_t index)
{
    (void)factory;
    return index == 0 ? &g_presets_desc : NULL;
}

static const clap_preset_discovery_provider_t *presets_create(const clap_preset_discovery_factory_t *factory,
                                                              const clap_preset_discovery_indexer_t *indexer, const char *id)
{
    struct preset_provider *p;

    if (!indexer || !id || strcmp(id, LV2_PRESET_PROVIDER) != 0 || !(p = calloc(1, sizeof(*p))))
        return NULL;
    p->indexer = indexer;
    p->slot = slot_of_presets(factory);
    p->provider = (clap_preset_discovery_provider_t){ &g_presets_desc, p, provider_init, provider_destroy, provider_get_metadata,
                                                      provider_get_extension };
    return &p->provider;
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
    if (index >= OMX_CLAP_LV2_BUNDLES || !g_slots[index].refs || !id)
        return NULL;
    if (!strcmp(id, CLAP_PRESET_DISCOVERY_FACTORY_ID))
        return &g_slots[index].presets;
    if (strcmp(id, CLAP_PLUGIN_FACTORY_ID) != 0)
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
    config.state_root = cfg->state_root;
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

int omx_clap_lv2_feature_options(const struct omx_lv2_feature_options *o, char why[OMX_CLAP_LV2_WHY_MAX])
{
    char core_why[LV2_CORE_WHY_MAX];

    if (lv2_core_feature_options_set(o, core_why) != 0)
    {
        why_set(why, core_why);
        return -1;
    }
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
    slot->presets.count = presets_count;
    slot->presets.get_descriptor = presets_descriptor;
    slot->presets.create = presets_create;
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

const char *omx_clap_lv2_last_refusal(void)
{
    return g_last_refusal;
}

const char *omx_clap_lv2_param_symbol(const clap_plugin_t *plugin, clap_id id)
{
    const struct shim *s = plugin && plugin->process == plugin_process && plugin->plugin_data ? SHIM_OF(plugin) : NULL;
    const int32_t k = s && s->control_of_port ? control_of_id(s, id) : -1;

    return k >= 0 ? s->lv2->controls[k].symbol : NULL;
}

void omx_clap_lv2_worker_quiesce(const clap_plugin_t *plugin)
{
    if (plugin && plugin->plugin_data)
        lv2_instance_worker_quiesce(SHIM_OF(plugin)->in);
}

uint32_t omx_clap_lv2_saved_dir(const clap_plugin_t *plugin, char *buf, uint32_t size)
{
    const struct shim *s = plugin && plugin->process == plugin_process && plugin->plugin_data ? SHIM_OF(plugin) : NULL;
    const char *dir = s && s->in ? lv2_instance_saved_dir(s->in) : "";

    if (buf && size)
        snprintf(buf, size, "%s", dir);
    return (uint32_t)strlen(dir);
}

int omx_clap_lv2_saved_drop(const char *saved_dir, char why[OMX_CLAP_LV2_WHY_MAX])
{
    char core_why[LV2_CORE_WHY_MAX];

    if (lv2_saved_drop(saved_dir, core_why) != 0)
    {
        why_set(why, core_why);
        return -1;
    }
    why_set(why, "");
    return 0;
}

/* the shim of a plugin this adapter created, NULL for any other */
static struct shim *shim_of_any(const clap_plugin_t *plugin)
{
    return plugin && plugin->process == plugin_process && plugin->plugin_data ? SHIM_OF(plugin) : NULL;
}

/* the index of text property `uri`, or -1 */
static int32_t text_of_uri(const struct shim *s, const char *uri)
{
    uint32_t j;

    for (j = 0; s && uri && j < s->lv2->n_texts; j++)
        if (strcmp(s->lv2->texts[j].uri, uri) == 0)
            return (int32_t)j;
    return -1;
}

uint32_t omx_clap_lv2_patch_text_count(const clap_plugin_t *plugin)
{
    const struct shim *s = shim_of_any(plugin);

    return s ? s->lv2->n_texts : 0;
}

bool omx_clap_lv2_patch_text_info(const clap_plugin_t *plugin, uint32_t index, omx_clap_lv2_text_info_t *info)
{
    const struct shim *s = shim_of_any(plugin);

    if (!s || !info || index >= s->lv2->n_texts)
        return false;
    info->uri = s->lv2->texts[index].uri;
    info->name = s->lv2->texts[index].name;
    info->is_path = s->lv2->texts[index].is_path;
    info->max_bytes = lv2_plugin_text_limit(s->lv2);
    return true;
}

int omx_clap_lv2_patch_text_set(const clap_plugin_t *plugin, const char *uri, const char *value, char why[OMX_CLAP_LV2_WHY_MAX])
{
    struct shim *s = shim_of_any(plugin);
    const int32_t j = text_of_uri(s, uri);
    char core_why[LV2_CORE_WHY_MAX];

    if (j < 0 || !s->in)
    {
        why_set(why, LV2_CODE_PATCH_NOT_WRITABLE);
        return -1;
    }
    if (lv2_instance_text_set(s->in, (uint32_t)j, value, core_why) != 0)
    {
        why_set(why, core_why);
        return -1;
    }
    why_set(why, "");
    return 0;
}

uint32_t omx_clap_lv2_patch_text_get(const clap_plugin_t *plugin, const char *uri, char *buf, uint32_t size)
{
    const struct shim *s = shim_of_any(plugin);
    const int32_t j = text_of_uri(s, uri);

    if (j < 0 || !s->in)
    {
        if (buf && size)
            buf[0] = '\0';
        return 0;
    }
    return lv2_instance_text_get(s->in, (uint32_t)j, buf, size);
}
