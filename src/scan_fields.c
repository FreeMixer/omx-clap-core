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


/* omx-clap-scan --fields: the field dump of one plugin, every field it states as a host reads it, one record a line,
 * `<scope>\t<field>\t<value>`, as plugin-hostd's format tables name the fields (plugin-hostd docs/formats.md, "The field
 * dump"). A CLAP plugin is read through the core's instance, created and initialised, and activated once at 48 kHz when
 * it offers clap.latency or clap.voice-info, which answer only while active; an LV2 plugin through the adapter's own
 * lilv world, its binary never opened. The dump is made in a child and written only when it is whole: a plugin that
 * crashes, hangs or ends its process early leaves nothing on stdout, and whatever the plugin prints goes to stderr. */

/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "clap_host.h"
#include "hosted_stage_limits.h"
#include "lv2_core.h"
#include "lv2_locate.h"
#include "scan_fields.h"
#include "scan_panel.h"


/*
************************************************************************************************************************
*           LOCAL DEFINES
************************************************************************************************************************
*/

#define FIELDS_VERSION      1
/* the seconds a plugin gets to answer the dump, as a listing gives a file; OMX_CLAP_SCAN_FIELDS_TIMEOUT (1 to 3600)
 * sets another, as the scanner's test does to see a hang end */
#define FIELDS_TIMEOUT_S 30
#define FIELDS_TIMEOUT_MAX_S 3600
#define FIELDS_RATE         48000.0
#define FIELDS_MAX_FRAMES   1024u
#define FIELDS_ENUM_MAX     1024    ///< the most values of one enumerated parameter asked for a text


/*
************************************************************************************************************************
*           LOCAL DATA TYPES
************************************************************************************************************************
*/

typedef struct NAMED_T {
    const char *name;
    const char *value;
} named_t;

typedef struct BIT_NAME_T {
    uint32_t bit;
    const char *name;
} bit_name_t;

typedef struct PREFIX_T {
    const char *prefix;
    const char *ns;
    const char *spec;
} prefix_t;

struct lv2_find {
    const char *uri;
    char bundle[PATH_MAX];
};


/*
************************************************************************************************************************
*           LOCAL GLOBAL VARIABLES
************************************************************************************************************************
*/

/* the extension ids a plugin is asked for, each by its TERM id (omx-contract data/formats/clap) */
static const named_t g_extensions[] = {
    { "CLAP_EXT_AMBISONIC", "clap.ambisonic/3" },
    { "CLAP_EXT_AMBISONIC_COMPAT", "clap.ambisonic.draft/3" },
    { "CLAP_EXT_AUDIO_PORTS", "clap.audio-ports" },
    { "CLAP_EXT_AUDIO_PORTS_ACTIVATION", "clap.audio-ports-activation/2" },
    { "CLAP_EXT_AUDIO_PORTS_ACTIVATION_COMPAT", "clap.audio-ports-activation/draft-2" },
    { "CLAP_EXT_AUDIO_PORTS_CONFIG", "clap.audio-ports-config" },
    { "CLAP_EXT_AUDIO_PORTS_CONFIG_INFO", "clap.audio-ports-config-info/1" },
    { "CLAP_EXT_AUDIO_PORTS_CONFIG_INFO_COMPAT", "clap.audio-ports-config-info/draft-0" },
    { "CLAP_EXT_BACKGROUND_ACTIVATION", "clap.background-activation/1" },
    { "CLAP_EXT_BACKGROUND_PROGRESS", "clap.background-progress/1" },
    { "CLAP_EXT_BACKGROUND_STATE_CONTEXT", "clap.background-state-context/1" },
    { "CLAP_EXT_CONFIGURABLE_AUDIO_PORTS", "clap.configurable-audio-ports/1" },
    { "CLAP_EXT_CONFIGURABLE_AUDIO_PORTS_COMPAT", "clap.configurable-audio-ports.draft1" },
    { "CLAP_EXT_CONTEXT_MENU", "clap.context-menu/1" },
    { "CLAP_EXT_CONTEXT_MENU_COMPAT", "clap.context-menu.draft/0" },
    { "CLAP_EXT_EVENT_REGISTRY", "clap.event-registry" },
    { "CLAP_EXT_EXTENSIBLE_AUDIO_PORTS", "clap.extensible-audio-ports/1" },
    { "CLAP_EXT_FLUSH_EVENTS", "clap.flush-events/1" },
    { "CLAP_EXT_GAIN_ADJUSTMENT_METERING", "clap.gain-adjustment-metering/0" },
    { "CLAP_EXT_GUI", "clap.gui" },
    { "CLAP_EXT_LATENCY", "clap.latency" },
    { "CLAP_EXT_LOG", "clap.log" },
    { "CLAP_EXT_MINI_CURVE_DISPLAY", "clap.mini-curve-display/3" },
    { "CLAP_EXT_NOTE_NAME", "clap.note-name" },
    { "CLAP_EXT_NOTE_PORTS", "clap.note-ports" },
    { "CLAP_EXT_OCTAVE_NUMBER", "clap.octave-number/1" },
    { "CLAP_EXT_PARAMS", "clap.params" },
    { "CLAP_EXT_PARAMS_ORIGIN", "clap.params-origin/1" },
    { "CLAP_EXT_PARAM_HOVERED", "clap.param-hovered/1" },
    { "CLAP_EXT_PARAM_INDICATION", "clap.param-indication/4" },
    { "CLAP_EXT_PARAM_INDICATION_COMPAT", "clap.param-indication.draft/4" },
    { "CLAP_EXT_POSIX_FD_SUPPORT", "clap.posix-fd-support" },
    { "CLAP_EXT_PRESET_LOAD", "clap.preset-load/2" },
    { "CLAP_EXT_PRESET_LOAD_COMPAT", "clap.preset-load.draft/2" },
    { "CLAP_EXT_PROJECT_LOCATION", "clap.project-location/2" },
    { "CLAP_EXT_REMOTE_CONTROLS", "clap.remote-controls/2" },
    { "CLAP_EXT_REMOTE_CONTROLS_COMPAT", "clap.remote-controls.draft/2" },
    { "CLAP_EXT_RENDER", "clap.render" },
    { "CLAP_EXT_RESOURCE_DIRECTORY", "clap.resource-directory/1" },
    { "CLAP_EXT_SCRATCH_MEMORY", "clap.scratch-memory/1" },
    { "CLAP_EXT_STATE", "clap.state" },
    { "CLAP_EXT_STATE_CONTEXT", "clap.state-context/2" },
    { "CLAP_EXT_SURROUND", "clap.surround/4" },
    { "CLAP_EXT_SURROUND_COMPAT", "clap.surround.draft/4" },
    { "CLAP_EXT_TAIL", "clap.tail" },
    { "CLAP_EXT_THREAD_CHECK", "clap.thread-check" },
    { "CLAP_EXT_THREAD_POOL", "clap.thread-pool" },
    { "CLAP_EXT_TIMER_SUPPORT", "clap.timer-support" },
    { "CLAP_EXT_TRACK_INFO", "clap.track-info/1" },
    { "CLAP_EXT_TRACK_INFO_COMPAT", "clap.track-info.draft/1" },
    { "CLAP_EXT_TRANSPORT_CONTROL", "clap.transport-control/2" },
    { "CLAP_EXT_TRIGGERS", "clap.triggers/1" },
    { "CLAP_EXT_TUNING", "clap.tuning/2" },
    { "CLAP_EXT_UNDO", "clap.undo/4" },
    { "CLAP_EXT_UNDO_CONTEXT", "clap.undo_context/4" },
    { "CLAP_EXT_UNDO_DELTA", "clap.undo_delta/4" },
    { "CLAP_EXT_VOICE_INFO", "clap.voice-info" },
    { "CLAP_EXT_WEBVIEW", "clap.webview/3" },
};

