// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#ifndef OMX_CLAP_CORE_HOST_VERBS_H
#define OMX_CLAP_CORE_HOST_VERBS_H
/*
 * GENERATED: DO NOT EDIT BY HAND.
 * Produced by `omx-contract render --target c` from omx-clap-core's files.
 * The host verbs (docs/contract-dir.md): a verb is OMX_VERB_<VERB>, its word, OMX_VERB_<VERB>_FMT, the line
 * protocol_add_command takes, and OMX_VERB_<VERB>_REPLY; a verb that answers refusals carries their resp numbers
 * in OMX_VERB_<VERB>_CODES_INIT. omx_clap_core_host_verbs() is the table of what each host owes each verb.
 */

#include <string.h>

#ifndef OMX_HOST_VERB_TYPES_H
#define OMX_HOST_VERB_TYPES_H
/* What a host owes a verb: required, answered as its grammar says; optional, answered or refused, never misread;
 * unsupported, refused on purpose. */
enum omx_host_verb_level {
  OMX_HOST_VERB_UNSUPPORTED = 0,
  OMX_HOST_VERB_OPTIONAL = 1,
  OMX_HOST_VERB_REQUIRED = 2,
};
/* The hosts of VERB_HOSTS, a column each of a verb's level, in this order. */
#define OMX_HOST_VERB_HOST_PLUGIN_HOSTD 0u
#define OMX_HOST_VERB_HOST_OMX_CLAP_HOST 1u
#define OMX_HOST_VERB_HOST_MOD_HOST 2u
#define OMX_HOST_VERB_HOST_COUNT 3u
#define OMX_HOST_VERB_HOSTS_INIT { "plugin-hostd", "omx-clap-host", "mod-host" }
struct omx_host_verb {
  const char *verb;
  const char *format;
  const char *reply;
  enum omx_host_verb_level level[OMX_HOST_VERB_HOST_COUNT];
};
#endif /* OMX_HOST_VERB_TYPES_H */

/* OMX_CLAP_CORE_SHARED_VERBS.trackInfo (data/verbs.json): The strip's name, colour and kind: <instance> <name> <#RRGGBB | -> [bus | return | master]; a string word is at most 255 bytes of UTF-8 with no control character, and any other word is -902. A CLAP plugin reads them through clap.track-info. */
#define OMX_VERB_TRACK_INFO "track_info"
#define OMX_VERB_TRACK_INFO_FMT "track_info %i %s %s ..."
#define OMX_VERB_TRACK_INFO_REPLY "resp 0"
#define OMX_VERB_TRACK_INFO_CODES_COUNT 2u
#define OMX_VERB_TRACK_INFO_CODES_INIT { -902, -3 } /* reply.host.invalid-operation, reply.host.instance-non-exists */

/* OMX_CLAP_CORE_SHARED_VERBS.remotePages (data/verbs.json): The plugin's remote-control pages, from clap.remote-controls; 0 for a plugin that has none. */
#define OMX_VERB_REMOTE_PAGES "remote_pages"
#define OMX_VERB_REMOTE_PAGES_FMT "remote_pages %i"
#define OMX_VERB_REMOTE_PAGES_REPLY "resp <count>"
#define OMX_VERB_REMOTE_PAGES_CODES_COUNT 2u
#define OMX_VERB_REMOTE_PAGES_CODES_INIT { -902, -3 } /* reply.host.invalid-operation, reply.host.instance-non-exists */

/* OMX_CLAP_CORE_SHARED_VERBS.remotePageGet (data/verbs.json): One page, 0 to count - 1: its section and name as quoted words, and each slot the symbol param_set takes, - for an empty one; -902 for a page that is not one. */
#define OMX_VERB_REMOTE_PAGE_GET "remote_page_get"
#define OMX_VERB_REMOTE_PAGE_GET_FMT "remote_page_get %i %i"
#define OMX_VERB_REMOTE_PAGE_GET_REPLY "resp 0 <page_id> <section> <page_name> <s1> <s2> <s3> <s4> <s5> <s6> <s7> <s8>"
#define OMX_VERB_REMOTE_PAGE_GET_CODES_COUNT 2u
#define OMX_VERB_REMOTE_PAGE_GET_CODES_INIT { -902, -3 } /* reply.host.invalid-operation, reply.host.instance-non-exists */

