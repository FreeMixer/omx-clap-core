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

/** @file
 * @brief The real-time body of a hosted CLAP instance. */

/*
************************************************************************************************************************
*
* The CLAP body of the RT stage, part of libomx-clap-core: a plugin's process() on the consumer's own RT thread, over
* hosted_stage.h. It loads, creates, activates and destroys nothing: that is the control thread's (clap_host.h).
*
* Per block (omx_clap_run):
*  1. Out of place, always. The lane is copied into private bounce inputs and the plugin writes private bounce outputs;
*     the audio buffers in and out are distinct, data64 NULL, latency 0, the input constant_mask 0. A mono lane is
*     mirrored onto a stereo plugin's R input and only L returns. An instrument has no input buffer: its lane is the
*     silence its dry body is.
*  2. Parameter events are drained from the lock-free ring the control thread fills into the stage's own preallocated
*     event array, CLAP_HOST_EVENTS_PER_BLOCK at most, time 0, the cookie get_info returned; a surplus stays in the ring
*     for the next block. The note events of the block, written ahead of it by omx_clap_note_in, follow them.
*     out_events.try_push is a counting sink: a CLAP_EVENT_PARAM_VALUE from the plugin raises plugin_changed and nothing
*     is stored.
*  3. process() with steady_time a running frame counter from activate, never -1, transport NULL (a processing block
*     of omx_clap_run_transport carries the host's transport; the warm-up never does). CLAP_PROCESS_ERROR
*     discards the block (the lane passes dry); SLEEP, CONTINUE_IF_NOT_QUIET and TAIL are CONTINUE: a published plugin is
*     never put to sleep.
*  4. Flush-to-zero is re-asserted after process(): the mode is per thread.
*  5. A constant output channel is expanded from its sample 0 before the scan and the clamp.
*  6. The scan for non-finite output, the clamp, the one-block crossfade on every change of body and the bit-identical
*     steady bypass are hosted_stage.h's, as the stage's guards say. A steady bypass marks the stage need_reset; the
*     body never calls reset(): the host's re-engage (omx_clap_host_bypass) does, on the control thread holding the
*     audio role, before the stage resumes.
*  7. The latency is not read on the RT: latency.get() is [main-thread]; the control thread publishes it.
*  8. start_processing and stop_processing run on the audio thread, driven by the state the control thread moves by
*     compare-and-swap: ARMED to PROCESSING on the first block after publish, STOPPING to STOPPED on the first block
*     that sees it. HELD is the control thread standing in for the audio thread: a cycle that arrives meanwhile passes
*     the lane through, and the control thread waits for the cycle under way, marked by in_cycle.
*
************************************************************************************************************************
*/

#ifndef CLAP_STAGE_H
#define CLAP_STAGE_H


/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <clap/audio-buffer.h>
#include <clap/events.h>
#include <clap/ext/note-ports.h>
#include <clap/plugin.h>
#include <clap/process.h>

#include "hosted_stage.h"


/*
************************************************************************************************************************
*           DATA TYPES
************************************************************************************************************************
*/

/** The published state the control thread and the RT hand back and forth. */
enum omx_clap_state
{
    OMX_CLAP_IDLE = 0,          ///< not published; the control thread holds the audio role
    OMX_CLAP_ARMED,             ///< published: the RT's first block calls start_processing
    OMX_CLAP_PROCESSING,        ///< the plugin processes every block
    OMX_CLAP_STOPPING,          ///< the control thread asked: the RT's next block stops
    OMX_CLAP_STOPPED,           ///< stop_processing done; the control thread may deactivate
    OMX_CLAP_HELD               ///< the control thread stands in for the audio thread; a block passes the lane through
};

/** the parameter ring is the hosted stage's; a record's id is the clap_id, its cookie what get_info returned */
#define omx_clap_param_record omx_hosted_param_record
#define omx_clap_param_queue omx_hosted_param_queue  ///< the parameter queue of the hosted core
#define omx_clap_queue_init omx_hosted_queue_init  ///< omx_hosted_queue_init
#define omx_clap_queue_push omx_hosted_queue_push  ///< omx_hosted_queue_push
#define omx_clap_queue_pending omx_hosted_queue_pending  ///< omx_hosted_queue_pending

/** the sysex messages one block may carry; a sysex past it is counted in sysex_dropped */
#define OMX_CLAP_SYSEX_PER_BLOCK        8u
/** the longest sysex the stage carries; a longer one is counted in notes_unmappable, never held past its block */
#define OMX_CLAP_SYSEX_MAX_BYTES        256u

/** the pitch-bend range in semitones a stage starts with: MIDI's default, since RPN 0 (pitch bend sensitivity) is not tracked */
#define OMX_CLAP_BEND_SEMITONES_DEFAULT 2.0

/** Every event the host hands a plugin ahead of a block besides the parameter writes */
union omx_clap_note
{
    clap_event_header_t header;     ///< the common header
    clap_event_note_t note;         ///< a CLAP note event
    clap_event_midi_t midi;         ///< a MIDI event
    clap_event_note_expression_t expr;  ///< a CLAP note expression: pitch bend and pressure in the CLAP dialect
    clap_event_midi_sysex_t sysex;  ///< a sysex message, its bytes in the stage's pool
};