/* the plugin features the CLAP headers define, each by its TERM id */
static const named_t g_features[] = {
    { "CLAP_PLUGIN_FEATURE_AMBISONIC", "ambisonic" },
    { "CLAP_PLUGIN_FEATURE_ANALYZER", "analyzer" },
    { "CLAP_PLUGIN_FEATURE_AUDIO_EFFECT", "audio-effect" },
    { "CLAP_PLUGIN_FEATURE_CHORUS", "chorus" },
    { "CLAP_PLUGIN_FEATURE_COMPRESSOR", "compressor" },
    { "CLAP_PLUGIN_FEATURE_DEESSER", "de-esser" },
    { "CLAP_PLUGIN_FEATURE_DELAY", "delay" },
    { "CLAP_PLUGIN_FEATURE_DISTORTION", "distortion" },
    { "CLAP_PLUGIN_FEATURE_DRUM", "drum" },
    { "CLAP_PLUGIN_FEATURE_DRUM_MACHINE", "drum-machine" },
    { "CLAP_PLUGIN_FEATURE_EQUALIZER", "equalizer" },
    { "CLAP_PLUGIN_FEATURE_EXPANDER", "expander" },
    { "CLAP_PLUGIN_FEATURE_FILTER", "filter" },
    { "CLAP_PLUGIN_FEATURE_FLANGER", "flanger" },
    { "CLAP_PLUGIN_FEATURE_FREQUENCY_SHIFTER", "frequency-shifter" },
    { "CLAP_PLUGIN_FEATURE_GATE", "gate" },
    { "CLAP_PLUGIN_FEATURE_GLITCH", "glitch" },
    { "CLAP_PLUGIN_FEATURE_GRANULAR", "granular" },
    { "CLAP_PLUGIN_FEATURE_INSTRUMENT", "instrument" },
    { "CLAP_PLUGIN_FEATURE_LIMITER", "limiter" },
    { "CLAP_PLUGIN_FEATURE_MASTERING", "mastering" },
    { "CLAP_PLUGIN_FEATURE_MIXING", "mixing" },
    { "CLAP_PLUGIN_FEATURE_MONO", "mono" },
    { "CLAP_PLUGIN_FEATURE_MULTI_EFFECTS", "multi-effects" },
    { "CLAP_PLUGIN_FEATURE_NOTE_DETECTOR", "note-detector" },
    { "CLAP_PLUGIN_FEATURE_NOTE_EFFECT", "note-effect" },
    { "CLAP_PLUGIN_FEATURE_PHASER", "phaser" },
    { "CLAP_PLUGIN_FEATURE_PHASE_VOCODER", "phase-vocoder" },
    { "CLAP_PLUGIN_FEATURE_PITCH_CORRECTION", "pitch-correction" },
    { "CLAP_PLUGIN_FEATURE_PITCH_SHIFTER", "pitch-shifter" },
    { "CLAP_PLUGIN_FEATURE_RESTORATION", "restoration" },
    { "CLAP_PLUGIN_FEATURE_REVERB", "reverb" },
    { "CLAP_PLUGIN_FEATURE_SAMPLER", "sampler" },
    { "CLAP_PLUGIN_FEATURE_STEREO", "stereo" },
    { "CLAP_PLUGIN_FEATURE_SURROUND", "surround" },
    { "CLAP_PLUGIN_FEATURE_SYNTHESIZER", "synthesizer" },
    { "CLAP_PLUGIN_FEATURE_TRANSIENT_SHAPER", "transient-shaper" },
    { "CLAP_PLUGIN_FEATURE_TREMOLO", "tremolo" },
    { "CLAP_PLUGIN_FEATURE_UTILITY", "utility" },
};

/* the port types, each by its TERM id */
static const named_t g_port_types[] = {
    { "CLAP_PORT_AMBISONIC", "ambisonic" },
    { "CLAP_PORT_MONO", "mono" },
    { "CLAP_PORT_STEREO", "stereo" },
    { "CLAP_PORT_SURROUND", "surround" },
};

static const bit_name_t g_param_flags[] = {
    { CLAP_PARAM_IS_STEPPED, "CLAP_PARAM_IS_STEPPED" },
    { CLAP_PARAM_IS_PERIODIC, "CLAP_PARAM_IS_PERIODIC" },
    { CLAP_PARAM_IS_HIDDEN, "CLAP_PARAM_IS_HIDDEN" },
    { CLAP_PARAM_IS_READONLY, "CLAP_PARAM_IS_READONLY" },
    { CLAP_PARAM_IS_BYPASS, "CLAP_PARAM_IS_BYPASS" },
    { CLAP_PARAM_IS_AUTOMATABLE, "CLAP_PARAM_IS_AUTOMATABLE" },
    { CLAP_PARAM_IS_AUTOMATABLE_PER_NOTE_ID, "CLAP_PARAM_IS_AUTOMATABLE_PER_NOTE_ID" },
    { CLAP_PARAM_IS_AUTOMATABLE_PER_KEY, "CLAP_PARAM_IS_AUTOMATABLE_PER_KEY" },
    { CLAP_PARAM_IS_AUTOMATABLE_PER_CHANNEL, "CLAP_PARAM_IS_AUTOMATABLE_PER_CHANNEL" },
    { CLAP_PARAM_IS_AUTOMATABLE_PER_PORT, "CLAP_PARAM_IS_AUTOMATABLE_PER_PORT" },
    { CLAP_PARAM_IS_MODULATABLE, "CLAP_PARAM_IS_MODULATABLE" },
    { CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID, "CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID" },
    { CLAP_PARAM_IS_MODULATABLE_PER_KEY, "CLAP_PARAM_IS_MODULATABLE_PER_KEY" },
    { CLAP_PARAM_IS_MODULATABLE_PER_CHANNEL, "CLAP_PARAM_IS_MODULATABLE_PER_CHANNEL" },
    { CLAP_PARAM_IS_MODULATABLE_PER_PORT, "CLAP_PARAM_IS_MODULATABLE_PER_PORT" },
    { CLAP_PARAM_REQUIRES_PROCESS, "CLAP_PARAM_REQUIRES_PROCESS" },
    { CLAP_PARAM_IS_ENUM, "CLAP_PARAM_IS_ENUM" },
    { 0, NULL },
};