/* OMX_CLAP_CORE_SHARED_VERBS.paramInfo (data/verbs.json): What a parameter's value means: its unit, its scale (linear, log or stepped), its range, default and step, and the symbol it keeps across hosts and versions. */
#define OMX_VERB_PARAM_INFO "param_info"
#define OMX_VERB_PARAM_INFO_FMT "param_info %i %s"
#define OMX_VERB_PARAM_INFO_REPLY "resp 0 <unit> <scale> <min> <max> <default> <step> <stable_symbol>"
#define OMX_VERB_PARAM_INFO_CODES_COUNT 4u
#define OMX_VERB_PARAM_INFO_CODES_INIT { -103, -511, -3, -902 } /* reply.host.param-symbol-invalid, reply.host.no-param-contract, reply.host.instance-non-exists, reply.host.invalid-operation */

/* OMX_CLAP_CORE_SHARED_VERBS.pinExpect (data/verbs.json): The layout pin <scheme>:<sha256> the next add of the instance checks after init and before activate; plugin-hostd sends it to a worker of a format that checks layouts, and refuses the add with any other answer than resp 0. */
#define OMX_VERB_PIN_EXPECT "pin_expect"
#define OMX_VERB_PIN_EXPECT_FMT "pin_expect %i %s"
#define OMX_VERB_PIN_EXPECT_REPLY "resp 0"
#define OMX_VERB_PIN_EXPECT_CODES_COUNT 3u
#define OMX_VERB_PIN_EXPECT_CODES_INIT { -1, -902, -508 } /* reply.host.instance-invalid, reply.host.invalid-operation, reply.host.pin-absent */

/* OMX_CLAP_CORE_SHARED_VERBS.noteInput (data/verbs.json): Whether the instance takes notes: the name of its note input on its worker's jack client, - for a plugin that takes none. */
#define OMX_VERB_NOTE_INPUT "note_input"
#define OMX_VERB_NOTE_INPUT_FMT "note_input %i"
#define OMX_VERB_NOTE_INPUT_REPLY "resp 0 <port>"
#define OMX_VERB_NOTE_INPUT_CODES_COUNT 3u
#define OMX_VERB_NOTE_INPUT_CODES_INIT { -3, -902, -514 } /* reply.host.instance-non-exists, reply.host.invalid-operation, reply.host.notes-by-jack */

/* OMX_CLAP_CORE_SHARED_VERBS.noteOn (data/verbs.json): Play a key: <instance> <channel> <key> <velocity>, channel 0 to 15, key 0 to 127, velocity 1 to 127, a MIDI note on at the start of the worker's next cycle; -902 for a word out of its range. */
#define OMX_VERB_NOTE_ON "note_on"
#define OMX_VERB_NOTE_ON_FMT "note_on %i %i %i %i"
#define OMX_VERB_NOTE_ON_REPLY "resp 0"
#define OMX_VERB_NOTE_ON_CODES_COUNT 5u
#define OMX_VERB_NOTE_ON_CODES_INIT { -513, -514, -517, -3, -902 } /* reply.host.no-note-input, reply.host.notes-by-jack, reply.host.note-queue-full, reply.host.instance-non-exists, reply.host.invalid-operation */

/* OMX_CLAP_CORE_SHARED_VERBS.noteOff (data/verbs.json): Release a key: <instance> <channel> <key> <velocity>, velocity 0 to 127, a MIDI note off; answered as note_on is. */
#define OMX_VERB_NOTE_OFF "note_off"
#define OMX_VERB_NOTE_OFF_FMT "note_off %i %i %i %i"
#define OMX_VERB_NOTE_OFF_REPLY "resp 0"
#define OMX_VERB_NOTE_OFF_CODES_COUNT 5u
#define OMX_VERB_NOTE_OFF_CODES_INIT { -513, -514, -517, -3, -902 } /* reply.host.no-note-input, reply.host.notes-by-jack, reply.host.note-queue-full, reply.host.instance-non-exists, reply.host.invalid-operation */

/* OMX_CLAP_CORE_SHARED_VERBS.allNotesOff (data/verbs.json): Release every key of every channel, MIDI's all notes off (controller 123) on each; answered as note_on is. */
#define OMX_VERB_ALL_NOTES_OFF "all_notes_off"
#define OMX_VERB_ALL_NOTES_OFF_FMT "all_notes_off %i"
#define OMX_VERB_ALL_NOTES_OFF_REPLY "resp 0"
#define OMX_VERB_ALL_NOTES_OFF_CODES_COUNT 5u
#define OMX_VERB_ALL_NOTES_OFF_CODES_INIT { -513, -514, -517, -3, -902 } /* reply.host.no-note-input, reply.host.notes-by-jack, reply.host.note-queue-full, reply.host.instance-non-exists, reply.host.invalid-operation */