/** The RT stage of one hosted CLAP instance: the hosted core, the bound plugin, the block's events and the counters. */
struct omx_clap_stage
{
    // the hosted core, the head of the stage: the bounce, the legs, bypass, guards, fault and the counters
    struct omx_hosted_stage h;                          ///< the hosted core, at offset 0

    // bound by the control thread before publish (omx_clap_stage_init, omx_clap_bind)
    const clap_plugin_t *plugin;                        ///< the hosted plugin
    float *in_ptrs[2],                                  ///< what the input buffer's data32 points at
          *out_ptrs[2];                                 ///< what the output buffer's data32 points at
    clap_audio_buffer_t ain;                            ///< distinct from the outputs, over the bounce
    clap_audio_buffer_t aout[1 + CLAP_HOST_AUX_OUTPUTS];///< the main output, then one scratch-backed buffer per auxiliary output
    uint32_t n_aux;                                     ///< auxiliary output ports, after aout[0]
    float *aux_ptrs[2];                                 ///< the scratch pair every auxiliary output's data32 points at
    clap_event_param_value_t events[CLAP_HOST_EVENTS_PER_BLOCK];   ///< this block's parameter events
    uint32_t n_events;                                  ///< the events in `events`
    union omx_clap_note notes[CLAP_HOST_NOTES_PER_BLOCK];  ///< this block's notes, written ahead of it by the RT thread
    uint32_t n_notes;                                   ///< the notes in `notes`
    uint8_t sysex_bytes[OMX_CLAP_SYSEX_PER_BLOCK][OMX_CLAP_SYSEX_MAX_BYTES];  ///< the bytes this block's sysex slots point at
    uint32_t n_sysex;                                   ///< the sysex messages in `sysex_bytes`
    _Atomic uint32_t notes_visible;                     ///< 1 only while the plugin is inside process()
    uint32_t note_inputs;                               ///< 0 or 1 note input
    uint32_t note_dialect;                              ///< CLAP_NOTE_DIALECT_CLAP or CLAP_NOTE_DIALECT_MIDI: how notes travel
    uint32_t note_dialects;                             ///< every dialect the note input's port declared, not just note_dialect
    double bend_semitones;                              ///< full-scale pitch bend as CLAP tuning, in semitones either way; the host sets it
                                                        ///< before arming, the RT reads it with no lock (default OMX_CLAP_BEND_SEMITONES_DEFAULT)
    clap_input_events_t in_events;                      ///< the input event list handed to process()
    clap_output_events_t out_events;                    ///< the counting sink handed to process()
    clap_process_t proc;                                ///< the process structure handed to the plugin
    struct omx_clap_param_queue queue;                  ///< the parameter writes waiting for the next block
    int64_t steady_time;                                ///< frames processed since activate; the RT and the warm-up advance it

    // control thread -> RT
    _Atomic uint32_t state;                             ///< enum omx_clap_state
    _Atomic uint32_t latency_frames;                    ///< published by the control thread after activate

    // RT-owned; the control thread reads them relaxed
    _Atomic uint32_t in_cycle;                          ///< 1 while omx_clap_run is under way
    uint32_t need_reset;                                ///< a steady bypass happened since the last process(): the RT marks it,
                                                        ///< the control thread resets the plugin at re-engage holding the role
    _Atomic uint32_t process_errors;                    ///< of which CLAP_PROCESS_ERROR returns
    _Atomic uint32_t constant_channels;                 ///< output channels the plugin reported constant, expanded
    _Atomic uint32_t events_delivered;                  ///< parameter events drained into process()
    _Atomic uint32_t notes_delivered;                   ///< note events handed to process()
    _Atomic uint32_t notes_dropped;                     ///< note events past CLAP_HOST_NOTES_PER_BLOCK
    _Atomic uint32_t sysex_dropped;                     ///< a sysex message past OMX_CLAP_SYSEX_PER_BLOCK
    _Atomic uint32_t notes_unmappable;                  ///< a channel message with no mapping into the plugin's dialect: counted, never silently dropped
    _Atomic uint32_t out_events_seen;                   ///< events the plugin pushed at the counting sink
    _Atomic uint32_t plugin_changed;                    ///< a CLAP_EVENT_PARAM_VALUE came back: read-back due
    _Atomic uint32_t resets;                            ///< reset() calls on re-engage, made by the control thread
    _Atomic uint32_t start_refused;                     ///< start_processing() answered false
};

_Static_assert(offsetof(struct omx_clap_stage, h) == 0, "the hosted core heads the CLAP stage");


/*
************************************************************************************************************************
*           THE EVENT LISTS
************************************************************************************************************************
*/

/** The notes the plugin sees: this block's while it is inside process(), none otherwise. */
static inline uint32_t omx_clap_notes_in_view(const struct omx_clap_stage *s)
{
    return atomic_load_explicit(&s->notes_visible, memory_order_acquire) ? s->n_notes : 0;
}

/** The input event list's size: the parameter events, then the visible notes. */
static inline uint32_t omx_clap_in_size(const struct clap_input_events *list)
{
    const struct omx_clap_stage *s = (const struct omx_clap_stage *)list->ctx;

    return s->n_events + omx_clap_notes_in_view(s);
}