static const bit_name_t g_audio_port_flags[] = {
    { CLAP_AUDIO_PORT_IS_MAIN, "CLAP_AUDIO_PORT_IS_MAIN" },
    { CLAP_AUDIO_PORT_SUPPORTS_64BITS, "CLAP_AUDIO_PORT_SUPPORTS_64BITS" },
    { CLAP_AUDIO_PORT_PREFERS_64BITS, "CLAP_AUDIO_PORT_PREFERS_64BITS" },
    { CLAP_AUDIO_PORT_REQUIRES_COMMON_SAMPLE_SIZE, "CLAP_AUDIO_PORT_REQUIRES_COMMON_SAMPLE_SIZE" },
    { 0, NULL },
};

static const bit_name_t g_note_dialects[] = {
    { CLAP_NOTE_DIALECT_CLAP, "CLAP_NOTE_DIALECT_CLAP" },
    { CLAP_NOTE_DIALECT_MIDI, "CLAP_NOTE_DIALECT_MIDI" },
    { CLAP_NOTE_DIALECT_MIDI_MPE, "CLAP_NOTE_DIALECT_MIDI_MPE" },
    { CLAP_NOTE_DIALECT_MIDI2, "CLAP_NOTE_DIALECT_MIDI2" },
    { 0, NULL },
};

static const bit_name_t g_voice_flags[] = {
    { CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES, "CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES" },
    { 0, NULL },
};

/* the extensions of this tree's own, by their id: no TERM names them */
static const char *const g_omx_extensions[] = { OMX_CLAP_EXT_DECLARATION, OMX_CLAP_EXT_PARAM_CONTRACT, OMX_CLAP_EXT_PANEL_DESCRIPTION,
                                                OMX_CLAP_EXT_RESOURCE_HINTS_DRAFT, NULL };

/* the prefixes a URI is written under, and the specification each LV2 one is (written <prefix>:<spec>.lv2) */
static const prefix_t g_prefixes[] = {
    { "lv2", "http://lv2plug.in/ns/lv2core#", "core" },
    { "atom", "http://lv2plug.in/ns/ext/atom#", "atom" },
    { "bufsz", "http://lv2plug.in/ns/ext/buf-size#", "buf-size" },
    { "da", "http://lv2plug.in/ns/ext/data-access#", "data-access" },
    { "dman", "http://lv2plug.in/ns/ext/dynmanifest#", "dynmanifest" },
    { "ev", "http://lv2plug.in/ns/ext/event#", "event" },
    { "ia", "http://lv2plug.in/ns/ext/instance-access#", "instance-access" },
    { "log", "http://lv2plug.in/ns/ext/log#", "log" },
    { "midi", "http://lv2plug.in/ns/ext/midi#", "midi" },
    { "morph", "http://lv2plug.in/ns/ext/morph#", "morph" },
    { "opts", "http://lv2plug.in/ns/ext/options#", "options" },
    { "param", "http://lv2plug.in/ns/ext/parameters#", "parameters" },
    { "patch", "http://lv2plug.in/ns/ext/patch#", "patch" },
    { "pg", "http://lv2plug.in/ns/ext/port-groups#", "port-groups" },
    { "pprops", "http://lv2plug.in/ns/ext/port-props#", "port-props" },
    { "pset", "http://lv2plug.in/ns/ext/presets#", "presets" },
    { "rsz", "http://lv2plug.in/ns/ext/resize-port#", "resize-port" },
    { "state", "http://lv2plug.in/ns/ext/state#", "state" },
    { "time", "http://lv2plug.in/ns/ext/time#", "time" },
    { "ui", "http://lv2plug.in/ns/extensions/ui#", "ui" },
    { "units", "http://lv2plug.in/ns/extensions/units#", "units" },
    { "urimap", "http://lv2plug.in/ns/ext/uri-map#", "uri-map" },
    { "urid", "http://lv2plug.in/ns/ext/urid#", "urid" },
    { "work", "http://lv2plug.in/ns/ext/worker#", "worker" },
    { "rdf", "http://www.w3.org/1999/02/22-rdf-syntax-ns#", NULL },
    { "rdfs", "http://www.w3.org/2000/01/rdf-schema#", NULL },
    { "doap", "http://usefulinc.com/ns/doap#", NULL },
    { "foaf", "http://xmlns.com/foaf/0.1/", NULL },
};

/* the fields asked of an LV2 plugin and of each port: every property TERM (omx-contract data/formats/lv2) but lv2:port and
 * lv2:index, the scopes themselves, and the RDF and DOAP predicates the table reads; 1: a resource it names is written by
 * its label (a port group) */
