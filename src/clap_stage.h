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
*     for the next block. The note events of the block, written ahead of it by omx_clap_note_in, follow them. The
*     last OMX_CLAP_NOTES_RESERVED slots of the note list are kept for releases (a note off, all notes off, all sound
*     off, a pedal let go): a flood of note ons fills the rest and is counted, and the releases after it still arrive.
*     out_events.try_push is a counting sink: a CLAP_EVENT_PARAM_VALUE from the plugin raises plugin_changed and nothing
*     is stored. libomx-clap-core's instance puts lists of its own in the stage's two (clap_host.h): its sink also keeps
*     the plugin's values and gestures for the control thread, and its input list turns the host's modulations and
*     gestures, which ride the ring as tagged records, into their events.
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
#include "omx_clap_midi.h"


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

/** What a port's channels mean, from clap_audio_port_info.port_type: a type the core does not know, or none, is unknown. */
enum omx_clap_port_kind
{
    OMX_CLAP_PORT_UNKNOWN = 0,  ///< no port_type, or one the core does not know: discrete channels
    OMX_CLAP_PORT_MONO,         ///< CLAP_PORT_MONO
    OMX_CLAP_PORT_STEREO,       ///< CLAP_PORT_STEREO
    OMX_CLAP_PORT_SURROUND,     ///< CLAP_PORT_SURROUND: `map` names each channel
    OMX_CLAP_PORT_AMBISONIC     ///< CLAP_PORT_AMBISONIC: the ordering and the normalization name the channels
};

/** One audio port of a plugin as the core describes it, main or auxiliary, on either side. */
struct omx_clap_port
{
    clap_id id;                             ///< clap_audio_port_info.id
    char name[CLAP_NAME_SIZE];              ///< clap_audio_port_info.name, terminated
    uint32_t channels;                      ///< 1..CLAP_HOST_PORT_CHANNELS; 0 is no port
    uint32_t kind;                          ///< an omx_clap_port_kind
    uint8_t map[CLAP_HOST_PORT_CHANNELS];   ///< the CLAP surround ids, `channels` of them, for a surround port
    uint32_t ambisonic_ordering,            ///< clap_ambisonic_config_t.ordering, for an ambisonic port
             ambisonic_normalization;       ///< clap_ambisonic_config_t.normalization, for an ambisonic port
    clap_id in_place_pair;                  ///< clap_audio_port_info.in_place_pair: CLAP_INVALID_ID for none
    uint8_t ambisonic_assumed;              ///< 1 when the plugin did not say: no clap.ambisonic, or its get_config answered false, and the core read ACN/SN3D
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

/**
 * The slots at the end of a block's note list that only a release takes. A release is a message that ends sound: a note
 * off (or a note on with velocity 0), all sound off (controller 120), reset all controllers (121), all notes off and
 * the mode changes that imply it (123 to 127), and a sustain, sostenuto or hold pedal let go (64, 66, 69 below 64).
 * Every other message (a note on, an expression, a controller, a program change, a sysex) stops at
 * CLAP_HOST_NOTES_PER_BLOCK - OMX_CLAP_NOTES_RESERVED and is counted in notes_dropped; a release stops only when the
 * whole list is full and is counted in notes_dropped and releases_dropped. One list, written in arrival order, so the
 * plugin sees the survivors in the order and at the frames they came.
 */
#define OMX_CLAP_NOTES_RESERVED         (CLAP_HOST_NOTES_PER_BLOCK / 2u)

/** the pitch-bend range in semitones a stage starts with: MIDI's default, since RPN 0 (pitch bend sensitivity) is not tracked */
#define OMX_CLAP_BEND_SEMITONES_DEFAULT 2.0

/**
 * The note ids one stage holds at once. A CLAP-dialect note on takes a free id, its note off and its per-note
 * expressions carry it, and it is free again when the plugin reports CLAP_EVENT_NOTE_END for it, or, for a plugin that
 * never does, note_id_hold_frames after its note off. A note on with no free id is skipped and counted in
 * notes_skipped; its note off is then dropped, since nothing holds it.
 */
#define OMX_CLAP_NOTE_IDS               256u
/** how long after its note off an id waits for the plugin's NOTE_END before it may be taken again, in milliseconds */
#define OMX_CLAP_NOTE_ID_HOLD_MS        2000u
/** MPE's default pitch-bend range of a zone's member channels, in semitones (the MPE specification's default) */
#define OMX_CLAP_MPE_MEMBER_BEND_DEFAULT 48.0
/** MPE's default pitch-bend range of a zone's master channel, in semitones */
#define OMX_CLAP_MPE_MASTER_BEND_DEFAULT 2.0
/** key_id's mark of a key whose note on found no free id: its note off is dropped */
#define OMX_CLAP_NOTE_SKIPPED           (-2)
/** mpe_wanted's mark of a request the RT has not taken yet */
#define OMX_CLAP_MPE_WANTED             0x80000000u
/** The events of a plugin's note output one ring holds between two reads: a block's worth, the note events a block may hand
 * a plugin (CLAP_HOST_NOTES_PER_BLOCK); one past them is dropped and counted in the ring's `dropped`. */
#define OMX_CLAP_NOTE_OUT_DEPTH         CLAP_HOST_NOTES_PER_BLOCK

/** The MPE zone a CLAP-dialect input reads: none, the lower zone (master channel 1, members from 2 up) or the upper one
 * (master channel 16, members from 15 down). The MPE configuration message (RPN 6 on channel 1 or 16) sets it. */
enum omx_clap_mpe
{
    OMX_CLAP_MPE_OFF = 0,       ///< no zone: every channel is a plain MIDI channel
    OMX_CLAP_MPE_LOWER,         ///< master channel 1 (index 0), members 2 .. 1 + mpe_members
    OMX_CLAP_MPE_UPPER          ///< master channel 16 (index 15), members 15 .. 16 - mpe_members
};

/** One note id of the pool: free, held by a sounding key, or released and waiting for the plugin's NOTE_END. */
struct omx_clap_note_id
{
    uint8_t state;              ///< 0 free, 1 held (its key is down), 2 released (its note off sent)
    uint8_t channel;            ///< the channel of the note it names
    uint8_t key;                ///< the key of the note it names
    uint8_t pad;                ///< zero
    int64_t released_at;        ///< the frame its note off was sent at, in the stage's steady time
};

/** Every event the host hands a plugin ahead of a block besides the parameter writes */
union omx_clap_note
{
    clap_event_header_t header;     ///< the common header
    clap_event_note_t note;         ///< a CLAP note event
    clap_event_midi_t midi;         ///< a MIDI event
    clap_event_note_expression_t expr;  ///< a CLAP note expression: pitch bend and pressure in the CLAP dialect
    clap_event_midi_sysex_t sysex;  ///< a sysex message, its bytes in the stage's pool
    clap_event_midi2_t midi2;       ///< a MIDI 2.0 channel voice message, translated from MIDI 1.0
};

/** The RT stage of one hosted CLAP instance: the hosted core, the bound plugin, the block's events and the counters. */
struct omx_clap_stage
{
    // the hosted core, the head of the stage: the bounce, the legs, bypass, guards, fault and the counters
    struct omx_hosted_stage h;                          ///< the hosted core, at offset 0

    // bound by the control thread before publish (omx_clap_stage_init, omx_clap_bind)
    const clap_plugin_t *plugin;                        ///< the hosted plugin
    float *in_ptrs[CLAP_HOST_PORT_CHANNELS],            ///< what the main input buffer's data32 points at: the legs, then the rest
          *out_ptrs[CLAP_HOST_PORT_CHANNELS];           ///< what the main output buffer's data32 points at: the legs, then the rest
    clap_audio_buffer_t ain;                            ///< distinct from the outputs, over the bounce
    clap_audio_buffer_t aout[1 + CLAP_HOST_AUX_OUTPUTS];///< the main output, then one buffer per auxiliary output
    uint32_t n_aux;                                     ///< auxiliary output ports, after aout[0]
    float *aux_ptrs[CLAP_HOST_AUX_OUTPUTS * CLAP_HOST_PORT_CHANNELS];  ///< what each auxiliary output channel writes: the caller's
                                                        ///< buffer or its scratch block, port i's from aux_base[i]
    uint32_t aux_base[CLAP_HOST_AUX_OUTPUTS];           ///< where each auxiliary output's channels start in aux_ptrs
    float *aux_scratch[CLAP_HOST_AUX_OUTPUTS * CLAP_HOST_PORT_CHANNELS];  ///< each auxiliary output channel's own scratch block
    clap_event_param_value_t events[CLAP_HOST_EVENTS_PER_BLOCK];   ///< this block's parameter events
    uint32_t n_events;                                  ///< the events in `events`
    union omx_clap_note notes[CLAP_HOST_NOTES_PER_BLOCK];  ///< this block's notes, written ahead of it by the RT thread
    uint32_t n_notes;                                   ///< the notes in `notes`
    uint8_t sysex_bytes[OMX_CLAP_SYSEX_PER_BLOCK][OMX_CLAP_SYSEX_MAX_BYTES];  ///< the bytes this block's sysex slots point at
    uint32_t n_sysex;                                   ///< the sysex messages in `sysex_bytes`
    _Atomic uint32_t notes_visible;                     ///< 1 only while the plugin is inside process()
    uint32_t note_inputs;                               ///< 0 or 1 note input
    uint32_t note_dialect;                              ///< one CLAP_NOTE_DIALECT_*: how notes travel
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
    _Atomic uint32_t notes_dropped;                     ///< note events the block had no room for, releases included (OMX_CLAP_NOTES_RESERVED)
    _Atomic uint32_t sysex_dropped;                     ///< a sysex message past OMX_CLAP_SYSEX_PER_BLOCK
    _Atomic uint32_t notes_unmappable;                  ///< a channel message with no mapping into the plugin's dialect: counted, never silently dropped
    _Atomic uint32_t out_events_seen;                   ///< events the plugin pushed at the counting sink
    _Atomic uint32_t plugin_changed;                    ///< a CLAP_EVENT_PARAM_VALUE came back: read-back due
    _Atomic uint32_t resets;                            ///< reset() calls on re-engage, made by the control thread
    _Atomic uint32_t start_refused;                     ///< start_processing() answered false
    // ABI 5: in the stage's tail padding, so its size and every offset before it are unchanged
    _Atomic uint32_t releases_dropped;                  ///< of notes_dropped, the releases: past the whole list, CLAP_HOST_NOTES_PER_BLOCK