/** The input event list's event at `index`: the parameter events, then the visible notes. */
static inline const clap_event_header_t *omx_clap_in_get(const struct clap_input_events *list, uint32_t index)
{
    const struct omx_clap_stage *s = (const struct omx_clap_stage *)list->ctx;

    if (index < s->n_events)
        return &s->events[index].header;
    index -= s->n_events;
    return index < omx_clap_notes_in_view(s) ? &s->notes[index].header : NULL;
}

static inline bool omx_clap_out_push(const struct clap_output_events *list, const clap_event_header_t *event)
{
    struct omx_clap_stage *s = (struct omx_clap_stage *)list->ctx;

    omx_hosted_count(&s->out_events_seen, 1);
    if (event != NULL && event->space_id == CLAP_CORE_EVENT_SPACE_ID && event->type == CLAP_EVENT_PARAM_VALUE)
        atomic_store_explicit(&s->plugin_changed, 1u, memory_order_relaxed);
    return true;    ///< accepted and counted; nothing is stored on the RT
}


/*
************************************************************************************************************************
*           THE CONTROL THREAD'S SIDE OF THE STAGE
************************************************************************************************************************
*/

/**
 * Control thread, before create_plugin: zero the stage and bind its memory. Returns -1 (stage unusable) unless the four
 * bounce buffers are present and distinct and the ring's records are a power-of-two capacity.
 */
static inline int omx_clap_stage_init(struct omx_clap_stage *s, const struct omx_hosted_bounce *bounce,
                                      struct omx_clap_param_record *recs, uint32_t rec_cap)
{
    if (omx_hosted_stage_init(s, sizeof(*s), bounce) != 0)
        return -1;
    if (omx_clap_queue_init(&s->queue, recs, rec_cap) != 0)
        return -1;
    s->in_ptrs[0] = s->h.in_l;
    s->in_ptrs[1] = s->h.in_r;
    s->out_ptrs[0] = s->h.out_l;
    s->out_ptrs[1] = s->h.out_r;
    s->bend_semitones = OMX_CLAP_BEND_SEMITONES_DEFAULT;
    s->in_events.ctx = s;
    s->in_events.size = omx_clap_in_size;
    s->in_events.get = omx_clap_in_get;
    s->out_events.ctx = s;
    s->out_events.try_push = omx_clap_out_push;
    return 0;
}

/**
 * Control thread, after init: the plugin's auxiliary output ports, `channels[i]` wide (1 or 2 each, up to
 * CLAP_HOST_AUX_OUTPUTS of them), every one handed the same scratch pair: the plugin writes it and nothing reads it.
 * Returns -1 on a width or a count the stage cannot hold.
 */
static inline int omx_clap_bind_aux(struct omx_clap_stage *s, float *scratch_l, float *scratch_r, uint32_t count,
                                    const uint32_t *channels)
{
    uint32_t i;

    if (count > CLAP_HOST_AUX_OUTPUTS || (count && (scratch_l == NULL || scratch_r == NULL || channels == NULL)))
        return -1;
    s->aux_ptrs[0] = scratch_l;
    s->aux_ptrs[1] = scratch_r;
    for (i = 0; i < count; i++)
    {
        if (channels[i] == 0 || channels[i] > 2)
            return -1;
        s->aout[1 + i].data32 = s->aux_ptrs;
        s->aout[1 + i].data64 = NULL;
        s->aout[1 + i].channel_count = channels[i];
        s->aout[1 + i].latency = 0;
        s->aout[1 + i].constant_mask = 0;
    }
    s->n_aux = count;
    return 0;
}

/**
 * Control thread, after init and activate: bind the plugin and its main port widths, `n_in` 0 (an instrument), 1 or 2
 * and `n_out` 1 or 2. The audio buffers point at the bounce once, the addresses never move, and every fixed field of
 * clap_process_t is set here, so the RT writes only frames_count and steady_time.
 */
static inline int omx_clap_bind_ports(struct omx_clap_stage *s, const clap_plugin_t *plugin, uint32_t n_in, uint32_t n_out)
{
    if (s == NULL || plugin == NULL || plugin->process == NULL)
        return -1;
    if (n_in > 2 || n_out < 1 || n_out > 2)
        return -1;
    s->plugin = plugin;
    s->h.n_in = n_in;
    s->h.n_out = n_out;
    s->ain.data32 = s->in_ptrs;
    s->ain.data64 = NULL;
    s->ain.channel_count = n_in;
    s->ain.latency = 0;
    s->ain.constant_mask = 0;
    s->aout[0].data32 = s->out_ptrs;
    s->aout[0].data64 = NULL;
    s->aout[0].channel_count = n_out;
    s->aout[0].latency = 0;
    s->aout[0].constant_mask = 0;
    memset(&s->proc, 0, sizeof(s->proc));
    s->proc.transport = NULL;
    s->proc.audio_inputs = n_in ? &s->ain : NULL;
    s->proc.audio_outputs = s->aout;
    s->proc.audio_inputs_count = n_in ? 1 : 0;
    s->proc.audio_outputs_count = 1 + s->n_aux;
    s->proc.in_events = &s->in_events;
    s->proc.out_events = &s->out_events;
    s->steady_time = 0;
    atomic_store_explicit(&s->h.rendered_wet, 0, memory_order_relaxed);
    s->need_reset = 0;
    return 0;
}