static const struct { const char *curie; int by_label; } g_lv2_fields[] = {
    { "atom:beatTime", 0 },
    { "atom:bufferType", 0 },
    { "atom:cType", 0 },
    { "atom:childType", 0 },
    { "atom:frameTime", 0 },
    { "atom:supports", 0 },
    { "doap:license", 0 },
    { "doap:maintainer", 0 },
    { "doap:name", 0 },
    { "ev:generatesTimeStamp", 0 },
    { "ev:inheritsEvent", 0 },
    { "ev:inheritsTimeStamp", 0 },
    { "ev:supportsEvent", 0 },
    { "ev:supportsTimeStamp", 0 },
    { "lv2:appliesTo", 0 },
    { "lv2:binary", 0 },
    { "lv2:default", 0 },
    { "lv2:designation", 0 },
    { "lv2:documentation", 0 },
    { "lv2:enabled", 0 },
    { "lv2:extensionData", 0 },
    { "lv2:freeWheeling", 0 },
    { "lv2:latency", 0 },
    { "lv2:maximum", 0 },
    { "lv2:microVersion", 0 },
    { "lv2:minimum", 0 },
    { "lv2:minorVersion", 0 },
    { "lv2:name", 0 },
    { "lv2:optionalFeature", 0 },
    { "lv2:portProperty", 0 },
    { "lv2:project", 0 },
    { "lv2:prototype", 0 },
    { "lv2:requiredFeature", 0 },
    { "lv2:scalePoint", 0 },
    { "lv2:shortName", 0 },
    { "lv2:symbol", 0 },
    { "midi:benderValue", 0 },
    { "midi:binding", 0 },
    { "midi:byteNumber", 0 },
    { "midi:channel", 0 },
    { "midi:chunk", 0 },
    { "midi:controllerNumber", 0 },
    { "midi:controllerValue", 0 },
    { "midi:noteNumber", 0 },
    { "midi:pressure", 0 },
    { "midi:programNumber", 0 },
    { "midi:property", 0 },
    { "midi:songNumber", 0 },
    { "midi:songPosition", 0 },
    { "midi:status", 0 },
    { "midi:statusMask", 0 },
    { "midi:velocity", 0 },
    { "morph:supportsType", 0 },
    { "opts:requiredOption", 0 },
    { "opts:supportedOption", 0 },
    { "patch:accept", 0 },
    { "patch:add", 0 },
    { "patch:body", 0 },
    { "patch:context", 0 },
    { "patch:destination", 0 },
    { "patch:property", 0 },
    { "patch:readable", 0 },
    { "patch:remove", 0 },
    { "patch:request", 0 },
    { "patch:sequenceNumber", 0 },
    { "patch:subject", 0 },
    { "patch:value", 0 },
    { "patch:writable", 0 },
    { "pg:element", 0 },
    { "pg:group", 1 },
    { "pg:harmonicDegree", 0 },
    { "pg:harmonicIndex", 0 },
    { "pg:letterCode", 0 },
    { "pg:mainInput", 1 },
    { "pg:mainOutput", 1 },
    { "pg:sideChainOf", 1 },
    { "pg:source", 1 },
    { "pg:subGroupOf", 1 },
    { "pprops:displayPriority", 0 },
    { "pprops:rangeSteps", 0 },
    { "pset:bank", 0 },
    { "pset:preset", 0 },
    { "pset:value", 0 },
    { "rdf:type", 0 },
    { "rdfs:comment", 0 },
    { "rdfs:seeAlso", 0 },
    { "rsz:asLargeAs", 0 },
    { "rsz:minimumSize", 0 },
    { "state:state", 0 },
    { "time:bar", 0 },
    { "time:barBeat", 0 },
    { "time:beat", 0 },
    { "time:beatUnit", 0 },
    { "time:beatsPerBar", 0 },
    { "time:beatsPerMinute", 0 },
    { "time:frame", 0 },
    { "time:framesPerSecond", 0 },
    { "time:position", 0 },
    { "time:speed", 0 },
    { "ui:binary", 0 },
    { "ui:notifyType", 0 },
    { "ui:plugin", 0 },
    { "ui:portIndex", 0 },
    { "ui:portNotification", 0 },
    { "ui:protocol", 0 },
    { "ui:ui", 0 },
    { "ui:updateRate", 0 },
    { "ui:windowTitle", 0 },
    { "units:conversion", 0 },
    { "units:factor", 0 },
    { "units:prefixConversion", 0 },
    { "units:render", 0 },
    { "units:symbol", 0 },
    { "units:to", 0 },
    { "units:unit", 0 },
};

static FILE *g_out;     ///< the dump, held in memory until it is whole


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS: THE RECORDS
************************************************************************************************************************
*/

/* a text as one column: a TAB or a newline in it becomes a space */
static void put_column(const char *s)
{
    for (; *s; s++)
        fputc(*s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s, g_out);
}

static void record(const char *scope, const char *field, const char *value)
{
    put_column(scope);
    fputc('\t', g_out);
    put_column(field);
    fputc('\t', g_out);
    put_column(value);
    fputc('\n', g_out);
}

/* a text the plugin states; an empty or absent one is not stated */
static void record_text(const char *scope, const char *field, const char *value)
{
    if (value && value[0])
        record(scope, field, value);
}

static void record_number(const char *scope, const char *field, double value)
{
    char text[64];

    snprintf(text, sizeof(text), "%.17g", value);
    record(scope, field, text);
}

/* one record per bit set that the table names */
static void record_bits(const char *scope, const char *field, uint32_t bits, const bit_name_t *names)
{
    for (; names->name; names++)
        if (bits & names->bit)
            record(scope, field, names->name);
}

/* a value by the TERM id that names it, as it is when none does */
static const char *named(const named_t *table, size_t n, const char *value)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (strcmp(table[i].value, value) == 0)
            return table[i].name;
    return value;
}

#define NAMED(table, value) named(table, sizeof(table) / sizeof(table[0]), value)


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS: CLAP
************************************************************************************************************************
*/

static void clap_params(const struct omx_clap_instance *in)
{
    const clap_plugin_params_t *params = in->params;
    const uint32_t count = params ? params->count(in->plugin) : 0;
    uint32_t i;

    for (i = 0; i < count; i++)
    {
        clap_param_info_t info;
        char scope[32];

        memset(&info, 0, sizeof(info));
        if (!params->get_info(in->plugin, i, &info))
            continue;
        info.name[sizeof(info.name) - 1] = '\0';
        info.module[sizeof(info.module) - 1] = '\0';
        snprintf(scope, sizeof(scope), "param:%u", i);
        record_number(scope, "clap_param_info.id", info.id);
        record_text(scope, "clap_param_info.name", info.name);
        record_text(scope, "clap_param_info.module", info.module);
        record_number(scope, "clap_param_info.min_value", info.min_value);
        record_number(scope, "clap_param_info.max_value", info.max_value);
        record_number(scope, "clap_param_info.default_value", info.default_value);
        record_bits(scope, "clap_param_info.flags", info.flags, g_param_flags);
        // an enumeration's values, each with the text the plugin gives it: what the host lists it by
        if ((info.flags & CLAP_PARAM_IS_ENUM) && (info.flags & CLAP_PARAM_IS_STEPPED) && params->value_to_text &&
            isfinite(info.min_value) && isfinite(info.max_value) && info.max_value - info.min_value < FIELDS_ENUM_MAX)
        {
            double v;

            for (v = ceil(info.min_value); v <= info.max_value; v += 1.0)
            {
                char text[256], point[320];

                text[0] = '\0';
                if (!params->value_to_text(in->plugin, info.id, v, text, sizeof(text)))
                    continue;
                text[sizeof(text) - 1] = '\0';
                snprintf(point, sizeof(point), "%.17g %s", v, text);
                record(scope, "clap_plugin_params.value_to_text", point);
            }
        }
    }
}