/* OMX_CLAP_HOST_VERBS.audioPorts (data/verbs.json): The instance's audio ports, inputs first and each side's main port first, each with the JACK ports that carry it; dry while the instance passes its input through. */
#define OMX_VERB_AUDIO_PORTS "audio_ports"
#define OMX_VERB_AUDIO_PORTS_FMT "audio_ports %i"
#define OMX_VERB_AUDIO_PORTS_REPLY "resp 0 [dry] <port> ..."
#define OMX_VERB_AUDIO_PORTS_CODES_COUNT 2u
#define OMX_VERB_AUDIO_PORTS_CODES_INIT { -3, -901 } /* reply.host.instance-non-exists, reply.host.memory-allocation */

/* OMX_CLAP_HOST_VERBS.hideExternalUi (data/verbs.json): Hide the plugin's own window that show_external_ui showed. */
#define OMX_VERB_HIDE_EXTERNAL_UI "hide_external_ui"
#define OMX_VERB_HIDE_EXTERNAL_UI_FMT "hide_external_ui %i"
#define OMX_VERB_HIDE_EXTERNAL_UI_REPLY "resp 0"
#define OMX_VERB_HIDE_EXTERNAL_UI_CODES_COUNT 1u
#define OMX_VERB_HIDE_EXTERNAL_UI_CODES_INIT { -3 } /* reply.host.instance-non-exists */

/* OMX_CLAP_HOST_VERBS.idleHold (data/verbs.json): The CPU idle hold as it stands; idle_hold <off | microseconds> changes its target at once and answers resp 0. */
#define OMX_VERB_IDLE_HOLD "idle_hold"
#define OMX_VERB_IDLE_HOLD_FMT "idle_hold ..."
#define OMX_VERB_IDLE_HOLD_REPLY "resp 0 <target> <held> <aggregate> <active> <code>"
#define OMX_VERB_IDLE_HOLD_CODES_COUNT 1u
#define OMX_VERB_IDLE_HOLD_CODES_INIT { -902 } /* reply.host.invalid-operation */

/* OMX_CLAP_HOST_VERBS.noteDrops (data/verbs.json): The note events the instance's blocks had no room for, of them the releases, the sysex past a block's pool, and the messages no dialect of the plugin carries. */
#define OMX_VERB_NOTE_DROPS "note_drops"
#define OMX_VERB_NOTE_DROPS_FMT "note_drops %i"
#define OMX_VERB_NOTE_DROPS_REPLY "resp 0 <notes> <releases> <sysex> <unmappable>"
#define OMX_VERB_NOTE_DROPS_CODES_COUNT 1u
#define OMX_VERB_NOTE_DROPS_CODES_INIT { -3 } /* reply.host.instance-non-exists */

/* OMX_CLAP_HOST_VERBS.noteOutDrops (data/verbs.json): The plugin's note output counters: the events the ring had no room for, those with no MIDI form, and the messages midi_out had no room for. */
#define OMX_VERB_NOTE_OUT_DROPS "note_out_drops"
#define OMX_VERB_NOTE_OUT_DROPS_FMT "note_out_drops %i"
#define OMX_VERB_NOTE_OUT_DROPS_REPLY "resp 0 <dropped> <unmapped> <midi_out_full>"
#define OMX_VERB_NOTE_OUT_DROPS_CODES_COUNT 1u
#define OMX_VERB_NOTE_OUT_DROPS_CODES_INIT { -3 } /* reply.host.instance-non-exists */

#define OMX_CLAP_CORE_HOST_VERB_COUNT 14u