/** The same for an effect: `channels` in and out, 1 or 2. */
static inline int omx_clap_bind(struct omx_clap_stage *s, const clap_plugin_t *plugin, uint32_t channels)
{
    if (channels != 1 && channels != 2)
        return -1;
    return omx_clap_bind_ports(s, plugin, channels, channels);
}

/** Control -> RT: command bypass. The next block crossfades; the block after is bit-identical dry. */
static inline void omx_clap_set_bypass(struct omx_clap_stage *s, int on)
{
    atomic_store_explicit(&s->h.bypass, on ? 1u : 0u, memory_order_release);
}

/** Control thread, after the warm-up and the latency publish: arm the stage for the RT. */
static inline void omx_clap_arm(struct omx_clap_stage *s)
{
    atomic_store_explicit(&s->state, OMX_CLAP_ARMED, memory_order_release);
}

/** Control thread: ask the RT to stop; poll omx_clap_stopped off the RT before deactivating. */
static inline void omx_clap_request_stop(struct omx_clap_stage *s)
{
    uint32_t expect = OMX_CLAP_PROCESSING;

    if (!atomic_compare_exchange_strong_explicit(&s->state, &expect, OMX_CLAP_STOPPING, memory_order_acq_rel,
                                                 memory_order_acquire))
    {
        // armed and never run: nothing started, so nothing stops; idle and stopped stay as they are
        expect = OMX_CLAP_ARMED;
        atomic_compare_exchange_strong_explicit(&s->state, &expect, OMX_CLAP_STOPPED, memory_order_acq_rel,
                                                memory_order_acquire);
    }
}

/** Nonzero once the stage is stopped or idle. */
static inline int omx_clap_stopped(const struct omx_clap_stage *s)
{
    const uint32_t st = atomic_load_explicit(&s->state, memory_order_acquire);

    return st == OMX_CLAP_STOPPED || st == OMX_CLAP_IDLE;
}

/** Control thread: publish the latency it read with latency.get() after activate. */
static inline void omx_clap_publish_latency(struct omx_clap_stage *s, uint32_t frames)
{
    atomic_store_explicit(&s->latency_frames, frames, memory_order_relaxed);
}

/** Control thread: enqueue one parameter write for the RT to drain. -1: ring full. */
static inline int omx_clap_param_push(struct omx_clap_stage *s, clap_id id, double value, void *cookie)
{
    return omx_clap_queue_push(&s->queue, id, value, cookie);
}


/*
************************************************************************************************************************
*           THE RT BODY
************************************************************************************************************************
*/

/** RT: one queued write becomes one CLAP_EVENT_PARAM_VALUE in this block's event array. */
static inline void omx_clap_event_sink(void *ctx, const struct omx_clap_param_record *r)
{
    struct omx_clap_stage *s = (struct omx_clap_stage *)ctx;
    clap_event_param_value_t *e = &s->events[s->n_events++];

    e->header.size = sizeof(*e);
    e->header.time = 0;
    e->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e->header.type = CLAP_EVENT_PARAM_VALUE;
    e->header.flags = 0;
    e->param_id = r->id;
    e->cookie = r->cookie;
    e->note_id = -1;
    e->port_index = -1;
    e->channel = -1;
    e->key = -1;
    e->value = r->value;
}

/** RT: move up to CLAP_HOST_EVENTS_PER_BLOCK records into this block's event array, the surplus staying queued. */
static inline void omx_clap_drain(struct omx_clap_stage *s)
{
    uint32_t n;

    s->n_events = 0;
    n = omx_hosted_queue_drain(&s->queue, CLAP_HOST_EVENTS_PER_BLOCK, omx_clap_event_sink, s);
    if (n)
        omx_hosted_count(&s->events_delivered, n);
}

/** The next note slot of this block, filled with the header; NULL, counted as dropped, past CLAP_HOST_NOTES_PER_BLOCK. */
static inline union omx_clap_note *omx_clap_note_slot(struct omx_clap_stage *s, uint32_t time, uint16_t type, uint32_t size)
{
    union omx_clap_note *slot;

    if (s->n_notes >= CLAP_HOST_NOTES_PER_BLOCK)
    {
        omx_hosted_count(&s->notes_dropped, 1);
        return NULL;
    }
    slot = &s->notes[s->n_notes++];
    memset(slot, 0, sizeof(*slot));
    slot->header.size = size;
    slot->header.time = time;
    slot->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    slot->header.type = type;
    return slot;
}

/** RT: fill a CLAP_EVENT_MIDI slot with one raw channel message, one to three bytes. */
static inline void omx_clap_midi_slot(struct omx_clap_stage *s, uint32_t time, const uint8_t *data, size_t size)
{
    union omx_clap_note *slot = omx_clap_note_slot(s, time, CLAP_EVENT_MIDI, sizeof(clap_event_midi_t));

    if (!slot)
        return;
    slot->midi.port_index = 0;
    slot->midi.data[0] = data[0];
    slot->midi.data[1] = size > 1 ? data[1] : 0;
    slot->midi.data[2] = size > 2 ? data[2] : 0;
}