static void clap_audio_ports(const struct omx_clap_instance *in, bool is_input)
{
    const clap_plugin_audio_ports_t *ports = in->audio_ports;
    const uint32_t count = ports ? ports->count(in->plugin, is_input) : 0;
    uint32_t i;

    for (i = 0; i < count; i++)
    {
        clap_audio_port_info_t info;
        char scope[32];

        memset(&info, 0, sizeof(info));
        info.in_place_pair = CLAP_INVALID_ID;
        if (!ports->get(in->plugin, i, is_input, &info))
            continue;
        info.name[sizeof(info.name) - 1] = '\0';
        snprintf(scope, sizeof(scope), "%s:%u", is_input ? "audio_in" : "audio_out", i);
        record_number(scope, "clap_audio_port_info.id", info.id);
        record_text(scope, "clap_audio_port_info.name", info.name);
        record_number(scope, "clap_audio_port_info.channel_count", info.channel_count);
        record_bits(scope, "clap_audio_port_info.flags", info.flags, g_audio_port_flags);
        if (info.port_type)
            record_text(scope, "clap_audio_port_info.port_type", NAMED(g_port_types, info.port_type));
        if (info.in_place_pair != CLAP_INVALID_ID)
            record_number(scope, "clap_audio_port_info.in_place_pair", info.in_place_pair);
    }
}

static void clap_note_ports(const struct omx_clap_instance *in, bool is_input)
{
    const clap_plugin_note_ports_t *ports = in->note_ports;
    const uint32_t count = ports ? ports->count(in->plugin, is_input) : 0;
    uint32_t i;

    for (i = 0; i < count; i++)
    {
        clap_note_port_info_t info;
        char scope[32];

        memset(&info, 0, sizeof(info));
        if (!ports->get(in->plugin, i, is_input, &info))
            continue;
        info.name[sizeof(info.name) - 1] = '\0';
        snprintf(scope, sizeof(scope), "%s:%u", is_input ? "note_in" : "note_out", i);
        record_number(scope, "clap_note_port_info.id", info.id);
        record_text(scope, "clap_note_port_info.name", info.name);
        record_bits(scope, "clap_note_port_info.supported_dialects", info.supported_dialects, g_note_dialects);
        record_bits(scope, "clap_note_port_info.preferred_dialect", info.preferred_dialect, g_note_dialects);
    }
}

/* a code of the panel description by its enumerator in omx_panel_description.h, as its number when it names none */
static void record_code(const char *scope, const char *field, const char *name, uint32_t code)
{
    if (name)
        record(scope, field, name);
    else
        record_number(scope, field, code);
}

/* a parameter or meter id of the panel description; none is not stated */
static void record_id(const char *scope, const char *field, uint32_t id)
{
    if (id != CLAP_INVALID_ID)
        record_number(scope, field, id);
}

/* each table by omx-plugins' word for it */
#define PANEL_TABLE_WORD(code, name, id) [code] = name,
static const char *const g_panel_tables[SCAN_PANEL_TABLE_COUNT] = { OMX_CLAP_PANEL_TABLES(PANEL_TABLE_WORD) };
#undef PANEL_TABLE_WORD

/* the panel description the plugin publishes (org.openmixer.panel-description/1): the family and the fingerprint under
 * panel, each table's entry under <table>:<index in the plugin's table>, its fields as omx_panel_description.h names them;
 * no record when the plugin serves none */