/* Every verb, in the order of its items, with what each host owes it. */
static inline const struct omx_host_verb *omx_clap_core_host_verbs(void) {
  static const struct omx_host_verb table[OMX_CLAP_CORE_HOST_VERB_COUNT] = {
    { OMX_VERB_TRACK_INFO, OMX_VERB_TRACK_INFO_FMT, OMX_VERB_TRACK_INFO_REPLY, { OMX_HOST_VERB_OPTIONAL, OMX_HOST_VERB_REQUIRED, OMX_HOST_VERB_UNSUPPORTED } },
    { OMX_VERB_REMOTE_PAGES, OMX_VERB_REMOTE_PAGES_FMT, OMX_VERB_REMOTE_PAGES_REPLY, { OMX_HOST_VERB_OPTIONAL, OMX_HOST_VERB_REQUIRED, OMX_HOST_VERB_UNSUPPORTED } },
    { OMX_VERB_REMOTE_PAGE_GET, OMX_VERB_REMOTE_PAGE_GET_FMT, OMX_VERB_REMOTE_PAGE_GET_REPLY, { OMX_HOST_VERB_OPTIONAL, OMX_HOST_VERB_REQUIRED, OMX_HOST_VERB_UNSUPPORTED } },
    { OMX_VERB_PARAM_INFO, OMX_VERB_PARAM_INFO_FMT, OMX_VERB_PARAM_INFO_REPLY, { OMX_HOST_VERB_OPTIONAL, OMX_HOST_VERB_REQUIRED, OMX_HOST_VERB_UNSUPPORTED } },
    { OMX_VERB_PIN_EXPECT, OMX_VERB_PIN_EXPECT_FMT, OMX_VERB_PIN_EXPECT_REPLY, { OMX_HOST_VERB_UNSUPPORTED, OMX_HOST_VERB_REQUIRED, OMX_HOST_VERB_UNSUPPORTED } },
    { OMX_VERB_NOTE_INPUT, OMX_VERB_NOTE_INPUT_FMT, OMX_VERB_NOTE_INPUT_REPLY, { OMX_HOST_VERB_OPTIONAL, OMX_HOST_VERB_REQUIRED, OMX_HOST_VERB_UNSUPPORTED } },
    { OMX_VERB_NOTE_ON, OMX_VERB_NOTE_ON_FMT, OMX_VERB_NOTE_ON_REPLY, { OMX_HOST_VERB_OPTIONAL, OMX_HOST_VERB_REQUIRED, OMX_HOST_VERB_UNSUPPORTED } },
    { OMX_VERB_NOTE_OFF, OMX_VERB_NOTE_OFF_FMT, OMX_VERB_NOTE_OFF_REPLY, { OMX_HOST_VERB_OPTIONAL, OMX_HOST_VERB_REQUIRED, OMX_HOST_VERB_UNSUPPORTED } },
    { OMX_VERB_ALL_NOTES_OFF, OMX_VERB_ALL_NOTES_OFF_FMT, OMX_VERB_ALL_NOTES_OFF_REPLY, { OMX_HOST_VERB_OPTIONAL, OMX_HOST_VERB_REQUIRED, OMX_HOST_VERB_UNSUPPORTED } },
    { OMX_VERB_AUDIO_PORTS, OMX_VERB_AUDIO_PORTS_FMT, OMX_VERB_AUDIO_PORTS_REPLY, { OMX_HOST_VERB_UNSUPPORTED, OMX_HOST_VERB_REQUIRED, OMX_HOST_VERB_UNSUPPORTED } },
    { OMX_VERB_HIDE_EXTERNAL_UI, OMX_VERB_HIDE_EXTERNAL_UI_FMT, OMX_VERB_HIDE_EXTERNAL_UI_REPLY, { OMX_HOST_VERB_UNSUPPORTED, OMX_HOST_VERB_REQUIRED, OMX_HOST_VERB_UNSUPPORTED } },
    { OMX_VERB_IDLE_HOLD, OMX_VERB_IDLE_HOLD_FMT, OMX_VERB_IDLE_HOLD_REPLY, { OMX_HOST_VERB_UNSUPPORTED, OMX_HOST_VERB_REQUIRED, OMX_HOST_VERB_UNSUPPORTED } },
    { OMX_VERB_NOTE_DROPS, OMX_VERB_NOTE_DROPS_FMT, OMX_VERB_NOTE_DROPS_REPLY, { OMX_HOST_VERB_UNSUPPORTED, OMX_HOST_VERB_REQUIRED, OMX_HOST_VERB_UNSUPPORTED } },
    { OMX_VERB_NOTE_OUT_DROPS, OMX_VERB_NOTE_OUT_DROPS_FMT, OMX_VERB_NOTE_OUT_DROPS_REPLY, { OMX_HOST_VERB_UNSUPPORTED, OMX_HOST_VERB_REQUIRED, OMX_HOST_VERB_UNSUPPORTED } },
  };
  return table;
}

/* The verb whose word is `verb`, or NULL. */
static inline const struct omx_host_verb *omx_clap_core_host_verb_find(const char *verb) {
  const struct omx_host_verb *t = omx_clap_core_host_verbs();
  for (unsigned i = 0; i < OMX_CLAP_CORE_HOST_VERB_COUNT; i++)
    if (!strcmp(t[i].verb, verb)) return &t[i];
  return NULL;
}

/* What the host at column `host` (an OMX_HOST_VERB_HOST_* index) owes `verb`: unsupported for a verb not here. */
static inline enum omx_host_verb_level omx_clap_core_host_verb_level(const char *verb, unsigned host) {
  const struct omx_host_verb *v = omx_clap_core_host_verb_find(verb);
  return v && host < OMX_HOST_VERB_HOST_COUNT ? v->level[host] : OMX_HOST_VERB_UNSUPPORTED;
}

#endif /* OMX_CLAP_CORE_HOST_VERBS_H */