/** RT: fill a CLAP_EVENT_NOTE_EXPRESSION slot; `key` -1 for a channel-wide expression. */
static inline void omx_clap_expr_slot(struct omx_clap_stage *s, uint32_t time, int32_t expression_id, int16_t channel,
                                      int16_t key, double value)
{
    union omx_clap_note *slot = omx_clap_note_slot(s, time, CLAP_EVENT_NOTE_EXPRESSION, sizeof(clap_event_note_expression_t));

    if (!slot)
        return;
    slot->expr.expression_id = expression_id;
    slot->expr.note_id = -1;
    slot->expr.port_index = 0;
    slot->expr.channel = channel;
    slot->expr.key = key;
    slot->expr.value = value;
}

/**
 * RT, ahead of the block they belong to and in the order they arrived: one MIDI message of the note input, as the event
 * the input's dialect wants.
 *  - A MIDI-dialect input: every channel message of one to three bytes, raw, as CLAP_EVENT_MIDI; a sysex (0xf0..0xf7, any
 *    length: JACK hands one complete message) the same, as CLAP_EVENT_MIDI_SYSEX.
 *  - A CLAP-dialect input: note on (velocity above 0) and note off (or a note on with velocity 0) as CLAP note events.
 *    Every other channel message and a sysex go the same way the MIDI-dialect input takes them when the port's declared
 *    dialects also include MIDI; when they do not, pitch bend and channel or poly pressure become the
 *    CLAP_NOTE_EXPRESSION_TUNING and _PRESSURE expressions (channel-wide, or one key for poly aftertouch). A bend's
 *    full scale is `bend_semitones` either way (default 2, MIDI's default range; RPN 0 is not tracked, so a host that
 *    wants another range sets the field before arming): value = (bend14 - 8192) / 8192 * bend_semitones. Anything
 *    still unmappable (a controller, a program change, a sysex) is counted, never silently dropped.
 * A system real-time byte besides sysex is dropped; a channel message longer than three bytes is counted as unmappable. Past CLAP_HOST_NOTES_PER_BLOCK a message is counted and dropped, and
 * past OMX_CLAP_SYSEX_PER_BLOCK a sysex is the same. A stage with no note input takes none.
 */
static inline void omx_clap_note_in(struct omx_clap_stage *s, uint32_t time, const uint8_t *data, size_t size)
{
    const int also_midi = (s->note_dialects & CLAP_NOTE_DIALECT_MIDI) != 0;
    uint8_t type, channel;
    union omx_clap_note *slot;

    if (!s->note_inputs || size == 0 || data[0] < 0x80)
        return;

    if (data[0] == 0xf0)        // sysex: one complete message, any length
    {
        if (!also_midi || size > OMX_CLAP_SYSEX_MAX_BYTES)
        {
            omx_hosted_count(&s->notes_unmappable, 1);
            return;
        }
        if (s->n_sysex >= OMX_CLAP_SYSEX_PER_BLOCK)
        {
            omx_hosted_count(&s->sysex_dropped, 1);
            return;
        }
        slot = omx_clap_note_slot(s, time, CLAP_EVENT_MIDI_SYSEX, sizeof(clap_event_midi_sysex_t));
        if (!slot)
            return;
        memcpy(s->sysex_bytes[s->n_sysex], data, size);
        slot->sysex.port_index = 0;
        slot->sysex.buffer = s->sysex_bytes[s->n_sysex];
        slot->sysex.size = (uint32_t)size;
        s->n_sysex++;
        return;
    }

    if (data[0] >= 0xf0)
        return;                 // a system real-time byte: not ours
    if (size > 3)
    {
        omx_hosted_count(&s->notes_unmappable, 1);  // a channel message longer than one can be: no dialect carries it
        return;
    }

    type = data[0] & 0xf0;
    channel = data[0] & 0x0f;

    if (s->note_dialect == CLAP_NOTE_DIALECT_MIDI)
    {
        omx_clap_midi_slot(s, time, data, size);
        return;
    }

    if ((type == 0x80 || type == 0x90) && size == 3)
    {
        slot = omx_clap_note_slot(s, time, type == 0x90 && data[2] ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF, sizeof(clap_event_note_t));
        if (!slot)
            return;
        slot->note.note_id = -1;
        slot->note.port_index = 0;
        slot->note.channel = (int16_t)channel;
        slot->note.key = data[1];
        slot->note.velocity = (double)data[2] / 127.0;
        return;
    }

    if (also_midi)
    {
        omx_clap_midi_slot(s, time, data, size);
        return;
    }

    if (type == 0xe0 && size == 3)              // pitch bend -> relative tuning, the whole channel
    {
        const int bend14 = ((int)data[2] << 7 | data[1]) - 8192;

        omx_clap_expr_slot(s, time, CLAP_NOTE_EXPRESSION_TUNING, (int16_t)channel, -1, (double)bend14 / 8192.0 * s->bend_semitones);
        return;
    }
    if (type == 0xd0 && size == 2)              // channel pressure -> the whole channel
    {
        omx_clap_expr_slot(s, time, CLAP_NOTE_EXPRESSION_PRESSURE, (int16_t)channel, -1, (double)data[1] / 127.0);
        return;
    }
    if (type == 0xa0 && size == 3)              // poly aftertouch -> one key of the channel
    {
        omx_clap_expr_slot(s, time, CLAP_NOTE_EXPRESSION_PRESSURE, (int16_t)channel, (int16_t)data[1], (double)data[2] / 127.0);
        return;
    }
    omx_hosted_count(&s->notes_unmappable, 1);  // a controller or a program change: no CLAP note expression carries it
}