static void clap_panel(const struct omx_clap_instance *in)
{
    struct scan_panel p;
    char scope[48], hex[24], refusal[80];
    uint32_t i;

    if (scan_panel_read(in->plugin, &p) != 1)
        return;
    if (p.has_family)
        record_code("panel", "omx_clap_plugin_panel_description.family", scan_panel_family_enum(p.family), p.family);
    if (p.has_fingerprint)
        record_text("panel", "omx_clap_plugin_panel_description.fingerprint", p.fingerprint);
    // a table refused for its count: the table, the count the plugin answered and the maximum, and no entry of it
    for (i = 0; i < SCAN_PANEL_TABLE_COUNT; i++)
        if (p.table[i].refused)
        {
            snprintf(refusal, sizeof(refusal), "%s %u refused, past %u", g_panel_tables[i], p.table[i].refused,
                     OMX_CLAP_PANEL_DESCRIPTION_MAX_ENTRIES);
            record("panel", "omx_clap_plugin_panel_description.count", refusal);
        }
    for (i = 0; i < p.table[SCAN_PANEL_SECTIONS].n; i++)
    {
        const omx_clap_panel_description_section_t *s = SCAN_PANEL_ROW(&p, SCAN_PANEL_SECTIONS, omx_clap_panel_description_section_t, i);

        snprintf(scope, sizeof(scope), "section:%u", p.table[SCAN_PANEL_SECTIONS].index[i]);
        record_text(scope, "omx_clap_panel_section.key", s->key);
        record_text(scope, "omx_clap_panel_section.label_key", s->label_key);
        record_text(scope, "omx_clap_panel_section.label", s->label);
    }
    for (i = 0; i < p.table[SCAN_PANEL_CONTROLS].n; i++)
    {
        const omx_clap_panel_description_control_t *c = SCAN_PANEL_ROW(&p, SCAN_PANEL_CONTROLS, omx_clap_panel_description_control_t, i);

        snprintf(scope, sizeof(scope), "control:%u", p.table[SCAN_PANEL_CONTROLS].index[i]);
        record_id(scope, "omx_clap_panel_control.param_id", c->param_id);
        record_number(scope, "omx_clap_panel_control.section", c->section);
        record_code(scope, "omx_clap_panel_control.widget", scan_panel_widget_enum(c->widget), c->widget);
        record_text(scope, "omx_clap_panel_control.label_key", c->label_key);
        record_text(scope, "omx_clap_panel_control.label", c->label);
        // the visibility rule: the parameter it follows and, in hex as the canonical serialisation writes it, its mask
        if (c->visible_param_id != CLAP_INVALID_ID)
        {
            record_number(scope, "omx_clap_panel_control.visible_param_id", c->visible_param_id);
            snprintf(hex, sizeof(hex), "%llx", (unsigned long long)c->visible_mask);
            record(scope, "omx_clap_panel_control.visible_mask", hex);
        }
    }
    for (i = 0; i < p.table[SCAN_PANEL_ROLES].n; i++)
    {
        const omx_clap_panel_description_role_t *r = SCAN_PANEL_ROW(&p, SCAN_PANEL_ROLES, omx_clap_panel_description_role_t, i);

        snprintf(scope, sizeof(scope), "role:%u", p.table[SCAN_PANEL_ROLES].index[i]);
        record_text(scope, "omx_clap_panel_role.role", r->role);
        record_number(scope, "omx_clap_panel_role.index", r->index);
        record_id(scope, "omx_clap_panel_role.param_id", r->param_id);
    }
    for (i = 0; i < p.table[SCAN_PANEL_DISPLAYS].n; i++)
    {
        const omx_clap_panel_description_display_t *d = SCAN_PANEL_ROW(&p, SCAN_PANEL_DISPLAYS, omx_clap_panel_description_display_t, i);

        snprintf(scope, sizeof(scope), "display:%u", p.table[SCAN_PANEL_DISPLAYS].index[i]);
        record_code(scope, "omx_clap_panel_display.kind", scan_panel_display_kind_enum(d->kind), d->kind);
        record_number(scope, "omx_clap_panel_display.section", d->section);
        record_code(scope, "omx_clap_panel_display.source", scan_panel_display_source_enum(d->source), d->source);
        record_id(scope, "omx_clap_panel_display.meter_ids[0]", d->meter_ids[0]);
        record_id(scope, "omx_clap_panel_display.meter_ids[1]", d->meter_ids[1]);
        record_number(scope, "omx_clap_panel_display.curve_index", d->curve_index);
        record_id(scope, "omx_clap_panel_display.floor_param_id", d->floor_param_id);
        record_number(scope, "omx_clap_panel_display.floor_db", d->floor_db);
        record_text(scope, "omx_clap_panel_display.label_key", d->label_key);
        record_text(scope, "omx_clap_panel_display.label", d->label);
    }
    for (i = 0; i < p.table[SCAN_PANEL_PLACEMENT_DEFAULTS].n; i++)
    {
        const omx_clap_panel_description_default_t *d = SCAN_PANEL_ROW(&p, SCAN_PANEL_PLACEMENT_DEFAULTS, omx_clap_panel_description_default_t, i);

        snprintf(scope, sizeof(scope), "placement_default:%u", p.table[SCAN_PANEL_PLACEMENT_DEFAULTS].index[i]);
        record_code(scope, "omx_clap_panel_placement_default.placement", scan_panel_placement_enum(d->placement), d->placement);
        record_id(scope, "omx_clap_panel_placement_default.param_id", d->param_id);
        record_number(scope, "omx_clap_panel_placement_default.value", d->value);
    }
    scan_panel_free(&p);
}

/* the one activation: clap.latency and clap.voice-info answer while active */
static void clap_active(const struct omx_clap_instance *in)
{
    const clap_plugin_t *plugin = in->plugin;
    const clap_plugin_voice_info_t *voice = plugin->get_extension(plugin, CLAP_EXT_VOICE_INFO);
    clap_voice_info_t info;

    if ((!in->latency && !(voice && voice->get)) || !plugin->activate(plugin, FIELDS_RATE, 1, FIELDS_MAX_FRAMES))
        return;
    if (in->latency && in->latency->get)
        record_number("plugin", "clap_plugin_latency.get", in->latency->get(plugin));
    memset(&info, 0, sizeof(info));
    if (voice && voice->get && voice->get(plugin, &info))
    {
        record_number("plugin", "clap_voice_info.voice_count", info.voice_count);
        record_number("plugin", "clap_voice_info.voice_capacity", info.voice_capacity);
        record_bits("plugin", "clap_voice_info.flags", info.flags, g_voice_flags);
    }
    plugin->deactivate(plugin);
}

static int fields_clap(const char *path, const char *id)
{
    char reason[512];
    struct omx_clap_binary *binary = omx_clap_host_binary_open(path, reason, sizeof(reason));
    const clap_plugin_descriptor_t *desc = NULL;
    struct omx_clap_instance *in;
    const char *file = strrchr(path, '/');
    const char *const *f;
    uint32_t i, count;

    if (!binary)
    {
        fprintf(stderr, "omx-clap-scan: %s: %s\n", path, reason);
        return 1;
    }
    count = omx_clap_host_binary_count(binary);
    for (i = 0; i < count && !desc; i++)
    {
        const clap_plugin_descriptor_t *d = omx_clap_host_binary_descriptor(binary, i);

        if (d && d->id && strcmp(d->id, id) == 0)
            desc = d;
    }
    if (!desc || omx_clap_host_create(binary, desc, &in) != 0)
    {
        fprintf(stderr, "omx-clap-scan: %s: %s\n", path, desc ? "the plugin failed to create or init" : "no plugin of that id");
        omx_clap_host_binary_close(binary);
        return 1;
    }

    record("plugin", "clap_entry", file ? file + 1 : path);
    record_text("plugin", "clap_plugin_descriptor.id", desc->id);
    record_text("plugin", "clap_plugin_descriptor.name", desc->name);
    record_text("plugin", "clap_plugin_descriptor.vendor", desc->vendor);
    record_text("plugin", "clap_plugin_descriptor.version", desc->version);
    record_text("plugin", "clap_plugin_descriptor.description", desc->description);
    record_text("plugin", "clap_plugin_descriptor.url", desc->url);
    record_text("plugin", "clap_plugin_descriptor.manual_url", desc->manual_url);
    record_text("plugin", "clap_plugin_descriptor.support_url", desc->support_url);
    for (f = desc->features; f && *f; f++)
        record_text("plugin", "clap_plugin_descriptor.features", NAMED(g_features, *f));
    for (i = 0; i < sizeof(g_extensions) / sizeof(g_extensions[0]); i++)
        if (in->plugin->get_extension(in->plugin, g_extensions[i].value))
            record("plugin", "clap_plugin.get_extension", g_extensions[i].name);
    for (f = g_omx_extensions; *f; f++)
        if (in->plugin->get_extension(in->plugin, *f))
            record("plugin", "clap_plugin.get_extension", *f);
    clap_params(in);
    clap_audio_ports(in, true);
    clap_audio_ports(in, false);
    clap_note_ports(in, true);
    clap_note_ports(in, false);
    clap_panel(in);
    clap_active(in);
    omx_clap_host_close(in);
    omx_clap_host_binary_close(binary);
    return 0;
}


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS: LV2
************************************************************************************************************************
*/

