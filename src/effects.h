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
************************************************************************************************************************
*/

#ifndef EFFECTS_H
#define EFFECTS_H


/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <stddef.h>
#include <stdint.h>

#include "host-errors.h"


/*
************************************************************************************************************************
*           CONFIGURATION DEFINES
************************************************************************************************************************
*/

#define MAX_PLUGIN_INSTANCES    9990
#define MAX_TOOL_INSTANCES      10
#define MAX_INSTANCES           (MAX_PLUGIN_INSTANCES + MAX_TOOL_INSTANCES)


/*
************************************************************************************************************************
*           FUNCTION PROTOTYPES
************************************************************************************************************************
*/

/* the state root -s names, before effects_init; NULL for the default ($XDG_STATE_HOME/omx-clap-host) */
void effects_set_state_root(const char *dir);
int effects_init(void);
int effects_finish(void);
int effects_add(const char *uri, int instance, const char *client_name);
/* the hosting.* code of the last effects_add that failed with one, "" when it named none */
const char *effects_add_reason(void);
int effects_pin_expect(int instance, const char *layout);
int effects_remove(int effect_id);
int effects_bypass(int effect_id, int value);
int effects_set_parameter(int effect_id, const char *control_symbol, float value);
int effects_get_parameter(int effect_id, const char *control_symbol, float *value);
int effects_preset_load(int effect_id, const char *location);
/* preset_list: the count of the plugin's own presets, listed again and kept for preset_get */
int effects_preset_list(int effect_id);
/* preset_get: one row of the last preset_list, "resp 0 <load_key> <name>", into `reply` */
int effects_preset_get(int effect_id, int index, char *reply, size_t size);
/* preset_save: the instance into an org.openmixer.preset/2 file; SUCCESS with "resp 0" in `reply`, or the refusal's line */
int effects_preset_save(int effect_id, const char *path, const char *name, char *reply, size_t size);
/* preset_load: a preset file (an absolute path) or a key of preset_get; the reply line, a refusal's with its code */
int effects_preset_load_reply(int effect_id, const char *location, char *reply, size_t size);
int effects_state_save(const char *dir);
int effects_state_load(const char *dir);
int effects_connect(const char *portA, const char *portB);
int effects_disconnect(const char *portA, const char *portB);
int effects_monitor_output(int effect_id, const char *symbol);
int effects_track_info(int effect_id, const char *name, const char *color, const char *kind);
int effects_chain(int id, const int *instances, int n);
int effects_remote_pages(int effect_id);
int effects_remote_page_get(int effect_id, int page, char *reply, size_t size);
/* param_info: "resp 0 <unit> <scale> <min> <max> <default> <step> <stable_symbol>" from the plugin's parameter contract */
int effects_param_info(int effect_id, const char *symbol, char *reply, size_t size);
/* audio_ports: "resp 0", " dry" while the instance passes dry, and a record per registered audio port, inputs first and
 * each side's main port first, each "side:role:index:channels:kind[:layout][:assumed]:\"name\":jack,jack" (layout_pin.h,
 * layout_port_text), into *reply, the caller's to free; ERR_INSTANCE_NON_EXISTS, ERR_MEMORY_ALLOCATION */
int effects_audio_ports(int effect_id, char **reply);
float effects_jack_cpu_load(void);
/* note_drops: "resp 0 <dropped> <releases> <sysex> <unmappable>", the instance's note counters since add */
int effects_note_drops(int effect_id, char *reply, size_t size);
/* the host's transport: set_bpm, set_bpb and transport answer ERR_JACK_VALUE_OUT_OF_RANGE outside mod-host's ranges;
 * transport_sync answers ERR_INVALID_OPERATION for link and midi */
int effects_set_bpm(double bpm);
int effects_set_bpb(double bpb);
int effects_transport(int rolling, double bpb, double bpm);
int effects_transport_sync(const char *mode);
/* -t, before the first add: every plugin reads the JACK transport instead of the host's */
void effects_follow_jack_transport(int on);
/* -c, before the first add: the lane's width, 1 or 2, asked of a plugin whose main ports differ (request_layout) */
void effects_set_lane_channels(uint32_t channels);
/* the CPU idle hold's word, as --idle-hold and the idle_hold command take it: "off", or a latency in microseconds, 0 for
 * no idle state; 0 for a word that is neither */
int effects_idle_hold_word(const char *word, int32_t *target_us);
/* --idle-hold, before the first add: what /dev/cpu_dma_latency is held at while a plugin is active (#296) */
void effects_set_idle_hold(int32_t target_us);
/* idle_hold <word>: the target changed at once, ERR_INVALID_OPERATION for a word effects_idle_hold_word refuses */
int effects_idle_hold_set(const char *word);
/* idle_hold: "resp 0 <target> <held> <aggregate> <active> <code>", target and held "off" or microseconds, aggregate the
 * device's own answer or "-", active the plugins active, code the refusal or "-" */
int effects_idle_hold(char *reply, size_t size);
/* note_out_drops: "resp 0 <dropped> <unmapped> <port_full>", the instance's note output counters since add */
int effects_note_out_drops(int effect_id, char *reply, size_t size);
/* note_input: "resp 0 midi_in" or "resp 0 -", whether the instance takes notes */
int effects_note_input(int effect_id, char *reply, size_t size);
/* note_on, note_off, all_notes_off: `count` 3-byte MIDI messages queued whole for the start of the next cycle; -513 for
 * an instance that takes no notes, OMX_REPLY_HOST_NOTE_QUEUE_FULL_RESP (-517) when the queue has no room for them all */
int effects_note(int effect_id, const uint8_t (*messages)[3], uint32_t count);
/* show_external_ui: the plugin's own window shown (#180): SUCCESS, ERR_INSTANCE_NON_EXISTS, or ERR_EXTERNAL_UI_UNAVAILABLE
 * with *code the hosting code that says why (CLAP_HOST_CODE_WINDOW_*) */
int effects_show_ui(int effect_id, const char **code);
/* hide_external_ui: the window off the display, kept for the next show; SUCCESS also when there is none to hide */
int effects_hide_ui(int effect_id);
/* -S, before the first add: every plugin's window shown at its add, the add answered as before whatever it shows */
void effects_show_ui_at_add(int on);
/* --thread-pool, before the first add: which plugins get the process's thread pool (#395, docs/thread-pool.md).
 * QUALIFIED, the default: those the host is told their qualification grants it, and this host is told of none yet, so
 * every plugin runs its tasks serial; ON: every plugin that asks, for a worker plugin-hostd starts for a plugin whose
 * pin grants it; OFF: none */
#define EFFECTS_THREAD_POOL_QUALIFIED   0
#define EFFECTS_THREAD_POOL_OFF         1
#define EFFECTS_THREAD_POOL_ON          2
/* the word --thread-pool takes, qualified, off or on: 1 with *mode set, 0 for any other */
int effects_thread_pool_word(const char *word, int *mode);
void effects_set_thread_pool(int mode);
void effects_idle(void);


/*
************************************************************************************************************************
*           END HEADER
************************************************************************************************************************
*/

#endif