/** RT: fill every output channel the plugin reported constant from its sample 0. */
static inline void omx_clap_expand_constant(struct omx_clap_stage *s, uint32_t n)
{
    const uint64_t mask = s->aout[0].constant_mask;
    uint32_t c, i;

    if (mask == 0)
        return;
    for (c = 0; c < s->h.n_out; c++)
    {
        float *b;
        float v;

        if (!(mask & ((uint64_t)1 << c)))
            continue;
        b = s->out_ptrs[c];
        v = b[0];
        for (i = 1; i < n; i++)
            b[i] = v;
        omx_hosted_count(&s->constant_channels, 1);
    }
}

/**
 * The plugin's half of a block: drain, process, flush-to-zero, expand. Shared by the live body and the warm-up so the
 * warm-up exercises exactly the path the RT will. Returns 1 when the output is usable, 0 when the block must be
 * discarded (a process error or, for a stage that scans, a non-finite sample).
 */
static inline int omx_clap_run_plugin(struct omx_clap_stage *s, uint32_t n)
{
    clap_process_status status;

    omx_clap_drain(s);
    s->aout[0].constant_mask = 0;   ///< the plugin's to set; never carried from the last block
    s->ain.constant_mask = 0;
    s->proc.steady_time = s->steady_time;
    s->proc.frames_count = n;
    if (s->note_inputs)
        atomic_store_explicit(&s->notes_visible, 1, memory_order_release);
    status = s->plugin->process(s->plugin, &s->proc);
    if (s->note_inputs)
    {
        atomic_store_explicit(&s->notes_visible, 0, memory_order_release);
        omx_hosted_count(&s->notes_delivered, s->n_notes);
    }
    omx_hosted_denormals_off();
    s->steady_time += (int64_t)n;
    s->n_events = 0;
    if (status == CLAP_PROCESS_ERROR)
    {
        omx_hosted_count(&s->process_errors, 1);
        return 0;
    }
    omx_clap_expand_constant(s, n);
    return omx_hosted_out_finite(&s->h, n);
}

/**
 * The transport a host holds, as a processing block hands it to the plugin: `flags` the CLAP_TRANSPORT_* bits that
 * are valid (0: none, and the plugin gets a NULL transport), and each value under its flag. Beats and seconds are
 * CLAP's fixed point (clap_beattime, clap_sectime).
 */
struct omx_clap_transport
{
    uint32_t flags;  ///< CLAP_TRANSPORT_* bits that are valid; 0 for none
    double tempo;  ///< bpm, under CLAP_TRANSPORT_HAS_TEMPO
    clap_beattime song_pos_beats;  ///< position in beats, under CLAP_TRANSPORT_HAS_BEATS_TIMELINE
    clap_sectime song_pos_seconds;  ///< position in seconds, under CLAP_TRANSPORT_HAS_SECONDS_TIMELINE
    clap_beattime bar_start;  ///< start of the current bar, under CLAP_TRANSPORT_HAS_BEATS_TIMELINE
    int32_t bar_number;  ///< the bar at song position 0 is number 0, under CLAP_TRANSPORT_HAS_BEATS_TIMELINE
    uint16_t tsig_num;  ///< time signature numerator, under CLAP_TRANSPORT_HAS_TIME_SIGNATURE
    uint16_t tsig_denom;  ///< time signature denominator, under CLAP_TRANSPORT_HAS_TIME_SIGNATURE
};

/**
 * A host's published transport: the fields as atomic words, guarded by a sequence (odd while a publish is under way).
 * The control thread writes it whole with omx_clap_transport_publish; the audio role reads it whole with
 * omx_clap_transport_read, so one block never pairs the fields of two publishes. Its storage is the caller's and must
 * outlive the instance's publication.
 */
struct omx_clap_transport_src
{
    _Atomic uint32_t seq;  ///< odd while a publish is under way
    _Atomic uint32_t flags;  ///< the CLAP_TRANSPORT_* bits that are valid
    _Atomic uint64_t tempo_bits;  ///< the bpm as the bits of a double
    _Atomic clap_beattime song_pos_beats;  ///< position in beats
    _Atomic clap_sectime song_pos_seconds;  ///< position in seconds
    _Atomic clap_beattime bar_start;  ///< start of the current bar
    _Atomic int32_t bar_number;  ///< the bar number
    _Atomic uint16_t tsig_num;  ///< time signature numerator
    _Atomic uint16_t tsig_denom;  ///< time signature denominator
};