/* the bundle directory of `bundle_uri`'s plugin, as the dump writes its files: <bundle directory>/<file> */
static const char *g_bundle_dir;

/* a URI as the dump writes it: a file of the bundle by its path from the bundle's parent, a URI a prefix covers as
 * <prefix>:<local> (a specification itself as <prefix>:<spec>.lv2), any other as it is */
static void lv2_uri_text(const char *uri, char *buf, size_t size)
{
    size_t i, n;

    if (g_bundle_dir && strncmp(uri, "file://", 7) == 0)
    {
        const char *path = uri + 7, *base;

        n = strlen(g_bundle_dir);
        if (strncmp(path, g_bundle_dir, n) == 0 && path[n] == '/')
        {
            for (base = g_bundle_dir + n; base > g_bundle_dir && base[-1] != '/'; base--)
                ;
            snprintf(buf, size, "%s%s", base, path + n);
            return;
        }
    }
    for (i = 0; i < sizeof(g_prefixes) / sizeof(g_prefixes[0]); i++)
    {
        const prefix_t *p = &g_prefixes[i];

        n = strlen(p->ns);
        if (strncmp(uri, p->ns, n) == 0 && uri[n] && !strpbrk(uri + n, "#/"))
        {
            snprintf(buf, size, "%s:%s", p->prefix, uri + n);
            return;
        }
        if (p->spec && strncmp(uri, p->ns, n - 1) == 0 && uri[n - 1] == '\0')
        {
            snprintf(buf, size, "%s:%s.lv2", p->prefix, p->spec);
            return;
        }
    }
    snprintf(buf, size, "%s", uri);
}

static void lv2_field(void *ctx, int32_t port, uint32_t ask, enum lv2_field_kind kind, const char *value)
{
    char scope[32], text[PATH_MAX + 64];

    (void)ctx;
    if (port < 0)
        snprintf(scope, sizeof(scope), "plugin");
    else
        snprintf(scope, sizeof(scope), "port:%d", port);
    if (kind == LV2_FIELD_URI)
    {
        lv2_uri_text(value, text, sizeof(text));
        value = text;
    }
    record_text(scope, g_lv2_fields[ask].curie, value);
}

static int lv2_found(const char *bundle, void *ctx)
{
    struct lv2_find *find = ctx;
    char why[LV2_CORE_WHY_MAX];
    struct lv2_bundle *b = lv2_bundle_ref(bundle, why);
    uint32_t i, n = lv2_bundle_count(b);

    for (i = 0; i < n; i++)
        if (strcmp(lv2_bundle_uri(b, i), find->uri) == 0)
        {
            snprintf(find->bundle, sizeof(find->bundle), "%s", bundle);
            lv2_bundle_unref(b);
            return 1;
        }
    lv2_bundle_unref(b);
    return 0;
}

static int fields_lv2(const char *uri)
{
    const size_t count = sizeof(g_lv2_fields) / sizeof(g_lv2_fields[0]);
    struct lv2_field_ask asks[sizeof(g_lv2_fields) / sizeof(g_lv2_fields[0])];
    char uris[sizeof(g_lv2_fields) / sizeof(g_lv2_fields[0])][160];
    struct lv2_core_config config;
    struct lv2_find find;
    struct lv2_bundle *b;
    char why[LV2_CORE_WHY_MAX];
    size_t i, j;
    int rc;

    memset(&config, 0, sizeof(config));
    config.worker_poll_us = OMX_HOSTED_STAGE_WORKER_POLL_US;
    config.worker_ring_bytes = OMX_HOSTED_STAGE_WORKER_RING_BYTES;
    if (lv2_core_configure(&config, why) != 0)
    {
        fprintf(stderr, "omx-clap-scan: the LV2 adapter: %s\n", why);
        return 1;
    }
    memset(&find, 0, sizeof(find));
    find.uri = uri;
    if (!lv2_locate(NULL, uri, lv2_found, &find))
    {
        fprintf(stderr, "omx-clap-scan: %s: no bundle on the LV2 path holds it\n", uri);
        return 1;
    }
    // each field's URI, its CURIE spelled out under the prefix table
    for (i = 0; i < count; i++)
    {
        const char *colon = strchr(g_lv2_fields[i].curie, ':');

        uris[i][0] = '\0';
        for (j = 0; j < sizeof(g_prefixes) / sizeof(g_prefixes[0]); j++)
            if (strlen(g_prefixes[j].prefix) == (size_t)(colon - g_lv2_fields[i].curie) &&
                strncmp(g_prefixes[j].prefix, g_lv2_fields[i].curie, (size_t)(colon - g_lv2_fields[i].curie)) == 0)
                snprintf(uris[i], sizeof(uris[i]), "%s%s", g_prefixes[j].ns, colon + 1);
        asks[i].uri = uris[i];
        asks[i].by_label = g_lv2_fields[i].by_label;
    }
    b = lv2_bundle_ref(find.bundle, why);
    g_bundle_dir = find.bundle;
    rc = lv2_bundle_fields(b, uri, asks, (uint32_t)count, lv2_field, NULL, why);
    g_bundle_dir = NULL;
    lv2_bundle_unref(b);
    if (rc != 0)
    {
        fprintf(stderr, "omx-clap-scan: %s: %s\n", uri, why);
        return 1;
    }
    return 0;
}


/*
************************************************************************************************************************
*           GLOBAL FUNCTIONS
************************************************************************************************************************
*/

int scan_fields_list(void)
{
    size_t i;

    printf("# omx-clap-scan --fields %d\n", FIELDS_VERSION);
    for (i = 0; i < sizeof(g_extensions) / sizeof(g_extensions[0]); i++)
        printf("clap\textension\t%s\t%s\n", g_extensions[i].name, g_extensions[i].value);
    for (i = 0; i < sizeof(g_features) / sizeof(g_features[0]); i++)
        printf("clap\tfeature\t%s\t%s\n", g_features[i].name, g_features[i].value);
    for (i = 0; i < sizeof(g_port_types) / sizeof(g_port_types[0]); i++)
        printf("clap\tport-type\t%s\t%s\n", g_port_types[i].name, g_port_types[i].value);
    for (i = 0; i < sizeof(g_lv2_fields) / sizeof(g_lv2_fields[0]); i++)
        printf("lv2\tfield\t%s\n", g_lv2_fields[i].curie);
    for (i = 0; i < sizeof(g_prefixes) / sizeof(g_prefixes[0]); i++)
        printf("lv2\tprefix\t%s\t%s\n", g_prefixes[i].prefix, g_prefixes[i].ns);
    return 0;
}