    // ABI 13: the wide bounce, bound by the control thread before publish (omx_clap_bind_wide) when a main port is wider
    // than the two legs; omx_clap_run_ports then carries every main channel through it
    float *wide_in[CLAP_HOST_PORT_CHANNELS],            ///< one block per main input channel: the caller's input is copied here
          *wide_out[CLAP_HOST_PORT_CHANNELS];           ///< one block per main output channel: the plugin writes here
    uint32_t wide;                                      ///< 1 while the wide bounce is bound: the main buffers are wide_in and wide_out
    // ABI 14: the auxiliary output channels bound to the caller's buffers (omx_clap_set_aux_out); 0 spares a block every
    // per-channel test of the auxiliary outputs' guard and silence
    uint32_t aux_bound_channels;
};

/**
 * The note ids, the MPE zone and the note ends of one stage (ABI 6). They live beside the stage, at the end of the
 * instance, which binds them to its stage at activate (omx_clap_note_ids_bind). A stage fed through omx_clap_note_in has
 * none, and its notes go with note id -1.
 */
struct omx_clap_note_ids
{
    struct omx_clap_stage *stage;                       ///< the stage whose notes carry the ids
    clap_output_events_t out_events;                    ///< the stage's sink that also reads NOTE_END; ctx is this structure
    uint32_t hold_frames;                               ///< frames after a note off an id waits for NOTE_END (the bind sets it)
    _Atomic uint32_t mpe_wanted;                        ///< ABI 6 to 8 consumers' single zone; unused from ABI 9, see struct omx_clap_mpe_zones
    uint32_t mpe;                                       ///< ABI 6 to 8: unused from ABI 9
    uint32_t mpe_members;                               ///< ABI 6 to 8: unused from ABI 9
    double mpe_master_bend;                             ///< ABI 6 to 8: unused from ABI 9
    double mpe_member_bend;                             ///< ABI 6 to 8: unused from ABI 9
    uint8_t rpn[16][2];                                 ///< the RPN each channel selected (controllers 101, 100); 127 none
    int32_t chan_id[16];                                ///< the id of the newest note held on each channel, -1 none: an MPE member's note
    int32_t key_id[16][128];                            ///< the id each held key sounds under; -1 none, OMX_CLAP_NOTE_SKIPPED its note on was skipped
    struct omx_clap_note_id ids[OMX_CLAP_NOTE_IDS];     ///< the pool
    uint16_t free_ids[OMX_CLAP_NOTE_IDS];               ///< the free ids, a stack
    uint32_t n_free;                                    ///< the ids on the stack: OMX_CLAP_NOTE_IDS - n_free notes sound or end
    _Atomic uint32_t notes_skipped;                     ///< note ons with no free id: skipped, and their note offs dropped
    _Atomic uint32_t notes_ended;                       ///< ids the plugin's NOTE_END freed
    _Atomic uint32_t ids_expired;                       ///< ids taken back after their hold with no NOTE_END
};

/** The two MPE zones: index 0 the lower (master channel 1, members from 2 up), 1 the upper (master channel 16, members from 15 down). */
#define OMX_CLAP_MPE_ZONES              2u
/** master_have: the zone's master has sent a pitch bend since the zone was set */
#define OMX_CLAP_MPE_MASTER_BEND_BIT    1u
/** master_have: the zone's master has sent a channel pressure since the zone was set */
#define OMX_CLAP_MPE_MASTER_PRESSURE_BIT 2u
/** master_have: the zone's master has sent a controller 74 since the zone was set */
#define OMX_CLAP_MPE_MASTER_BRIGHTNESS_BIT 4u

/** One MPE zone: its member channels (0: the zone does not exist), its bend ranges and the master's last values. */
struct omx_clap_mpe_zone
{
    uint32_t members;                                   ///< the zone's member channels, 0 none (no zone) or 1 to 15
    uint32_t master_have;                                ///< OMX_CLAP_MPE_MASTER_*_BIT of the values below the master has sent since the zone was set
    double master_bend;                                 ///< the master channel's bend range in semitones (RPN 0 on the master)
    double member_bend;                                 ///< the member channels' bend range in semitones (RPN 0 on a member)
    double master_tuning;                               ///< the master's last bend, in semitones
    double master_pressure;                             ///< the master's last channel pressure, 0 to 1
    double master_brightness;                           ///< the master's last controller 74, 0 to 1
};

/**
 * The MPE zones of one input (ABI 9): a lower and an upper zone held together. struct omx_clap_note_ids grew past its
 * place in the instance for ABI 6 and 7, so the zones live in their own structure, appended to the instance. Zeroed
 * with its owner; a zone takes its bend defaults when omx_clap_mpe_take sets it.
 */
struct omx_clap_mpe_zones
{
    _Atomic uint32_t wanted[OMX_CLAP_MPE_ZONES];        ///< control thread -> RT: a zone to take, OMX_CLAP_MPE_WANTED | members << 8
    struct omx_clap_mpe_zone zone[OMX_CLAP_MPE_ZONES];  ///< the lower zone, then the upper
};

/** One event of a plugin's note output, as its MIDI message: what omx_clap_note_out_take hands the reader. */
struct omx_clap_note_out_event
{
    int64_t frame;                                      ///< its frame since activate, in the stage's steady time: the block's first frame plus `time`
    uint32_t time;                                      ///< the frame in its block the plugin gave it, not clamped to the block
    uint8_t size;                                       ///< the bytes of the message, 1 to 3
    uint8_t data[3];                                    ///< the MIDI message
};

/**
 * A plugin's note output (ABI 7): a lock-free single-producer, single-consumer ring of MIDI messages. The plugin's
 * output event list fills it inside process() on the audio role (omx_clap_note_out_push); one reader empties it
 * (omx_clap_note_out_take), the audio role right after the block in the same cycle, or one other thread. Nothing in it
 * allocates, locks or calls out. Zeroed with its owner; never reset while either side may run.
 */
struct omx_clap_note_out
{
    _Atomic uint32_t head;                              ///< consumer: the next event to take
    _Atomic uint32_t tail;                              ///< producer: the next slot to fill
    _Atomic uint32_t dropped;                           ///< events the ring had no room for
    _Atomic uint32_t unmapped;                          ///< events with no MIDI form here: a note expression, a sysex, a MIDI 2
                                                        ///< event, another port's, a note with no single key or channel
    struct omx_clap_note_out_event ring[OMX_CLAP_NOTE_OUT_DEPTH];  ///< the events
};

_Static_assert((OMX_CLAP_NOTE_OUT_DEPTH & (OMX_CLAP_NOTE_OUT_DEPTH - 1u)) == 0, "the note output ring is a power of two");

/**
 * The audio inputs of a stage with auxiliary inputs: the process list, the main input first when the plugin has one,
 * then the auxiliary inputs in port order; a source's list starts at the first auxiliary input. The instance holds it
 * beside its stage (aux_list), and omx_clap_bind_ports_aux points the stage's process structure at it.
 */
struct omx_clap_aux_in
{
    clap_audio_buffer_t ains[1 + CLAP_HOST_AUX_INPUTS]; ///< the main input (when there is one), then one per auxiliary input
    uint32_t n_aux_in;                                  ///< auxiliary input ports, after the main one
    float *aux_in_ptrs[CLAP_HOST_AUX_INPUTS * CLAP_HOST_PORT_CHANNELS];  ///< what each auxiliary input channel reads: the caller's
                                                        ///< buffer or the silence, port i's from aux_in_base[i]
    uint32_t aux_in_base[CLAP_HOST_AUX_INPUTS];         ///< where each auxiliary input's channels start in aux_in_ptrs
    float *aux_silence[2];                              ///< the zeroed block every unbound auxiliary input channel reads, twice
};

_Static_assert(offsetof(struct omx_clap_stage, h) == 0, "the hosted core heads the CLAP stage");
_Static_assert(sizeof(struct omx_clap_stage) <= 1u << 17, "stage within 128 KiB");


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
*           NOTE IDS
************************************************************************************************************************
*/

/** Control thread, holding the audio role (or before the first block): every id free and no key held; the MPE zones
 * (struct omx_clap_mpe_zones, kept beside the ids) are not touched. omx_clap_note_ids_bind calls it; the host calls it again when it resets the plugin,
 * whose voices are then gone. */
static inline void omx_clap_note_ids_reset(struct omx_clap_note_ids *n)
{
    uint32_t i, k;

    for (i = 0; i < 16; i++)
    {
        n->rpn[i][0] = n->rpn[i][1] = 127;
        n->chan_id[i] = -1;
        for (k = 0; k < 128; k++)
            n->key_id[i][k] = -1;
    }
    memset(n->ids, 0, sizeof(n->ids));
    for (i = 0; i < OMX_CLAP_NOTE_IDS; i++)
        n->free_ids[i] = (uint16_t)(OMX_CLAP_NOTE_IDS - 1u - i);    // popped from the top: id 0 first
    n->n_free = OMX_CLAP_NOTE_IDS;
}

/** RT: forget id `id` wherever a key or a channel still names it. */
static inline void omx_clap_note_id_unmap(struct omx_clap_note_ids *n, int32_t id)
{
    const struct omx_clap_note_id *e = &n->ids[id];

    if (n->key_id[e->channel][e->key] == id)
        n->key_id[e->channel][e->key] = -1;
    if (n->chan_id[e->channel] == id)
        n->chan_id[e->channel] = -1;
}

/** RT: id `id` is free again: its voice ended, or its hold ran out. */
static inline void omx_clap_note_id_free(struct omx_clap_note_ids *n, int32_t id)
{
    if (n->ids[id].state == 0)
        return;
    omx_clap_note_id_unmap(n, id);
    n->ids[id].state = 0;
    n->free_ids[n->n_free++] = (uint16_t)id;
}

/** RT: a free id for a note on channel `channel`, key `key`, or -1 when none is free. Only when the stack is empty, a
 * released id whose hold has run out by `now` is taken back (counted in ids_expired). Bounded: one pass over the pool. */
static inline int32_t omx_clap_note_id_take(struct omx_clap_note_ids *n, uint8_t channel, uint8_t key, int64_t now)
{
    int32_t id;

    if (n->n_free == 0)
        for (uint32_t i = 0; i < OMX_CLAP_NOTE_IDS; i++)
            if (n->ids[i].state == 2 && now - n->ids[i].released_at >= (int64_t)n->hold_frames)
            {
                omx_clap_note_id_free(n, (int32_t)i);
                omx_hosted_count(&n->ids_expired, 1);
            }
    if (n->n_free == 0)
        return -1;
    id = n->free_ids[--n->n_free];
    n->ids[id].state = 1;
    n->ids[id].channel = channel;
    n->ids[id].key = key;
    return id;
}

/** RT: id `id`'s note off was sent at `now`: it no longer names its key, and waits for NOTE_END or its hold. */
static inline void omx_clap_note_id_release(struct omx_clap_note_ids *n, int32_t id, int64_t now)
{
    if (n->ids[id].state != 1)
        return;
    omx_clap_note_id_unmap(n, id);
    n->ids[id].state = 2;
    n->ids[id].released_at = now;
}

/** RT: whether `id`, in use, names the note of channel `channel` and key `key` (-1 in either: any) */
static inline int omx_clap_note_id_names(const struct omx_clap_note_id *id, int16_t channel, int16_t key)
{
    return id->state != 0 && (channel < 0 || id->channel == channel) && (key < 0 || id->key == key);
}

/** RT: whether a note the ids name may still sound at steady time `now`: one held, or released less than hold_frames
 * ago with no NOTE_END yet. A stage with no ids bound (hold_frames 0) has none. */
static inline int omx_clap_note_ids_live(const struct omx_clap_note_ids *n, int64_t now)
{
    if (!n || n->n_free == OMX_CLAP_NOTE_IDS || n->hold_frames == 0)
        return 0;
    for (uint32_t i = 0; i < OMX_CLAP_NOTE_IDS; i++)
        if (n->ids[i].state == 1 || (n->ids[i].state == 2 && now - n->ids[i].released_at < (int64_t)n->hold_frames))
            return 1;
    return 0;
}

/** RT, inside process(): the plugin's CLAP_EVENT_NOTE_END. Its id is freed when the id still names the note the event
 * names (its channel and key, -1 in either matching any): an id whose hold ran out may already name a new note, and a
 * late NOTE_END for the old voice must not take the id from the one that sounds. With no id (-1), every id of its
 * channel and key is freed (-1 in either: every channel or every key). An id the host never gave is ignored. */
static inline void omx_clap_note_end(struct omx_clap_note_ids *n, const clap_event_note_t *e)
{
    if (e->note_id >= 0)
    {
        if ((uint32_t)e->note_id < OMX_CLAP_NOTE_IDS && omx_clap_note_id_names(&n->ids[e->note_id], e->channel, e->key))
        {
            omx_clap_note_id_free(n, e->note_id);
            omx_hosted_count(&n->notes_ended, 1);
        }
        return;
    }
    for (uint32_t i = 0; i < OMX_CLAP_NOTE_IDS; i++)
    {
        if (!omx_clap_note_id_names(&n->ids[i], e->channel, e->key))
            continue;
        omx_clap_note_id_free(n, (int32_t)i);
        omx_hosted_count(&n->notes_ended, 1);
    }
}

/** The output event list of a stage with note ids: every event goes on to the stage's own sink (s->out_events, which
 * counts it, raises plugin_changed on a value, and in libomx-clap-core's instance also keeps the plugin's values and
 * gestures), and a CLAP_EVENT_NOTE_END frees the id it names. Nothing else is stored on the RT. */
static inline bool omx_clap_out_push_ids(const struct clap_output_events *list, const clap_event_header_t *event)
{
    struct omx_clap_note_ids *n = (struct omx_clap_note_ids *)list->ctx;
    const clap_output_events_t *sink = &n->stage->out_events;

    sink->try_push(sink, event);
    if (event != NULL && event->space_id == CLAP_CORE_EVENT_SPACE_ID && event->type == CLAP_EVENT_NOTE_END &&
        event->size >= sizeof(clap_event_note_t))
        omx_clap_note_end(n, (const clap_event_note_t *)event);
    return true;    ///< accepted and counted
}

/**
 * Control thread, after omx_clap_bind_ports (which points process() at the stage's own sink) and before the stage is
 * armed: give stage `s` the note ids `n` (zeroed before the first bind), every id free, the hold `hold_frames` long (OMX_CLAP_NOTE_ID_HOLD_MS at the
 * activated rate). process() then hands the plugin n's sink, which reads NOTE_END and passes every event on to the
 * stage's sink, so a sink put in s->out_events before the bind still sees them all; feed the stage through
 * omx_clap_note_in_ids with `n`.
 */
static inline void omx_clap_note_ids_bind(struct omx_clap_note_ids *n, struct omx_clap_stage *s, uint32_t hold_frames)
{
    omx_clap_note_ids_reset(n);
    n->stage = s;
    n->hold_frames = hold_frames;
    n->out_events.ctx = n;
    n->out_events.try_push = omx_clap_out_push_ids;
    s->proc.out_events = &n->out_events;
}

/** Any thread: ask for MPE zone `zone` (enum omx_clap_mpe) with `members` member channels (1 to 15), as an MPE
 * configuration message would set it; the RT takes it ahead of the next message it reads. The other zone is not asked
 * about, but yields channels to this one as the configuration message makes it (omx_clap_mpe_take). `members` 0 ends
 * the zone; OMX_CLAP_MPE_OFF ends both. */
static inline void omx_clap_mpe_zones_want(struct omx_clap_mpe_zones *z, uint32_t zone, uint32_t members)
{
    if (zone == OMX_CLAP_MPE_OFF || zone > OMX_CLAP_MPE_UPPER)
    {
        atomic_store_explicit(&z->wanted[0], OMX_CLAP_MPE_WANTED, memory_order_release);
        atomic_store_explicit(&z->wanted[1], OMX_CLAP_MPE_WANTED, memory_order_release);
        return;
    }
    if (members > 15)
        members = 15;
    atomic_store_explicit(&z->wanted[zone - 1u], OMX_CLAP_MPE_WANTED | members << 8, memory_order_release);
}


/*
************************************************************************************************************************
*           NOTE OUTPUT
************************************************************************************************************************
*/

/**
 * RT, inside process(): one event the plugin pushed at its output list, kept in `o` when it is note output. A note on,
 * note off or choke of note port 0 becomes its MIDI message (omx_clap_note_midi), a CLAP_EVENT_MIDI of port 0 is kept
 * at its length (omx_clap_midi_size); a note expression, a sysex, a MIDI 2 event and a note or MIDI event that has no
 * such form are counted in `unmapped`. Any other event (a NOTE_END, which tells the host a voice of its input ended, a
 * parameter's, the transport) is no note output and is left alone. `block_start` is the block's first frame in the
 * stage's steady time. A full ring drops the event and counts it: the plugin is never refused or made to wait.
 */
static inline void omx_clap_note_out_push(struct omx_clap_note_out *o, const clap_event_header_t *h, int64_t block_start)
{
    struct omx_clap_note_out_event *slot;
    uint8_t m[3] = { 0, 0, 0 };
    uint32_t size = 0, head, tail;

    if (h == NULL || h->space_id != CLAP_CORE_EVENT_SPACE_ID)
        return;
    switch (h->type)
    {
        case CLAP_EVENT_NOTE_ON:
        case CLAP_EVENT_NOTE_OFF:
        case CLAP_EVENT_NOTE_CHOKE:
            size = omx_clap_note_midi(h, m);
            break;
        case CLAP_EVENT_MIDI:
            if (h->size >= sizeof(clap_event_midi_t) && ((const clap_event_midi_t *)h)->port_index == 0)
            {
                memcpy(m, ((const clap_event_midi_t *)h)->data, sizeof(m));
                size = omx_clap_midi_size(m[0]);
            }
            break;
        case CLAP_EVENT_NOTE_EXPRESSION:
        case CLAP_EVENT_MIDI_SYSEX:
        case CLAP_EVENT_MIDI2:
            break;
        default:
            return;
    }
    if (size == 0)
    {
        omx_hosted_count(&o->unmapped, 1);
        return;
    }
    tail = atomic_load_explicit(&o->tail, memory_order_relaxed);
    head = atomic_load_explicit(&o->head, memory_order_acquire);
    if (tail - head >= OMX_CLAP_NOTE_OUT_DEPTH)
    {
        omx_hosted_count(&o->dropped, 1);
        return;
    }
    slot = &o->ring[tail & (OMX_CLAP_NOTE_OUT_DEPTH - 1u)];
    slot->frame = block_start + (int64_t)h->time;
    slot->time = h->time;
    slot->size = (uint8_t)size;
    slot->data[0] = m[0];
    slot->data[1] = m[1];
    slot->data[2] = m[2];
    atomic_store_explicit(&o->tail, tail + 1u, memory_order_release);
}

/** The ring's one reader: take up to `capacity` events of `o` into `events`, oldest first, and free their slots. Returns
 * how many; 0 when it is empty. No allocation, no lock, no call out: safe on the audio role. */
static inline uint32_t omx_clap_note_out_take(struct omx_clap_note_out *o, struct omx_clap_note_out_event *events, uint32_t capacity)
{
    uint32_t head = atomic_load_explicit(&o->head, memory_order_relaxed);
    const uint32_t tail = atomic_load_explicit(&o->tail, memory_order_acquire);
    uint32_t n = 0;

    while (head != tail && n < capacity)
        events[n++] = o->ring[head++ & (OMX_CLAP_NOTE_OUT_DEPTH - 1u)];
    atomic_store_explicit(&o->head, head, memory_order_release);
    return n;
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
 * Control thread, after init: the plugin's auxiliary output ports `ports` (up to CLAP_HOST_AUX_OUTPUTS of them, each
 * 1..CLAP_HOST_PORT_CHANNELS channels), `scratch[k]` one block for each of their channels in port order: the plugin
 * writes its scratch and nothing reads it until the caller hands a port its own buffers (omx_clap_set_aux_out). Returns
 * -1 on a width or a count the stage cannot hold.
 */
static inline int omx_clap_bind_aux(struct omx_clap_stage *s, float *const *scratch, uint32_t count,
                                    const struct omx_clap_port *ports)
{
    uint32_t i, c, k = 0;

    if (count > CLAP_HOST_AUX_OUTPUTS || (count && (scratch == NULL || ports == NULL)))
        return -1;
    for (i = 0; i < count; i++)
    {
        if (ports[i].channels == 0 || ports[i].channels > CLAP_HOST_PORT_CHANNELS
            || k + ports[i].channels > CLAP_HOST_AUX_OUTPUTS * CLAP_HOST_PORT_CHANNELS)
            return -1;
        s->aux_base[i] = k;
        for (c = 0; c < ports[i].channels; c++, k++)
        {
            if (scratch[k] == NULL)
                return -1;
            s->aux_scratch[k] = scratch[k];
            s->aux_ptrs[k] = scratch[k];
        }
        s->aout[1 + i].data32 = &s->aux_ptrs[s->aux_base[i]];
        s->aout[1 + i].data64 = NULL;
        s->aout[1 + i].channel_count = ports[i].channels;
        s->aout[1 + i].latency = 0;
        s->aout[1 + i].constant_mask = 0;
    }
    s->n_aux = count;
    s->aux_bound_channels = 0;
    return 0;
}

/**
 * Control thread, with no cycle running: point auxiliary output `port`'s `channels` channels at the caller's buffers
 * `bufs`, or back at its scratch when `bufs` is NULL. `port` and `channels` are the bound port's.
 */
static inline void omx_clap_set_aux_out(struct omx_clap_stage *s, uint32_t port, uint32_t channels, float *const *bufs)
{
    const uint32_t base = s->aux_base[port];
    uint32_t c;

    for (c = 0; c < channels; c++)
    {
        const int was = s->aux_ptrs[base + c] != s->aux_scratch[base + c];

        s->aux_ptrs[base + c] = bufs && bufs[c] ? bufs[c] : s->aux_scratch[base + c];
        s->aux_bound_channels += (uint32_t)(s->aux_ptrs[base + c] != s->aux_scratch[base + c]) - (uint32_t)was;
    }
}

/**
 * Control thread, with no cycle running: point auxiliary input `port`'s `channels` channels at the caller's buffers
 * `bufs`, or back at the silence when `bufs` is NULL. `port` and `channels` are the bound port's.
 */
static inline void omx_clap_set_aux_in(struct omx_clap_aux_in *a, uint32_t port, uint32_t channels, float *const *bufs)
{
    const uint32_t base = a->aux_in_base[port];
    uint32_t c;

    for (c = 0; c < channels; c++)
        a->aux_in_ptrs[base + c] = bufs && bufs[c] ? bufs[c] : a->aux_silence[0];
}

/**
 * Control thread, after init and before the first block: the plugin's auxiliary input ports `ports` (up to
 * CLAP_HOST_AUX_INPUTS of them, each 1..CLAP_HOST_PORT_CHANNELS channels). `silence` is a zeroed block every channel
 * reads until the caller binds its own; `bound` holds the caller's buffers, port i's channels from `base[i]` (NULL: none
 * bound, or a channel NULL: that one unbound), and `base` is NULL exactly when `bound` is. Returns -1 on a width or a
 * count the stage cannot hold.
 */
static inline int omx_clap_bind_aux_in(struct omx_clap_aux_in *a, float *silence, uint32_t count,
                                       const struct omx_clap_port *ports, float *const *bound, const uint32_t *base)
{
    uint32_t i, k = 0;

    if (count > CLAP_HOST_AUX_INPUTS || (count && ports == NULL) || silence == NULL || (bound == NULL) != (base == NULL))
        return -1;
    a->aux_silence[0] = silence;
    a->aux_silence[1] = silence;
    for (i = 0; i < count; i++)
    {
        if (ports[i].channels == 0 || ports[i].channels > CLAP_HOST_PORT_CHANNELS
            || k + ports[i].channels > CLAP_HOST_AUX_INPUTS * CLAP_HOST_PORT_CHANNELS)
            return -1;
        a->aux_in_base[i] = k;
        omx_clap_set_aux_in(a, i, ports[i].channels, bound ? &bound[base[i]] : NULL);
        a->ains[1 + i].data32 = &a->aux_in_ptrs[k];
        a->ains[1 + i].data64 = NULL;
        a->ains[1 + i].channel_count = ports[i].channels;
        a->ains[1 + i].latency = 0;
        a->ains[1 + i].constant_mask = 0;
        k += ports[i].channels;
    }
    a->n_aux_in = count;
    return 0;
}

/**
 * Control thread, after init and before omx_clap_bind_ports_aux: bind the wide bounce, `ins[c]` one block of max_block
 * frames for each of the `n_in` main input channels (0 for a source) and `outs[c]` one for each of the `n_out` main
 * output channels, every block distinct from the caller's buffers. The main buffers are then these blocks and
 * omx_clap_run_ports carries every channel through them. Returns -1, binding nothing, on a width past
 * CLAP_HOST_PORT_CHANNELS, no output or a channel with no block.
 */
static inline int omx_clap_bind_wide(struct omx_clap_stage *s, float *const *ins, uint32_t n_in, float *const *outs, uint32_t n_out)
{
    uint32_t c;

    if (s == NULL || n_in > CLAP_HOST_PORT_CHANNELS || n_out < 1 || n_out > CLAP_HOST_PORT_CHANNELS || (n_in && ins == NULL) || outs == NULL)
        return -1;
    for (c = 0; c < n_in; c++)
        if (ins[c] == NULL)
            return -1;
    for (c = 0; c < n_out; c++)
        if (outs[c] == NULL)
            return -1;
    for (c = 0; c < n_in; c++)
        s->wide_in[c] = ins[c];
    for (c = 0; c < n_out; c++)
        s->wide_out[c] = outs[c];
    s->wide = 1;
    return 0;
}

/**
 * Control thread, after init and activate: bind the plugin and its main port widths, `n_in` 0 (a source) up to
 * CLAP_HOST_PORT_CHANNELS and `n_out` 1 up to CLAP_HOST_PORT_CHANNELS, and its auxiliary inputs `aux` as
 * omx_clap_bind_aux_in bound them (NULL: none). The first two channels of each side are the hosted core's legs, which
 * carry at most two (h.n_in, h.n_out); a channel past them is the caller's to point in_ptrs and out_ptrs at before
 * this call. A stage with the wide bounce bound (omx_clap_bind_wide) has its main buffers over wide_in and wide_out
 * instead, every channel of them bound. The audio buffers point at the bounce once, the addresses never move, and every
 * fixed field of clap_process_t is set here, so the RT writes only frames_count and steady_time.
 */
static inline int omx_clap_bind_ports_aux(struct omx_clap_stage *s, const clap_plugin_t *plugin, uint32_t n_in, uint32_t n_out,
                                          struct omx_clap_aux_in *aux)
{
    clap_audio_buffer_t *main_in;
    float **ins, **outs;