/** Control thread: publish `t` whole. Seen by the next block that reads it; the sequence is odd while it is written. */
static inline void omx_clap_transport_publish(struct omx_clap_transport_src *src, const struct omx_clap_transport *t)
{
    uint64_t bpm_bits = 0;
    uint32_t seq = atomic_load_explicit(&src->seq, memory_order_relaxed);

    memcpy(&bpm_bits, &t->tempo, sizeof bpm_bits);
    atomic_store_explicit(&src->seq, seq + 1u, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&src->flags, t->flags, memory_order_relaxed);
    atomic_store_explicit(&src->tempo_bits, bpm_bits, memory_order_relaxed);
    atomic_store_explicit(&src->song_pos_beats, t->song_pos_beats, memory_order_relaxed);
    atomic_store_explicit(&src->song_pos_seconds, t->song_pos_seconds, memory_order_relaxed);
    atomic_store_explicit(&src->bar_start, t->bar_start, memory_order_relaxed);
    atomic_store_explicit(&src->bar_number, t->bar_number, memory_order_relaxed);
    atomic_store_explicit(&src->tsig_num, t->tsig_num, memory_order_relaxed);
    atomic_store_explicit(&src->tsig_denom, t->tsig_denom, memory_order_relaxed);
    atomic_store_explicit(&src->seq, seq + 2u, memory_order_release);
}

/**
 * RT: read the published transport into `out`, whole. Retries a few times while a publish runs over it; 0 when none of
 * the tries reads whole, and `out` is then left as the caller had it (the block keeps the last record it read).
 */
static inline int omx_clap_transport_read(const struct omx_clap_transport_src *src, struct omx_clap_transport *out)
{
    for (int tries = 0; tries < 4; tries++)
    {
        const uint32_t s1 = atomic_load_explicit(&src->seq, memory_order_acquire);
        uint64_t bpm_bits;
        struct omx_clap_transport t;

        if (s1 & 1u)
            continue;
        t.flags = atomic_load_explicit(&src->flags, memory_order_relaxed);
        bpm_bits = atomic_load_explicit(&src->tempo_bits, memory_order_relaxed);
        t.song_pos_beats = atomic_load_explicit(&src->song_pos_beats, memory_order_relaxed);
        t.song_pos_seconds = atomic_load_explicit(&src->song_pos_seconds, memory_order_relaxed);
        t.bar_start = atomic_load_explicit(&src->bar_start, memory_order_relaxed);
        t.bar_number = atomic_load_explicit(&src->bar_number, memory_order_relaxed);
        t.tsig_num = atomic_load_explicit(&src->tsig_num, memory_order_relaxed);
        t.tsig_denom = atomic_load_explicit(&src->tsig_denom, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&src->seq, memory_order_relaxed) != s1)
            continue;
        memcpy(&t.tempo, &bpm_bits, sizeof t.tempo);
        *out = t;
        return 1;
    }
    return 0;
}

/**
 * The transport a processing block hands the plugin: the valid fields of `rec` under their flags, written into `t`
 * only here, inside the cycle bracket of a PROCESSING stage, and withdrawn when the block returns. A stage that is not
 * processing (idle, warming up, held by the control thread) never sees `t` written.
 */
static inline void omx_clap_transport_fill(clap_event_transport_t *t, const struct omx_clap_transport *rec)
{
    memset(t, 0, sizeof(*t));
    t->header.size = sizeof(*t);
    t->header.time = 0;
    t->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    t->header.type = CLAP_EVENT_TRANSPORT;
    t->header.flags = 0;
    t->flags = rec->flags;
    t->song_pos_beats = rec->song_pos_beats;
    t->song_pos_seconds = rec->song_pos_seconds;
    t->tempo = rec->tempo;
    t->tempo_inc = 0.0;
    t->bar_start = rec->bar_start;
    t->bar_number = rec->bar_number;
    t->tsig_num = rec->tsig_num;
    t->tsig_denom = rec->tsig_denom;
}

static inline void omx_clap_run_locked(struct omx_clap_stage *s, float *l, float *r, uint32_t n,
                                       clap_event_transport_t *t, const struct omx_clap_transport *rec)
{
    int usable;

    uint32_t st = atomic_load_explicit(&s->state, memory_order_acquire);
    uint32_t want_wet;

    if (st == OMX_CLAP_ARMED && atomic_compare_exchange_strong(&s->state, &st, OMX_CLAP_PROCESSING))
    {
        // the first block after publish: the RT takes the audio role
        if (s->plugin->start_processing == NULL || s->plugin->start_processing(s->plugin))
        {
            st = OMX_CLAP_PROCESSING;
        }
        else
        {
            omx_hosted_count(&s->start_refused, 1);
            st = OMX_CLAP_STOPPED;
            atomic_store_explicit(&s->state, st, memory_order_release);
        }
    }
    if (st == OMX_CLAP_STOPPING && atomic_compare_exchange_strong(&s->state, &st, OMX_CLAP_STOPPED))
    {
        if (s->plugin->stop_processing)
            s->plugin->stop_processing(s->plugin);
        st = OMX_CLAP_STOPPED;
    }
    if (st != OMX_CLAP_PROCESSING)
    {
        // not processing: the lane carries dry, and a body that was wet fades out once
        if (atomic_load_explicit(&s->h.rendered_wet, memory_order_relaxed) && n <= s->h.max_block)
        {
            memcpy(s->h.in_l, l, n * sizeof(float));
            memcpy(s->h.in_r, r ? r : l, n * sizeof(float));
            omx_hosted_deliver(0, 1, l, r, s->h.in_l, s->h.in_r, s->h.out_l, s->h.n_out == 2 ? s->h.out_r : s->h.out_l, n);
        }
        atomic_store_explicit(&s->h.rendered_wet, 0, memory_order_release);
        return;
    }
    switch (omx_hosted_run_begin(&s->h, l, r, n, &want_wet))
    {
        case OMX_HOSTED_SKIP:
            return;
        case OMX_HOSTED_IDLE:
            s->need_reset = 1;  ///< a tail may be frozen: the control thread resets the plugin at re-engage, never this one
            return;
        case OMX_HOSTED_GO:
            break;
    }
    if (t && rec && rec->flags)
    {
        omx_clap_transport_fill(t, rec);
        s->proc.transport = t;
    }
    usable = omx_clap_run_plugin(s, n);
    s->proc.transport = NULL;
    omx_hosted_run_end(&s->h, usable, want_wet, l, r, n);
}