/* the line that closes the child's dump on the pipe: the parent writes the dump only when it reads this as its end, so
 * a child that leaves early (a plugin that calls exit(0) in its code) gives no dump, not an empty one */
static const char g_dump_end[] = "# omx-clap-scan --fields end\n";

/* all of buffer to fd, through a short write and a signal */
static int write_all(int fd, const char *buffer, size_t size)
{
    while (size > 0)
    {
        ssize_t n = write(fd, buffer, size);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        buffer += n;
        size -= (size_t)n;
    }
    return 0;
}

/* the dump itself, in the child: the plugin's code runs here and only here; the whole dump, its version line first and
 * g_dump_end last, goes to fd once the plugin is done with */
static int fields_dump(const char *target, int fd)
{
    char *dump = NULL, head[64];
    size_t size = 0;
    int rc;

    // what the plugin prints goes to stderr; the dump to the pipe, once it is whole
    fflush(stdout);
    g_out = open_memstream(&dump, &size);
    if (!g_out || dup2(STDERR_FILENO, STDOUT_FILENO) < 0)
    {
        fprintf(stderr, "omx-clap-scan: no stream for the dump\n");
        return 1;
    }
    if (strncmp(target, "clap:", 5) == 0)
    {
        char *path = strdup(target + 5), *hash = path ? strrchr(path, '#') : NULL;

        if (hash)
            *hash = '\0';
        rc = hash ? fields_clap(path, hash + 1) : 1;
        free(path);
    }
    else
        rc = fields_lv2(target + 4);
    fclose(g_out);
    // the plugin is done with: its time limit is not the pipe's
    alarm(0);
    if (rc == 0)
    {
        snprintf(head, sizeof(head), "# omx-clap-scan --fields %d\n", FIELDS_VERSION);
        if (write_all(fd, head, strlen(head)) < 0 || write_all(fd, dump, size) < 0
            || write_all(fd, g_dump_end, sizeof(g_dump_end) - 1) < 0)
            rc = 1;
    }
    free(dump);
    return rc;
}

/* everything the child writes to fd, to its end; NULL when there is no memory for it */
static char *read_all(int fd, size_t *size)
{
    char *buffer = NULL;
    size_t have = 0, room = 0;

    for (;;)
    {
        ssize_t n;

        if (room - have < 4096)
        {
            char *grown = realloc(buffer, room ? room * 2 : 65536);

            if (!grown)
            {
                free(buffer);
                return NULL;
            }
            buffer = grown;
            room = room ? room * 2 : 65536;
        }
        n = read(fd, buffer + have, room - have);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        have += (size_t)n;
    }
    *size = have;
    return buffer;
}

/* the seconds the child gets: FIELDS_TIMEOUT_S, or OMX_CLAP_SCAN_FIELDS_TIMEOUT when it is a whole number in range */
static int fields_timeout(void)
{
    const char *env = getenv("OMX_CLAP_SCAN_FIELDS_TIMEOUT");
    char *end = NULL;
    long seconds;

    if (!env || !*env)
        return FIELDS_TIMEOUT_S;
    seconds = strtol(env, &end, 10);
    return *end == '\0' && seconds >= 1 && seconds <= FIELDS_TIMEOUT_MAX_S ? (int)seconds : FIELDS_TIMEOUT_S;
}

/* the dump is made in a child, as a listing's file is: a plugin that crashes or hangs in its entry, init or a call
 * (tests/crash_plugin.c aborts in clap_entry.init) takes the child, never the scanner. The child hands its dump over a
 * pipe and the parent writes it to stdout only when it is whole, from its version line to g_dump_end: a child that
 * crashes, hangs or leaves early leaves stdout empty, and the reason goes to stderr */
int scan_fields(const char *target)
{
    pid_t pid, waited;
    int status = 0, pipe_fd[2], timeout = fields_timeout();
    char *dump;
    size_t size = 0, head = 0, end = sizeof(g_dump_end) - 1;
    char version[64];

    if (strncmp(target, "clap:", 5) != 0 && strncmp(target, "lv2:", 4) != 0)
    {
        fprintf(stderr, "omx-clap-scan: --fields takes clap:<path>#<id> or lv2:<uri>, not %s\n", target);
        return 2;
    }
    if (strncmp(target, "clap:", 5) == 0 && !strrchr(target, '#'))
    {
        fprintf(stderr, "omx-clap-scan: --fields clap:<path>#<id>: %s names no plugin id\n", target);
        return 2;
    }
    fflush(stdout);
    fflush(stderr);
    if (pipe(pipe_fd) < 0)
    {
        fprintf(stderr, "omx-clap-scan: --fields: no pipe for the dump\n");
        return 1;
    }
    pid = fork();
    if (pid < 0)
    {
        fprintf(stderr, "omx-clap-scan: --fields: no process for the dump\n");
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return 1;
    }
    if (pid == 0)
    {
        close(pipe_fd[0]);
        alarm((unsigned)timeout);
        _exit(fields_dump(target, pipe_fd[1]));
    }
    close(pipe_fd[1]);
    dump = read_all(pipe_fd[0], &size);
    close(pipe_fd[0]);
    do
        waited = waitpid(pid, &status, 0);
    while (waited < 0 && errno == EINTR);
    if (waited < 0)
    {
        free(dump);
        return 1;
    }
    if (WIFSIGNALED(status))
    {
        if (WTERMSIG(status) == SIGALRM)
            fprintf(stderr, "omx-clap-scan: --fields: %s: timed out after %d s\n", target, timeout);
        else
            fprintf(stderr, "omx-clap-scan: --fields: %s: crashed with signal %d (%s)\n", target, WTERMSIG(status), strsignal(WTERMSIG(status)));
        free(dump);
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        free(dump);
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }
    head = (size_t)snprintf(version, sizeof(version), "# omx-clap-scan --fields %d\n", FIELDS_VERSION);
    if (!dump || size < head + end || memcmp(dump, version, head) != 0 || memcmp(dump + size - end, g_dump_end, end) != 0)
    {
        fprintf(stderr, "omx-clap-scan: --fields: %s: the plugin's process ended before its dump was whole\n", target);
        free(dump);
        return 1;
    }
    fwrite(dump, 1, size - end, stdout);
    fflush(stdout);
    free(dump);
    return 0;
}