    if (s == NULL || plugin == NULL || plugin->process == NULL)
        return -1;
    if (n_in > CLAP_HOST_PORT_CHANNELS || n_out < 1 || n_out > CLAP_HOST_PORT_CHANNELS)
        return -1;
    ins = s->wide ? s->wide_in : s->in_ptrs;
    outs = s->wide ? s->wide_out : s->out_ptrs;
    for (uint32_t c = s->wide ? 0 : 2; c < n_in; c++)
        if (ins[c] == NULL)
            return -1;
    for (uint32_t c = s->wide ? 0 : 2; c < n_out; c++)
        if (outs[c] == NULL)
            return -1;
    if (aux && aux->n_aux_in == 0)
        aux = NULL;
    main_in = aux ? &aux->ains[0] : &s->ain;
    s->plugin = plugin;
    s->h.n_in = n_in < 2 ? n_in : 2;
    s->h.n_out = n_out < 2 ? n_out : 2;
    main_in->data32 = ins;
    main_in->data64 = NULL;
    main_in->channel_count = n_in;
    main_in->latency = 0;
    main_in->constant_mask = 0;
    s->aout[0].data32 = outs;
    s->aout[0].data64 = NULL;
    s->aout[0].channel_count = n_out;
    s->aout[0].latency = 0;
    s->aout[0].constant_mask = 0;
    memset(&s->proc, 0, sizeof(s->proc));
    s->proc.transport = NULL;
    s->proc.audio_inputs = n_in ? main_in : aux ? &aux->ains[1] : NULL;
    s->proc.audio_outputs = s->aout;
    s->proc.audio_inputs_count = (n_in ? 1 : 0) + (aux ? aux->n_aux_in : 0);
    s->proc.audio_outputs_count = 1 + s->n_aux;
    s->proc.in_events = &s->in_events;
    s->proc.out_events = &s->out_events;
    s->steady_time = 0;
    atomic_store_explicit(&s->h.rendered_wet, 0, memory_order_relaxed);
    s->need_reset = 0;
    return 0;
}

/** The same with no auxiliary input. */
static inline int omx_clap_bind_ports(struct omx_clap_stage *s, const clap_plugin_t *plugin, uint32_t n_in, uint32_t n_out)
{
    return omx_clap_bind_ports_aux(s, plugin, n_in, n_out, NULL);
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

/** Whether a channel message of one to three bytes is a release (OMX_CLAP_NOTES_RESERVED says which). */
static inline int omx_clap_midi_is_release(const uint8_t *data, size_t size)
{
    const uint8_t type = data[0] & 0xf0;

    if (size != 3)
        return 0;
    if (type == 0x80 || (type == 0x90 && data[2] == 0))
        return 1;
    if (type != 0xb0)
        return 0;
    if (data[1] >= 120 && data[1] != 122)       // 122, local control, is the only channel mode message that ends no sound
        return 1;
    return (data[1] == 64 || data[1] == 66 || data[1] == 69) && data[2] < 64;
}

/**
 * The next note slot of this block, filled with the header. `release` is 1 for a release: it may take the last
 * OMX_CLAP_NOTES_RESERVED slots, every other event stops before them. NULL past its room, counted in notes_dropped (and a
 * release in releases_dropped too). Bounded and in place: no allocation, no lock, no call out.
 */
static inline union omx_clap_note *omx_clap_note_slot_for(struct omx_clap_stage *s, uint32_t time, uint16_t type, uint32_t size,
                                                          int release)
{
    const uint32_t room = release ? CLAP_HOST_NOTES_PER_BLOCK : CLAP_HOST_NOTES_PER_BLOCK - OMX_CLAP_NOTES_RESERVED;
    union omx_clap_note *slot;

    if (s->n_notes >= room)
    {
        omx_hosted_count(&s->notes_dropped, 1);
        if (release)
            omx_hosted_count(&s->releases_dropped, 1);
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

/** The next note slot of this block for a CLAP event `type`: a note off or a choke is a release, anything else is not. */
static inline union omx_clap_note *omx_clap_note_slot(struct omx_clap_stage *s, uint32_t time, uint16_t type, uint32_t size)
{
    return omx_clap_note_slot_for(s, time, type, size, type == CLAP_EVENT_NOTE_OFF || type == CLAP_EVENT_NOTE_CHOKE);
}

/** RT: fill a CLAP_EVENT_MIDI slot with one raw channel message, one to three bytes. */
static inline void omx_clap_midi_slot(struct omx_clap_stage *s, uint32_t time, const uint8_t *data, size_t size)
{
    union omx_clap_note *slot = omx_clap_note_slot_for(s, time, CLAP_EVENT_MIDI, sizeof(clap_event_midi_t),
                                                       omx_clap_midi_is_release(data, size));

    if (!slot)
        return;
    slot->midi.port_index = 0;
    slot->midi.data[0] = data[0];
    slot->midi.data[1] = size > 1 ? data[1] : 0;
    slot->midi.data[2] = size > 2 ? data[2] : 0;
}

/**
 * `value`, `src_bits` wide, scaled up to `dst_bits` (at most 32) by the MIDI 2.0 rule (min-centre-max scaling, the UMP
 * specification's default translation): 0 stays 0, the centre of the source range lands on the centre of the
 * destination, and the source's maximum on the destination's maximum, the bits below the centre repeated to fill.
 */
static inline uint32_t omx_clap_midi2_scale_up(uint32_t value, uint32_t src_bits, uint32_t dst_bits)
{
    const uint32_t scale_bits = dst_bits - src_bits;
    const uint32_t repeat_bits = src_bits - 1u;
    uint64_t shifted = (uint64_t)value << scale_bits;
    uint64_t repeat;

    if (value <= (1u << repeat_bits))
        return (uint32_t)shifted;
    repeat = value & ((1u << repeat_bits) - 1u);
    repeat = scale_bits > repeat_bits ? repeat << (scale_bits - repeat_bits) : repeat >> (repeat_bits - scale_bits);
    while (repeat)
    {
        shifted |= repeat;
        repeat >>= repeat_bits;
    }
    return (uint32_t)shifted;
}

/** The MIDI 2.0 note off a MIDI 1.0 note on of velocity 0 stands for: MIDI 1.0's release velocity, 64, scaled up. */
#define OMX_CLAP_MIDI2_RELEASE_VELOCITY 0x8000u

/**
 * One MIDI 1.0 channel message of one to three bytes as the MIDI 2.0 channel voice message (Universal MIDI Packet,
 * message type 4, group 0) the UMP specification translates it to, in `words`: a note on or off with its velocity
 * scaled from 7 to 16 bits (a note on of velocity 0 is a note off of velocity OMX_CLAP_MIDI2_RELEASE_VELOCITY) and no
 * attribute, poly and channel pressure and a controller from 7 to 32 bits, pitch bend from 14 to 32 bits, and a program
 * change with no bank. No state is kept: bank select, RPN and NRPN controllers pass as the plain controllers they are.
 * Returns 1, or 0 for a message that is not a channel voice message of its full length (`words` is then untouched).
 */
static inline int omx_clap_midi2_from_midi1(const uint8_t *data, size_t size, uint32_t words[4])
{
    const uint32_t type = data[0] & 0xf0u, channel = data[0] & 0x0fu;
    const size_t need = (type == 0xc0u || type == 0xd0u) ? 2u : 3u;
    uint32_t status = type, b2 = 0, value = 0;

    if (type < 0x80u || type > 0xe0u || size != need)
        return 0;
    switch (type)
    {
    case 0x90u:
    case 0x80u:
        b2 = data[1] & 0x7fu;
        if (type == 0x90u && (data[2] & 0x7fu) == 0)
        {
            status = 0x80u;
            value = OMX_CLAP_MIDI2_RELEASE_VELOCITY << 16;
        }
        else
            value = omx_clap_midi2_scale_up(data[2] & 0x7fu, 7, 16) << 16;
        break;
    case 0xa0u:
    case 0xb0u:
        b2 = data[1] & 0x7fu;
        value = omx_clap_midi2_scale_up(data[2] & 0x7fu, 7, 32);
        break;
    case 0xc0u:
        value = (uint32_t)(data[1] & 0x7fu) << 24;     // the program in the second word's top byte, no bank
        break;
    case 0xd0u:
        value = omx_clap_midi2_scale_up(data[1] & 0x7fu, 7, 32);
        break;
    default:                    // 0xe0
        value = omx_clap_midi2_scale_up((uint32_t)(data[2] & 0x7fu) << 7 | (data[1] & 0x7fu), 14, 32);
        break;
    }
    words[0] = 0x40000000u | (status | channel) << 16 | b2 << 8;
    words[1] = value;
    words[2] = 0;
    words[3] = 0;
    return 1;
}

/** RT: fill a CLAP_EVENT_MIDI2 slot with one MIDI 1.0 channel message, translated (omx_clap_midi2_from_midi1); a
 * message that does not translate is counted as unmappable. A release keeps its reserve, as it does as raw MIDI. */
static inline void omx_clap_midi2_slot(struct omx_clap_stage *s, uint32_t time, const uint8_t *data, size_t size)
{
    uint32_t words[4];
    union omx_clap_note *slot;

    if (!omx_clap_midi2_from_midi1(data, size, words))
    {
        omx_hosted_count(&s->notes_unmappable, 1);
        return;
    }
    slot = omx_clap_note_slot_for(s, time, CLAP_EVENT_MIDI2, sizeof(clap_event_midi2_t), omx_clap_midi_is_release(data, size));
    if (!slot)
        return;
    slot->midi2.port_index = 0;
    memcpy(slot->midi2.data, words, sizeof(words));
}

/** RT: fill a CLAP_EVENT_NOTE_EXPRESSION slot for note id `note_id` (-1: by channel and key); `key` -1 for every key
 * of the channel, `channel` -1 for every channel. */
static inline void omx_clap_expr_slot_id(struct omx_clap_stage *s, uint32_t time, int32_t expression_id, int32_t note_id,
                                         int16_t channel, int16_t key, double value)
{
    union omx_clap_note *slot = omx_clap_note_slot(s, time, CLAP_EVENT_NOTE_EXPRESSION, sizeof(clap_event_note_expression_t));

    if (!slot)
        return;
    slot->expr.expression_id = expression_id;
    slot->expr.note_id = note_id;
    slot->expr.port_index = 0;
    slot->expr.channel = channel;
    slot->expr.key = key;
    slot->expr.value = value;
}

/** RT: fill a CLAP_EVENT_NOTE_EXPRESSION slot with no note id; `key` -1 for a channel-wide expression. */
static inline void omx_clap_expr_slot(struct omx_clap_stage *s, uint32_t time, int32_t expression_id, int16_t channel,
                                      int16_t key, double value)
{
    omx_clap_expr_slot_id(s, time, expression_id, -1, channel, key, value);
}

/** RT: fill a CLAP note on or off slot. NULL past the block's room (omx_clap_note_slot). */
static inline union omx_clap_note *omx_clap_note_event(struct omx_clap_stage *s, uint32_t time, uint16_t type, int32_t note_id,
                                                       int16_t channel, int16_t key, double velocity)
{
    union omx_clap_note *slot = omx_clap_note_slot(s, time, type, sizeof(clap_event_note_t));

    if (!slot)
        return NULL;
    slot->note.note_id = note_id;
    slot->note.port_index = 0;
    slot->note.channel = channel;
    slot->note.key = key;
    slot->note.velocity = velocity;
    return slot;
}

/**
 * RT: a MIDI note on (`on`, velocity above 0) or note off as a CLAP note event. With note ids `n`, a note on takes a
 * free id (none free: skipped, counted in notes_skipped, its key marked OMX_CLAP_NOTE_SKIPPED) and a key already held
 * is first ended under its old id, so no voice is left without its note off; a note off carries the id its note on
 * took, is dropped when its note on was skipped, and goes with id -1 when the host holds no id for the key. A note off
 * with no room in the block keeps its key held, so a later one still ends it. With no `n`, every note has id -1.
 */
static inline void omx_clap_note_onoff(struct omx_clap_stage *s, struct omx_clap_note_ids *n, uint32_t time, uint8_t channel,
                                       uint8_t key, uint8_t velocity, int on)
{
    const int64_t now = s->steady_time + (int64_t)time;
    const double v = (double)velocity / 127.0;
    int32_t id;

    if (n == NULL)
    {
        omx_clap_note_event(s, time, on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF, -1, channel, key, v);
        return;
    }
    id = n->key_id[channel][key];
    if (!on)
    {
        if (id == OMX_CLAP_NOTE_SKIPPED)
        {
            n->key_id[channel][key] = -1;   // its note on never reached the plugin: nothing sounds
            return;
        }
        if (omx_clap_note_event(s, time, CLAP_EVENT_NOTE_OFF, id, channel, key, v) && id >= 0)
            omx_clap_note_id_release(n, id, now);
        return;
    }
    if (id >= 0)
    {
        if (!omx_clap_note_event(s, time, CLAP_EVENT_NOTE_OFF, id, channel, key, 0.0))
            return;     // no room to end the old note: the new one is not started over it
        omx_clap_note_id_release(n, id, now);
    }
    id = omx_clap_note_id_take(n, channel, key, now);
    if (id < 0)
    {
        n->key_id[channel][key] = OMX_CLAP_NOTE_SKIPPED;
        omx_hosted_count(&n->notes_skipped, 1);
        return;
    }
    if (!omx_clap_note_event(s, time, CLAP_EVENT_NOTE_ON, id, channel, key, v))
    {
        omx_clap_note_id_free(n, id);   // the block had no room: the note never started
        n->key_id[channel][key] = -1;
        return;
    }
    n->key_id[channel][key] = id;
    n->chan_id[channel] = id;
}

/** RT: every key held on `channel` is released at `now` (all notes off, all sound off). */
static inline void omx_clap_note_ids_release_channel(struct omx_clap_note_ids *n, uint8_t channel, int64_t now)
{
    for (uint32_t i = 0; i < OMX_CLAP_NOTE_IDS; i++)
        if (n->ids[i].state == 1 && n->ids[i].channel == channel)
            omx_clap_note_id_release(n, (int32_t)i, now);
    for (uint32_t k = 0; k < 128; k++)
        if (n->key_id[channel][k] == OMX_CLAP_NOTE_SKIPPED)
            n->key_id[channel][k] = -1;
}

/** Whether `channel` is a member channel of zone `zi` (0 lower, 1 upper) holding `members` member channels. */
static inline int omx_clap_mpe_is_member(uint32_t zi, uint32_t members, uint8_t channel)
{
    if (zi == 0)
        return channel >= 1 && channel <= members;
    return channel <= 14 && (uint32_t)channel + members >= 15u;
}

/** What channel `channel` is in the MPE zones: 0 outside them, 1 a zone's master, 2 a member; `*zi` is the zone (0 lower, 1 upper). */
static inline int omx_clap_mpe_role(const struct omx_clap_mpe_zones *z, uint8_t channel, uint32_t *zi)
{
    for (uint32_t i = 0; i < OMX_CLAP_MPE_ZONES; i++)
    {
        const uint32_t m = z->zone[i].members;

        if (m == 0)
            continue;
        *zi = i;
        if (channel == (i == 0 ? 0 : 15))
            return 1;
        if (omx_clap_mpe_is_member(i, m, channel))
            return 2;
    }
    return 0;
}

/**
 * RT: take zone `zi` (0 lower, 1 upper) with `members` member channels, 0 ending it. The zone's bend ranges return to
 * MPE's defaults and the master's kept values are cleared; the other zone is not touched, except for the channels
 * this one takes: with both zones the members share channels 2 to 15, so the other zone keeps 14 minus `members`
 * (none when `members` is 15, which reaches the other's master channel) and ends when none are left.
 */
static inline void omx_clap_mpe_take(struct omx_clap_mpe_zones *z, uint32_t zi, uint32_t members)
{
    struct omx_clap_mpe_zone *me = &z->zone[zi];
    struct omx_clap_mpe_zone *other = &z->zone[1u - zi];
    uint32_t room;

    if (members > 15)
        members = 15;
    memset(me, 0, sizeof(*me));
    if (members == 0)
        return;
    me->members = members;
    me->master_bend = OMX_CLAP_MPE_MASTER_BEND_DEFAULT;
    me->member_bend = OMX_CLAP_MPE_MEMBER_BEND_DEFAULT;
    room = members >= 15 ? 0u : 14u - members;
    if (other->members > room)
    {
        if (room == 0)
            memset(other, 0, sizeof(*other));
        else
            other->members = room;
    }
}

/**
 * RT: a controller of the RPN protocol (101 and 100 select, 6 and 38 set), tracked per channel. RPN 0 (pitch bend
 * sensitivity) on a zone's master sets that zone's master_bend, on one of its members its member_bend (semitones from
 * 6, cents from 38). RPN 6 on channel 1 or 16 is MPE's configuration message: its value is the member channel count of
 * the lower or the upper zone (0: no zone), and sets that zone alone. 1 when the controller was read: a selection, or a
 * value the zone took; a port that reads MIDI still gets it raw, a CLAP-only one does not count it unmappable.
 */
static inline int omx_clap_rpn(struct omx_clap_note_ids *n, struct omx_clap_mpe_zones *z, uint8_t channel, uint8_t cc,
                               uint8_t value)
{
    uint32_t zi = 0;
    const int role = z ? omx_clap_mpe_role(z, channel, &zi) : 0;
    double *range;

    if (cc == 101 || cc == 100)
    {
        n->rpn[channel][cc == 101 ? 0 : 1] = value;
        return 1;
    }
    if ((cc != 6 && cc != 38) || n->rpn[channel][0] != 0)
        return 0;
    if (z && n->rpn[channel][1] == 6 && cc == 6 && (channel == 0 || channel == 15))
    {
        omx_clap_mpe_take(z, channel == 0 ? 0u : 1u, value);
        return 1;
    }
    if (n->rpn[channel][1] != 0 || role == 0)
        return 0;
    range = role == 1 ? &z->zone[zi].master_bend : &z->zone[zi].member_bend;
    *range = cc == 6 ? (double)value : floor(*range) + (double)value / 100.0;
    return 1;
}

/**
 * RT: a message of a channel of an MPE zone that MPE makes per note: pitch bend (TUNING, the member or master range of
 * that zone), channel pressure (PRESSURE, 0 to 1) and controller 74 (BRIGHTNESS, 0 to 1). On a member channel the
 * expression carries the id, channel and key of the newest note held there (the whole channel when none is). On a
 * master it goes to each note held on the member channels of its own zone, with that note's id, channel and key, and
 * the value is kept for the notes that start in the zone later (omx_clap_mpe_note_start). 1 when the message was taken.
 */
static inline int omx_clap_mpe_in(struct omx_clap_stage *s, const struct omx_clap_note_ids *n, struct omx_clap_mpe_zones *z,
                                  uint32_t time, uint8_t type, uint8_t channel, const uint8_t *data, size_t size)
{
    uint32_t zi = 0;
    const int role = omx_clap_mpe_role(z, channel, &zi);
    struct omx_clap_mpe_zone *zone = &z->zone[zi];
    int32_t expression;
    double value;

    if (role == 0)
        return 0;
    if (type == 0xe0 && size == 3)
    {
        const int bend14 = ((int)data[2] << 7 | data[1]) - 8192;

        expression = CLAP_NOTE_EXPRESSION_TUNING;
        value = (double)bend14 / 8192.0 * (role == 2 ? zone->member_bend : zone->master_bend);
    }
    else if (type == 0xd0 && size == 2)
    {
        expression = CLAP_NOTE_EXPRESSION_PRESSURE;
        value = (double)data[1] / 127.0;
    }
    else if (type == 0xb0 && size == 3 && data[1] == 74)
    {
        expression = CLAP_NOTE_EXPRESSION_BRIGHTNESS;
        value = (double)data[2] / 127.0;
    }
    else
        return 0;
    if (role == 2)
    {
        const int32_t id = n->chan_id[channel];

        omx_clap_expr_slot_id(s, time, expression, id, (int16_t)channel, id >= 0 ? (int16_t)n->ids[id].key : (int16_t)-1, value);
        return 1;
    }
    if (expression == CLAP_NOTE_EXPRESSION_TUNING)
    {
        zone->master_tuning = value;
        zone->master_have |= OMX_CLAP_MPE_MASTER_BEND_BIT;
    }
    else if (expression == CLAP_NOTE_EXPRESSION_PRESSURE)
    {
        zone->master_pressure = value;
        zone->master_have |= OMX_CLAP_MPE_MASTER_PRESSURE_BIT;
    }
    else
    {
        zone->master_brightness = value;
        zone->master_have |= OMX_CLAP_MPE_MASTER_BRIGHTNESS_BIT;
    }
    for (uint32_t i = 0; i < OMX_CLAP_NOTE_IDS; i++)
        if (n->ids[i].state == 1 && n->ids[i].channel < 16 && omx_clap_mpe_is_member(zi, zone->members, (uint8_t)n->ids[i].channel))
            omx_clap_expr_slot_id(s, time, expression, (int32_t)i, n->ids[i].channel, n->ids[i].key, value);
    return 1;
}

/** RT: the note on of `key` on `channel` just started; when the channel is a member of a zone, the note begins with the
 * zone's master values the master sent since the zone was set (bend, pressure, controller 74), as expressions at the note on's frame. */
static inline void omx_clap_mpe_note_start(struct omx_clap_stage *s, const struct omx_clap_note_ids *n,
                                           const struct omx_clap_mpe_zones *z, uint32_t time, uint8_t channel, uint8_t key)
{
    uint32_t zi = 0;
    const int32_t id = n->key_id[channel][key];
    const struct omx_clap_mpe_zone *zone;

    if (id < 0 || omx_clap_mpe_role(z, channel, &zi) != 2)
        return;
    zone = &z->zone[zi];
    if (zone->master_have & OMX_CLAP_MPE_MASTER_BEND_BIT)
        omx_clap_expr_slot_id(s, time, CLAP_NOTE_EXPRESSION_TUNING, id, channel, key, zone->master_tuning);
    if (zone->master_have & OMX_CLAP_MPE_MASTER_PRESSURE_BIT)
        omx_clap_expr_slot_id(s, time, CLAP_NOTE_EXPRESSION_PRESSURE, id, channel, key, zone->master_pressure);
    if (zone->master_have & OMX_CLAP_MPE_MASTER_BRIGHTNESS_BIT)
        omx_clap_expr_slot_id(s, time, CLAP_NOTE_EXPRESSION_BRIGHTNESS, id, channel, key, zone->master_brightness);
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
 *    wants another range sets the field before arming): value = (bend14 - 8192) / 8192 * bend_semitones. All sound off
 *    (controller 120) becomes CLAP_EVENT_NOTE_CHOKE and all notes off (123, and 124 to 127, which imply it)
 *    CLAP_EVENT_NOTE_OFF, both on every key of the channel (key -1). Anything still unmappable (another controller, a
 *    program change, a sysex) is counted, never silently dropped.
 *  - A MIDI_MPE-dialect input: as the MIDI one; the plugin reads the MPE zone itself.
 *  - A MIDI2-dialect input (a port that declares MIDI 2.0 and neither CLAP nor MIDI, or prefers it): every channel
 *    message as the MIDI 2.0 channel voice message the UMP specification translates it to (omx_clap_midi2_from_midi1),
 *    as CLAP_EVENT_MIDI2; a sysex goes as it does for a CLAP input. A CLAP-dialect input whose port also declares
 *    MIDI 2.0 and not MIDI gets a controller and a program change that way too, rather than counted.
 * With note ids `n` (NULL: none, every CLAP note and expression has id -1), a CLAP-dialect input's notes carry ids
 * (omx_clap_note_onoff), poly aftertouch carries its key's id, and with MPE zones `z` (NULL: none read) the zones are
 * read: the configuration messages and RPN 0 (omx_clap_rpn), or a zone the control thread asked for
 * (omx_clap_mpe_zones_want); in a zone, a zone channel's bend, channel pressure and controller 74 become per-note
 * expressions (omx_clap_mpe_in), a master's those of its own zone's notes, whatever else the port reads.
 * A system byte besides sysex (0xf1 and up) is dropped; a channel message longer than three bytes is counted as unmappable.
 * The types each dialect passes, one line each (doc/reference.md repeats them; tests/dialect_doc_check.sh compares):
 * Passes, CLAP only: note on, note off, all sound off, all notes off, pitch bend, channel pressure, poly pressure.
 * Counts, CLAP only: other controller, program change, sysex.
 * Passes, CLAP and MIDI: note on, note off, every other channel message, sysex.
 * Passes, MIDI: every channel message, sysex.
 * Passes, MIDI MPE: every channel message, sysex only when the port also declares MIDI.
 * Passes, MIDI 2.0: every channel message as MIDI 2.0, sysex only when the port also declares MIDI.
 * Passes, CLAP and MIDI 2.0: note on, note off, all sound off, all notes off, pitch bend, channel pressure, poly pressure, other controller and program change as MIDI 2.0.
 * Counts, CLAP and MIDI 2.0: sysex.
 * A full block drops what can be lost first: past CLAP_HOST_NOTES_PER_BLOCK - OMX_CLAP_NOTES_RESERVED a message that is
 * not a release is counted and dropped, and a release only past CLAP_HOST_NOTES_PER_BLOCK (OMX_CLAP_NOTES_RESERVED);
 * past OMX_CLAP_SYSEX_PER_BLOCK a sysex is counted and dropped too. A stage with no note input takes none.
 */
static inline void omx_clap_note_in_ids(struct omx_clap_stage *s, struct omx_clap_note_ids *n, struct omx_clap_mpe_zones *z,
                                        uint32_t time, const uint8_t *data, size_t size)
{
    const int also_midi = (s->note_dialects & CLAP_NOTE_DIALECT_MIDI) != 0;
    const int also_midi2 = (s->note_dialects & CLAP_NOTE_DIALECT_MIDI2) != 0;
    uint8_t type, channel;
    union omx_clap_note *slot;

    if (!s->note_inputs || size == 0 || data[0] < 0x80)
        return;
    if (z)
        for (uint32_t zi = 0; zi < OMX_CLAP_MPE_ZONES; zi++)
            if (atomic_load_explicit(&z->wanted[zi], memory_order_relaxed))
            {
                const uint32_t want = atomic_exchange_explicit(&z->wanted[zi], 0u, memory_order_acquire);

                if (want & OMX_CLAP_MPE_WANTED)
                    omx_clap_mpe_take(z, zi, (want >> 8) & 0xffu);
            }

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

    if (s->note_dialect == CLAP_NOTE_DIALECT_MIDI || s->note_dialect == CLAP_NOTE_DIALECT_MIDI_MPE)
    {
        omx_clap_midi_slot(s, time, data, size);     // MIDI_MPE: raw, the plugin reads the zone itself
        return;
    }
    if (s->note_dialect == CLAP_NOTE_DIALECT_MIDI2)
    {
        omx_clap_midi2_slot(s, time, data, size);    // every channel message, as its MIDI 2.0 channel voice message
        return;
    }

    if ((type == 0x80 || type == 0x90) && size == 3)
    {
        omx_clap_note_onoff(s, n, time, channel, data[1] & 0x7f, data[2] & 0x7f, type == 0x90 && data[2]);
        if (n && z && type == 0x90 && data[2])
            omx_clap_mpe_note_start(s, n, z, time, channel, data[1] & 0x7f);
        return;
    }

    if (n && type == 0xb0 && size == 3)
    {
        if (omx_clap_rpn(n, z, channel, data[1], data[2]) && !also_midi)
            return;                             // the host read it, and the port takes no raw MIDI
        if (data[1] == 120 || data[1] >= 123)   // the plugin ends every note of the channel
            omx_clap_note_ids_release_channel(n, channel, s->steady_time + (int64_t)time);
    }
    if (n && z && omx_clap_mpe_in(s, n, z, time, type, channel, data, size))
        return;

    if (also_midi)
    {
        omx_clap_midi_slot(s, time, data, size);
        return;
    }

    if (type == 0xb0 && size == 3 && (data[1] == 120 || data[1] >= 123))   // all sound off, all notes off: every key
    {
        omx_clap_note_event(s, time, data[1] == 120 ? CLAP_EVENT_NOTE_CHOKE : CLAP_EVENT_NOTE_OFF, -1, (int16_t)channel, -1, 0.0);
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
    if (type == 0xa0 && size == 3)              // poly aftertouch -> one key of the channel, under its note's id
    {
        const int32_t held = n ? n->key_id[channel][data[1] & 0x7f] : -1;

        omx_clap_expr_slot_id(s, time, CLAP_NOTE_EXPRESSION_PRESSURE, held >= 0 ? held : -1, (int16_t)channel, (int16_t)data[1],
                              (double)data[2] / 127.0);
        return;
    }
    if (also_midi2)
    {
        omx_clap_midi2_slot(s, time, data, size);    // a controller or a program change, in the port's other dialect
        return;
    }
    omx_hosted_count(&s->notes_unmappable, 1);  // a controller or a program change: no CLAP note expression carries it
}

/** RT: omx_clap_note_in_ids with no note ids: every CLAP note and expression goes with id -1, and no MPE zone is read. */
static inline void omx_clap_note_in(struct omx_clap_stage *s, uint32_t time, const uint8_t *data, size_t size)
{
    omx_clap_note_in_ids(s, NULL, NULL, time, data, size);
}

/** RT: fill every main output channel the plugin reported constant from its sample 0. */
static inline void omx_clap_expand_constant(struct omx_clap_stage *s, uint32_t n)
{
    const uint64_t mask = s->aout[0].constant_mask;
    uint32_t c, i;

    if (mask == 0)
        return;
    for (c = 0; c < s->aout[0].channel_count; c++)
    {
        float *b;
        float v;

        if (!(mask & ((uint64_t)1 << c)))
            continue;
        b = s->aout[0].data32[c];
        v = b[0];
        for (i = 1; i < n; i++)
            b[i] = v;
        omx_hosted_count(&s->constant_channels, 1);
    }
}

/**
 * The plugin's half of a block: drain, process, flush-to-zero, expand. Shared by the live body and the warm-up so the
 * warm-up exercises exactly the path the RT will. Returns 1 when the output is usable, 0 when the block must be
 * discarded (a process error or, for a stage that scans, a non-finite sample). `*said`, when `said` is not NULL, is the
 * status process() returned.
 */
static inline int omx_clap_run_plugin_status(struct omx_clap_stage *s, uint32_t n, clap_process_status *said)
{
    clap_process_status status;

    omx_clap_drain(s);
    s->aout[0].constant_mask = 0;   ///< the plugin's to set; never carried from the last block
    // the inputs' masks are the plugin's to set too: the process list is the stage's `ain` or the instance's `ains`
    for (uint32_t i = 0; i < s->proc.audio_inputs_count; i++)
        ((clap_audio_buffer_t *)s->proc.audio_inputs)[i].constant_mask = 0;
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
    omx_denormals_off();
    s->steady_time += (int64_t)n;
    s->n_events = 0;
    if (said)
        *said = status;
    if (status == CLAP_PROCESS_ERROR)
        return OMX_HOSTED_BLOCK_ERROR;       // counted once, in the strike (omx_hosted_run_end_why)
    omx_clap_expand_constant(s, n);
    return omx_hosted_out_finite(&s->h, n) ? OMX_HOSTED_BLOCK_USABLE : OMX_HOSTED_BLOCK_NONFINITE;
}

/** RT: omx_clap_run_plugin_status, the plugin's process status not kept. */
static inline int omx_clap_run_plugin(struct omx_clap_stage *s, uint32_t n)
{
    return omx_clap_run_plugin_status(s, n, NULL);
}

/** whether auxiliary output channel `k` (of the flat list, port by port) writes the caller's buffer, not its scratch:
 * only a bound channel is read, so only a bound channel is guarded and silenced */
static inline int omx_clap_aux_out_bound(const struct omx_clap_stage *s, uint32_t k)
{
    return s->aux_ptrs[k] != s->aux_scratch[k];
}

/**
 * RT, a live block the plugin ran with the verdict `usable` on its main output: every bound auxiliary output channel scanned
 * when the stage guards for non-finite output (one non-finite sample makes the block OMX_HOSTED_BLOCK_NONFINITE, which
 * strikes and delivers as a main one does) and, the block still usable, clamped when the stage clamps (counted in
 * clamped_samples). Returns the block's verdict.
 */
static inline int omx_clap_aux_out_guard(struct omx_clap_stage *s, int usable, uint32_t n)
{
    uint32_t i, c, moved = 0;

    if (s->aux_bound_channels == 0)
        return usable;
    if (usable == OMX_HOSTED_BLOCK_USABLE && (s->h.guards & OMX_HOSTED_GUARD_NONFINITE))
        for (i = 0; i < s->n_aux && usable == OMX_HOSTED_BLOCK_USABLE; i++)
            for (c = 0; c < s->aout[1 + i].channel_count; c++)
                if (omx_clap_aux_out_bound(s, s->aux_base[i] + c) && !omx_block_finite(s->aout[1 + i].data32[c], n))
                {
                    usable = OMX_HOSTED_BLOCK_NONFINITE;
                    break;
                }
    if (usable != OMX_HOSTED_BLOCK_USABLE || !(s->h.guards & OMX_HOSTED_GUARD_CLAMP))
        return usable;
    for (i = 0; i < s->n_aux; i++)
        for (c = 0; c < s->aout[1 + i].channel_count; c++)
            if (omx_clap_aux_out_bound(s, s->aux_base[i] + c))
                moved += omx_hosted_clamp(s->aout[1 + i].data32[c], n);
    if (moved)
        omx_hosted_count(&s->h.clamped_samples, moved);
    return usable;
}

/**
 * RT, a block whose auxiliary outputs the plugin did not write usable (not processing, a block longer than the bounce,
 * a bypass, a fault, an unusable block, a sleep): each bound auxiliary output channel silent for the block, never the
 * last block's samples again; a scratch block nothing reads is left. At most max_block frames, the length every bound
 * block has.
 */
static inline void omx_clap_aux_out_silence(struct omx_clap_stage *s, uint32_t n)
{
    const uint32_t m = n < s->h.max_block ? n : s->h.max_block;
    uint32_t i, c;

    if (s->aux_bound_channels == 0)
        return;
    for (i = 0; i < s->n_aux; i++)
        for (c = 0; c < s->aout[1 + i].channel_count; c++)
            if (omx_clap_aux_out_bound(s, s->aux_base[i] + c))
                memset(s->aout[1 + i].data32[c], 0, m * sizeof(float));
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
 * It has one writer, which publishes it whole with omx_clap_transport_publish: the control thread, or an audio thread
 * that publishes once per cycle (a console counting its song position in frames), never both. Any number of audio
 * threads read it whole with omx_clap_transport_read, in the same cycle as the writer or not, so one block never pairs
 * the fields of two publishes. Neither side allocates, locks or calls out. Its storage is the caller's and must outlive
 * the instance's publication.
 *
 * The ordering, with no standalone fence (which ThreadSanitizer cannot see): the writer makes the sequence odd, stores
 * each field with release, and makes the sequence even with release; a reader loads the sequence and each field with
 * acquire, then the sequence again. A field the reader took from a publish that had begun synchronises with that
 * publish's odd store, so the second load of the sequence cannot read the even value of the first, and the read is
 * tried again.
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

/** The one writer, the control thread or an audio thread (RT-safe: no allocation, lock or call out): publish `t`
 * whole. Seen by the next read; the sequence is odd while it is written. */
static inline void omx_clap_transport_publish(struct omx_clap_transport_src *src, const struct omx_clap_transport *t)
{
    uint64_t bpm_bits = 0;
    uint32_t seq = atomic_load_explicit(&src->seq, memory_order_relaxed);

    memcpy(&bpm_bits, &t->tempo, sizeof bpm_bits);
    atomic_store_explicit(&src->seq, seq + 1u, memory_order_relaxed);
    atomic_store_explicit(&src->flags, t->flags, memory_order_release);
    atomic_store_explicit(&src->tempo_bits, bpm_bits, memory_order_release);
    atomic_store_explicit(&src->song_pos_beats, t->song_pos_beats, memory_order_release);
    atomic_store_explicit(&src->song_pos_seconds, t->song_pos_seconds, memory_order_release);
    atomic_store_explicit(&src->bar_start, t->bar_start, memory_order_release);
    atomic_store_explicit(&src->bar_number, t->bar_number, memory_order_release);
    atomic_store_explicit(&src->tsig_num, t->tsig_num, memory_order_release);
    atomic_store_explicit(&src->tsig_denom, t->tsig_denom, memory_order_release);
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
        t.flags = atomic_load_explicit(&src->flags, memory_order_acquire);
        bpm_bits = atomic_load_explicit(&src->tempo_bits, memory_order_acquire);
        t.song_pos_beats = atomic_load_explicit(&src->song_pos_beats, memory_order_acquire);
        t.song_pos_seconds = atomic_load_explicit(&src->song_pos_seconds, memory_order_acquire);
        t.bar_start = atomic_load_explicit(&src->bar_start, memory_order_acquire);
        t.bar_number = atomic_load_explicit(&src->bar_number, memory_order_acquire);
        t.tsig_num = atomic_load_explicit(&src->tsig_num, memory_order_acquire);
        t.tsig_denom = atomic_load_explicit(&src->tsig_denom, memory_order_acquire);
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

/* the two-leg body; 1 when the plugin wrote this block's auxiliary outputs usable, 0 when the caller silences them */
static inline int omx_clap_run_locked_status(struct omx_clap_stage *s, float *l, float *r, uint32_t n, clap_event_transport_t *t,
                                             const struct omx_clap_transport *rec, clap_process_status *said)
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
        return 0;
    }
    switch (omx_hosted_run_begin(&s->h, l, r, n, &want_wet))
    {
        case OMX_HOSTED_SKIP:
            return 0;
        case OMX_HOSTED_IDLE:
            s->need_reset = 1;  ///< a tail may be frozen: the control thread resets the plugin at re-engage, never this one
            return 0;
        case OMX_HOSTED_GO:
            break;
    }
    if (t && rec && rec->flags)
    {
        omx_clap_transport_fill(t, rec);
        s->proc.transport = t;
    }
    usable = omx_clap_aux_out_guard(s, omx_clap_run_plugin_status(s, n, said), n);
    s->proc.transport = NULL;
    omx_hosted_run_end_why(&s->h, usable, &s->process_errors, want_wet, l, r, n);
    return usable == OMX_HOSTED_BLOCK_USABLE;
}

/** RT: omx_clap_run_locked_status, the plugin's process status not kept. */
static inline void omx_clap_run_locked(struct omx_clap_stage *s, float *l, float *r, uint32_t n,
                                       clap_event_transport_t *t, const struct omx_clap_transport *rec)
{
    if (!omx_clap_run_locked_status(s, l, r, n, t, rec, NULL))
        omx_clap_aux_out_silence(s, n);
}

/**
 * RT: the stage's body for one block, in place on the lane (`l`, and `r` or NULL for a mono lane), every guard as the
 * header comment lists them. A block longer than the bounce is passed through untouched and counted. The block marks
 * itself in_cycle, so the control thread taking the audio role waits for it, and forgets the notes it was handed.
 * omx_clap_run_transport is the same body handing a processing block the transport `rec` in `t` (NULL, or `rec` with no
 * flags: transport NULL); omx_clap_run hands none. `*said`, when `said` is not NULL, is the status the plugin's
 * process() returned this block, or -1 when it was not called (not processing, bypassed and idle, an oversize block).
 */
static inline void omx_clap_run_transport_status(struct omx_clap_stage *s, float *l, float *r, uint32_t n, clap_event_transport_t *t,
                                                 const struct omx_clap_transport *rec, clap_process_status *said)
{
    if (said)
        *said = -1;     // no process() was called
    if (s == NULL)
        return;
    if (s->plugin == NULL || l == NULL || n == 0)
    {
        s->n_notes = 0;     // no block follows these events: they must not reach the next one with their old frame times
        s->n_sysex = 0;
        return;
    }
    atomic_store(&s->in_cycle, 1);
    if (!omx_clap_run_locked_status(s, l, r, n, t, rec, said))
        omx_clap_aux_out_silence(s, n);
    s->n_notes = 0;
    s->n_sysex = 0;
    atomic_store(&s->in_cycle, 0);
}

/** RT: omx_clap_run_transport_status, the plugin's process status not kept. */
static inline void omx_clap_run_transport(struct omx_clap_stage *s, float *l, float *r, uint32_t n,
                                          clap_event_transport_t *t, const struct omx_clap_transport *rec)
{
    omx_clap_run_transport_status(s, l, r, n, t, rec, NULL);
}

/** Run one block of `n` frames in place on `l` and `r`, with no transport. */
static inline void omx_clap_run(struct omx_clap_stage *s, float *l, float *r, uint32_t n)
{
    omx_clap_run_transport(s, l, r, n, NULL, NULL);
}

/**
 * RT, a cycle as a client with separate input and output buffers has it (JACK): the lane is the output pair, an
 * effect's input is copied onto it and an instrument's lane starts silent. `inputs` is not read when the stage has no
 * audio input and may then be NULL. omx_clap_run_io and omx_clap_host_run_io run the stage on the lane after it.
 */
static inline void omx_clap_io_lane(const struct omx_clap_stage *s, const float *const *inputs, float *const *outputs, uint32_t n)
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
}

/** RT, a cycle as a client with separate input and output buffers has it (JACK): omx_clap_io_lane, then omx_clap_run on
 * the lane, with no transport. */
static inline void omx_clap_run_io(struct omx_clap_stage *s, const float *const *inputs, float *const *outputs, uint32_t n)
{
    omx_clap_io_lane(s, inputs, outputs, n);
    omx_clap_run(s, outputs[0], s->h.n_out == 2 ? outputs[1] : NULL, n);
}

/**
 * RT, a cycle with one buffer per main channel on the two-leg lane: the right leg of a stereo input to a mono output
 * (2 to 1), whose lane has one output buffer only. The bounce's right output, which a mono output never writes, carries
 * `ins[1]` for the block, so the plugin reads both inputs; its mono output is then delivered on both legs, of which the
 * caller keeps the left. NULL for any other pair, or a block longer than the bounce (which is passed through unrun).
 */
static inline float *omx_clap_ports_right(struct omx_clap_stage *s, const float *const *ins, uint32_t n)
{
    if (s->h.n_in != 2 || s->h.n_out != 1 || ins == NULL || n > s->h.max_block)
        return NULL;
    memcpy(s->h.out_r, ins[1], n * sizeof(float));
    return s->h.out_r;
}

/** The main input's channels as the stage was bound: 0 for a source. */
static inline uint32_t omx_clap_main_in_channels(const struct omx_clap_stage *s)
{
    return s->h.n_in ? s->proc.audio_inputs[0].channel_count : 0;
}

/** RT: the dry block of the wide path: output channel `c` is input channel c, the last input past them, silence for a
 * source. */
static inline void omx_clap_ports_dry(const float *const *ins, uint32_t n_in, float *const *outs, uint32_t n_out, uint32_t n)
{
    uint32_t c;

    for (c = 0; c < n_out; c++)
    {
        if (n_in == 0)
        {
            memset(outs[c], 0, n * sizeof(float));
        }
        else
        {
            const float *in = ins[c < n_in ? c : n_in - 1];

            if (outs[c] != in)
                memcpy(outs[c], in, n * sizeof(float));
        }
    }
}

/** RT: the state transitions a block makes before it runs (ARMED to PROCESSING with start_processing, STOPPING to
 * STOPPED with stop_processing): the state the block runs in. */
static inline uint32_t omx_clap_take_state(struct omx_clap_stage *s)
{
    uint32_t st = atomic_load_explicit(&s->state, memory_order_acquire);

    if (st == OMX_CLAP_ARMED && atomic_compare_exchange_strong(&s->state, &st, OMX_CLAP_PROCESSING))
    {
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
    return st;
}

/**
 * RT, the wide path's body for one block: `ins` one buffer per main input channel, `outs` one per main output channel.
 * Not processing, a block longer than the bounce (counted) or a commanded bypass or a fault carries dry; otherwise the
 * input is copied into the wide bounce, the plugin runs, and each output channel is clamped (OMX_HOSTED_GUARD_CLAMP,
 * counted in clamped_samples) and scanned (OMX_HOSTED_GUARD_NONFINITE): an unusable block delivers dry and strikes as
 * omx_hosted_run_end_why does. Bypass switches the body hard, with no crossfade, on the block it is commanded. The
 * auxiliary outputs are guarded with the main ones (omx_clap_aux_out_guard). Returns 1 when the plugin wrote them
 * usable, 0 on every dry path, where the caller silences them (omx_clap_aux_out_silence).
 */
static inline int omx_clap_run_ports_locked(struct omx_clap_stage *s, const float *const *ins, float *const *outs, uint32_t n,
                                            clap_event_transport_t *t, const struct omx_clap_transport *rec)
{
    const uint32_t n_in = omx_clap_main_in_channels(s);
    const uint32_t n_out = s->aout[0].channel_count;
    const float *const *dry = (const float *const *)s->wide_in;
    uint32_t c, moved = 0;
    int usable;

    if (omx_clap_take_state(s) != OMX_CLAP_PROCESSING)
    {
        omx_clap_ports_dry(ins, n_in, outs, n_out, n);
        atomic_store_explicit(&s->h.rendered_wet, 0, memory_order_release);
        return 0;
    }
    if (n > s->h.max_block)
    {
        omx_hosted_count(&s->h.oversize_blocks, 1);
        omx_clap_ports_dry(ins, n_in, outs, n_out, n);
        atomic_store_explicit(&s->h.rendered_wet, 0, memory_order_release);
        return 0;
    }
    if (atomic_load_explicit(&s->h.bypass, memory_order_acquire) != 0u
        || atomic_load_explicit(&s->h.fault, memory_order_relaxed) != OMX_HOSTED_FAULT_NONE)
    {
        omx_clap_ports_dry(ins, n_in, outs, n_out, n);
        atomic_store_explicit(&s->h.rendered_wet, 0, memory_order_release);
        s->need_reset = 1;  ///< a tail may be frozen: the control thread resets the plugin at re-engage, never this one
        return 0;
    }
    for (c = 0; c < n_in; c++)
        memcpy(s->wide_in[c], ins[c], n * sizeof(float));
    if (t && rec && rec->flags)
    {
        omx_clap_transport_fill(t, rec);
        s->proc.transport = t;
    }
    usable = omx_clap_run_plugin(s, n);
    s->proc.transport = NULL;
    if (usable == OMX_HOSTED_BLOCK_USABLE && (s->h.guards & OMX_HOSTED_GUARD_NONFINITE))
        for (c = 0; c < n_out; c++)
            if (!omx_block_finite(s->wide_out[c], n))
                usable = OMX_HOSTED_BLOCK_NONFINITE;
    usable = omx_clap_aux_out_guard(s, usable, n);
    if (usable != OMX_HOSTED_BLOCK_USABLE)
    {
        omx_clap_ports_dry(dry, n_in, outs, n_out, n);
        omx_hosted_run_end_why(&s->h, usable, &s->process_errors, 0u, NULL, NULL, n);   // counts, strikes, marks the body dry
        return 0;
    }
    omx_hosted_count(&s->h.runs, 1);
    if (s->h.guards & OMX_HOSTED_GUARD_CLAMP)
    {
        for (c = 0; c < n_out; c++)
            moved += omx_hosted_clamp(s->wide_out[c], n);
        if (moved)
            omx_hosted_count(&s->h.clamped_samples, moved);
    }
    for (c = 0; c < n_out; c++)
        memcpy(outs[c], s->wide_out[c], n * sizeof(float));
    atomic_store_explicit(&s->h.rendered_wet, 1, memory_order_release);
    return 1;
}

/** RT: omx_clap_run_ports_locked inside the cycle bracket, handing a processing block the transport `rec` in `t` (NULL,
 * or `rec` with no flags: transport NULL); the block forgets the notes it was handed. */
static inline void omx_clap_run_ports_transport(struct omx_clap_stage *s, const float *const *ins, float *const *outs, uint32_t n,
                                                clap_event_transport_t *t, const struct omx_clap_transport *rec)
{
    if (s == NULL)
        return;
    if (s->plugin == NULL || outs == NULL || (s->h.n_in && ins == NULL) || n == 0)
    {
        s->n_notes = 0;     // no block follows these events: they must not reach the next one with their old frame times
        s->n_sysex = 0;
        return;
    }
    atomic_store(&s->in_cycle, 1);
    if (!omx_clap_run_ports_locked(s, ins, outs, n, t, rec))
        omx_clap_aux_out_silence(s, n);
    s->n_notes = 0;
    s->n_sysex = 0;
    atomic_store(&s->in_cycle, 0);
}

/**
 * RT, a cycle with one buffer per main channel (JACK): `ins` one per main input channel (not read for a source, and may
 * then be NULL), `outs` one per main output channel, the counts the stage was bound with. A stage without the wide
 * bounce runs the two-leg path, omx_clap_run_io, bit for bit, except a stereo input to a mono output, whose right leg
 * is omx_clap_ports_right; a wide stage runs omx_clap_run_ports_transport with no transport.
 */
static inline void omx_clap_run_ports(struct omx_clap_stage *s, const float *const *ins, float *const *outs, uint32_t n)
{
    if (s && !s->wide)
    {
        float *r = s->plugin && outs && n ? omx_clap_ports_right(s, ins, n) : NULL;

        if (r)
        {
            omx_clap_io_lane(s, ins, outs, n);
            omx_clap_run(s, outs[0], r, n);
            return;
        }
        omx_clap_run_io(s, ins, outs, n);
        return;
    }
    omx_clap_run_ports_transport(s, ins, outs, n, NULL, NULL);
}

/**
 * Control thread, after activate and before publish, holding the audio role: start_processing, the warm-up blocks of `n`
 * frames through the same drain, process, flush-to-zero and expand path the RT takes, then reset() and stop_processing.
 * Returns the number of blocks whose output was unusable: anything but 0 means do not publish. The lane and the live
 * counters are untouched; the stage must not be published (state IDLE) while this runs.
 */
static inline int omx_clap_prime_block(void *ctx, uint32_t n)
{
    struct omx_clap_stage *s = (struct omx_clap_stage *)ctx;
    const uint32_t n_in = s->h.n_in ? s->proc.audio_inputs[0].channel_count : 0;

    // the warm-up signal the legs carry, on every channel of a wide main input (a leg that is the bounce has it already)
    for (uint32_t c = 0; s->wide && c < n_in; c++)
        if (s->wide_in[c] != s->h.in_l)
            memcpy(s->wide_in[c], s->h.in_l, n * sizeof(float));
    return omx_clap_run_plugin(s, n);
}

/**
 * Control thread, from omx_clap_prime: every block the plugin reads or writes, a full max_block of it, made resident,
 * so no page of a channel past the legs faults in on the first live block. The stage's own blocks (a wide main input,
 * every output) are zeroed; a block that may be the caller's input (an auxiliary input, the silence, a leg) is read
 * and written back as it is.
 */
static inline void omx_clap_prime_touch(struct omx_clap_stage *s)
{
    const uint32_t frames = s->h.max_block;

    for (uint32_t i = 0; i < s->proc.audio_inputs_count; i++)
    {
        const clap_audio_buffer_t *b = &s->proc.audio_inputs[i];
        const int own = s->wide && i == 0 && s->h.n_in;

        for (uint32_t c = 0; c < b->channel_count; c++)
        {
            volatile float *v = b->data32[c];

            if (own)
                memset(b->data32[c], 0, frames * sizeof(float));
            else
                for (uint32_t k = 0; k < frames; k++)
                    v[k] = v[k];
        }
    }
    for (uint32_t i = 0; i < s->proc.audio_outputs_count; i++)
        for (uint32_t c = 0; c < s->proc.audio_outputs[i].channel_count; c++)
            memset(s->proc.audio_outputs[i].data32[c], 0, frames * sizeof(float));
}

/** Prime an idle plugin with `n` frames of the warm-up signal on every main input channel, every block it reads or writes
 * first made resident for max_block frames, then reset and stop it: the number of bad blocks, UINT32_MAX when it cannot run. */
static inline uint32_t omx_clap_prime(struct omx_clap_stage *s, uint32_t n)
{
    uint32_t bad;

    if (s == NULL || s->plugin == NULL || n == 0 || n > s->h.max_block)
        return UINT32_MAX;
    if (atomic_load_explicit(&s->state, memory_order_acquire) != OMX_CLAP_IDLE)
        return UINT32_MAX;
    if (s->plugin->start_processing && !s->plugin->start_processing(s->plugin))
        return UINT32_MAX;
    omx_clap_prime_touch(s);
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