/**
 * RT: the stage's body for one block, in place on the lane (`l`, and `r` or NULL for a mono lane), every guard as the
 * header comment lists them. A block longer than the bounce is passed through untouched and counted. The block marks
 * itself in_cycle, so the control thread taking the audio role waits for it, and forgets the notes it was handed.
 * omx_clap_run_transport is the same body handing a processing block the transport `rec` in `t` (NULL, or `rec` with no
 * flags: transport NULL); omx_clap_run hands none.
 */
static inline void omx_clap_run_transport(struct omx_clap_stage *s, float *l, float *r, uint32_t n,
                                          clap_event_transport_t *t, const struct omx_clap_transport *rec)
{
    if (s == NULL)
        return;
    if (s->plugin == NULL || l == NULL || n == 0)
    {
        s->n_notes = 0;     // no block follows these events: they must not reach the next one with their old frame times
        s->n_sysex = 0;
        return;
    }
    atomic_store(&s->in_cycle, 1);
    omx_clap_run_locked(s, l, r, n, t, rec);
    s->n_notes = 0;
    s->n_sysex = 0;
    atomic_store(&s->in_cycle, 0);
}

/** Run one block of `n` frames in place on `l` and `r`, with no transport. */
static inline void omx_clap_run(struct omx_clap_stage *s, float *l, float *r, uint32_t n)
{
    omx_clap_run_transport(s, l, r, n, NULL, NULL);
}

/**
 * RT, a cycle as a client with separate input and output buffers has it (JACK): the lane is the output pair, an
 * effect's input is copied onto it and an instrument's lane starts silent, then omx_clap_run. `inputs` is not read when
 * the stage has no audio input and may then be NULL.
 */
static inline void omx_clap_run_io(struct omx_clap_stage *s, const float *const *inputs, float *const *outputs, uint32_t n)
{
    uint32_t c;

    for (c = 0; c < s->h.n_out; c++)
    {
        if (s->h.n_in == 0)
        {
            memset(outputs[c], 0, n * sizeof(float));
        }
        else
        {
            const float *in = inputs[c < s->h.n_in ? c : s->h.n_in - 1];

            if (outputs[c] != in)
                memcpy(outputs[c], in, n * sizeof(float));
        }
    }
    omx_clap_run(s, outputs[0], s->h.n_out == 2 ? outputs[1] : NULL, n);
}

/**
 * Control thread, after activate and before publish, holding the audio role: start_processing, the warm-up blocks of `n`
 * frames through the same drain, process, flush-to-zero and expand path the RT takes, then reset() and stop_processing.
 * Returns the number of blocks whose output was unusable: anything but 0 means do not publish. The lane and the live
 * counters are untouched; the stage must not be published (state IDLE) while this runs.
 */
static inline int omx_clap_prime_block(void *ctx, uint32_t n)
{
    return omx_clap_run_plugin((struct omx_clap_stage *)ctx, n);
}

/** Prime an idle plugin with `n` frames of the warm-up signal, then reset and stop it: the number of bad blocks, UINT32_MAX when it cannot run. */
static inline uint32_t omx_clap_prime(struct omx_clap_stage *s, uint32_t n)
{
    uint32_t bad;

    if (s == NULL || s->plugin == NULL || n == 0 || n > s->h.max_block)
        return UINT32_MAX;
    if (atomic_load_explicit(&s->state, memory_order_acquire) != OMX_CLAP_IDLE)
        return UINT32_MAX;
    if (s->plugin->start_processing && !s->plugin->start_processing(s->plugin))
        return UINT32_MAX;
    bad = omx_hosted_prime(&s->h, n, omx_clap_prime_block, s);
    if (s->plugin->reset)
        s->plugin->reset(s->plugin);
    if (s->plugin->stop_processing)
        s->plugin->stop_processing(s->plugin);
    return bad;
}


/*
************************************************************************************************************************
*           END HEADER
************************************************************************************************************************
*/

#endif
