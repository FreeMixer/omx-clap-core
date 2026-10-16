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


/* A synth built for the tests, deterministic to the sample. A note on starts
 * a sine at the key's frequency from phase 0 on the frame it arrives on, at
 * the velocity as its amplitude; a note off is silence from its frame on. One
 * voice, the last note on wins, a note off for another key is ignored. Every
 * plugin of the file is the same synth behind a different port layout:
 *   org.omx-clap-host.test.synth          stereo out, CLAP note dialect
 *   org.omx-clap-host.test.synth-midi     mono out, MIDI note dialect
 *   org.omx-clap-host.test.synth-aux      an extra audio input, refused
 *   org.omx-clap-host.test.synth-wide     4 output channels, refused
 *   org.omx-clap-host.test.synth-notes    two note inputs, refused
 *   org.omx-clap-host.test.synth-mpe      a note input with no dialect it can read, refused
 *   org.omx-clap-host.test.synth-poly     stereo out, CLAP note dialect, a voice per note id: a note off starts a
 *                                         release of FAKE_SYNTH_RELEASE frames, after which the synth pushes
 *                                         CLAP_EVENT_NOTE_END with the voice's id, channel and key. A note on whose
 *                                         id a voice still sounds under is counted (fake_synth_poly_stats)
 *   org.omx-clap-host.test.silent         an effect with no input and no note input, refused */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <clap/clap.h>

#define ID_SYNTH        "org.omx-clap-host.test.synth"
#define ID_SYNTH_MIDI   "org.omx-clap-host.test.synth-midi"
#define ID_SYNTH_AUX    "org.omx-clap-host.test.synth-aux"
#define ID_SYNTH_WIDE   "org.omx-clap-host.test.synth-wide"
#define ID_SYNTH_NOTES  "org.omx-clap-host.test.synth-notes"
#define ID_SYNTH_MPE    "org.omx-clap-host.test.synth-mpe"
#define ID_SILENT       "org.omx-clap-host.test.silent"
#define ID_SYNTH_POLY   "org.omx-clap-host.test.synth-poly"

/** the release of a synth-poly voice, in frames from its note off to its NOTE_END */
#define FAKE_SYNTH_RELEASE  1024u
/** the voices synth-poly holds, more than any id pool a host has */
#define FAKE_SYNTH_VOICES   1024u

typedef struct LAYOUT_T {
    const char *id;
    uint32_t out_channels;
    uint32_t aux_inputs;
    uint32_t note_inputs;
    uint32_t supported;
    uint32_t preferred;
    int instrument;
    int poly;
} layout_t;

static const layout_t g_layouts[] = {
    { ID_SYNTH,       2, 0, 1, CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI, CLAP_NOTE_DIALECT_CLAP, 1, 0 },
    { ID_SYNTH_MIDI,  1, 0, 1, CLAP_NOTE_DIALECT_MIDI, CLAP_NOTE_DIALECT_MIDI, 1, 0 },
    { ID_SYNTH_AUX,   2, 1, 1, CLAP_NOTE_DIALECT_CLAP, CLAP_NOTE_DIALECT_CLAP, 1, 0 },
    { ID_SYNTH_WIDE,  4, 0, 1, CLAP_NOTE_DIALECT_CLAP, CLAP_NOTE_DIALECT_CLAP, 1, 0 },
    { ID_SYNTH_NOTES, 2, 0, 2, CLAP_NOTE_DIALECT_CLAP, CLAP_NOTE_DIALECT_CLAP, 1, 0 },
    { ID_SYNTH_MPE,   2, 0, 1, CLAP_NOTE_DIALECT_MIDI_MPE, CLAP_NOTE_DIALECT_MIDI_MPE, 1, 0 },
    { ID_SILENT,      2, 0, 0, 0, 0, 0, 0 },
    { ID_SYNTH_POLY,  2, 0, 1, CLAP_NOTE_DIALECT_CLAP, CLAP_NOTE_DIALECT_CLAP, 1, 1 },
};

#define LAYOUT_COUNT (sizeof(g_layouts) / sizeof(g_layouts[0]))

/** one synth-poly voice: free, sounding (its key down), or in its release */
typedef struct VOICE_T {
    int32_t note_id;
    int16_t channel;
    int16_t key;
    uint32_t release_left;      ///< frames of release left; 0 while the key is down
    int on;                     ///< 1 sounding or releasing
} voice_t;

/** What synth-poly saw, for a test that dlsyms fake_synth_poly_stats: the file's one poly instance at a time. */
typedef struct FAKE_SYNTH_POLY_STATS_T {
    uint32_t notes_on;          ///< note ons taken
    uint32_t ids_reused;        ///< note ons whose id a voice still sounds or releases under
    uint32_t no_id;             ///< note ons with note id -1
    uint32_t ended;             ///< NOTE_END pushed
    uint32_t end_refused;       ///< NOTE_END the host's list refused
    uint32_t voices;            ///< voices sounding or releasing now
    uint32_t voices_max;        ///< the most at once
} fake_synth_poly_stats_t;

static fake_synth_poly_stats_t g_poly;

CLAP_EXPORT const fake_synth_poly_stats_t *fake_synth_poly_stats(void)
{
    return &g_poly;
}

typedef struct SYNTH_T {
    clap_plugin_t plugin;
    const layout_t *layout;
    double sample_rate;
    int sounding;
    int16_t key;
    double velocity;
    uint64_t age;
    voice_t voices[FAKE_SYNTH_VOICES];  ///< synth-poly's voices
} synth_t;

static const char *const g_synth_features[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, NULL };
static const char *const g_effect_features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL };

static clap_plugin_descriptor_t g_descriptors[LAYOUT_COUNT];

static void descriptors_fill(void)
{
    size_t i;

    for (i = 0; i < LAYOUT_COUNT; i++)
    {
        const clap_plugin_descriptor_t d = { CLAP_VERSION_INIT, g_layouts[i].id, g_layouts[i].id + 23, "omx-clap-host", "", "", "",
                                             "0", g_layouts[i].id, g_layouts[i].instrument ? g_synth_features : g_effect_features };
        g_descriptors[i] = d;
    }
}

static uint32_t audio_ports_count(const clap_plugin_t *plugin, bool is_input)
{
    const synth_t *synth = plugin->plugin_data;
    return is_input ? synth->layout->aux_inputs : 1;
}

static bool audio_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_audio_port_info_t *info)
{
    const synth_t *synth = plugin->plugin_data;

    if (index >= audio_ports_count(plugin, is_input))
        return false;
    memset(info, 0, sizeof(*info));
    info->id = index;
    snprintf(info->name, sizeof(info->name), "%s %u", is_input ? "in" : "out", index);
    info->flags = is_input ? 0 : CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = is_input ? 1 : synth->layout->out_channels;
    info->port_type = info->channel_count == 2 ? CLAP_PORT_STEREO : (info->channel_count == 1 ? CLAP_PORT_MONO : NULL);
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t g_audio_ports = { audio_ports_count, audio_ports_get };

static uint32_t note_ports_count(const clap_plugin_t *plugin, bool is_input)
{
    const synth_t *synth = plugin->plugin_data;
    return is_input ? synth->layout->note_inputs : 0;
}

static bool note_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_note_port_info_t *info)
{
    const synth_t *synth = plugin->plugin_data;

    if (index >= note_ports_count(plugin, is_input))
        return false;
    memset(info, 0, sizeof(*info));
    info->id = index;
    info->supported_dialects = synth->layout->supported;
    info->preferred_dialect = synth->layout->preferred;
    snprintf(info->name, sizeof(info->name), "notes %u", index);
    return true;
}

static const clap_plugin_note_ports_t g_note_ports = { note_ports_count, note_ports_get };

static void note_on(synth_t *synth, int16_t key, double velocity)
{
    synth->sounding = 1;
    synth->key = key;
    synth->velocity = velocity;
    synth->age = 0;
}

static void poly_event(synth_t *synth, const clap_event_header_t *header)
{
    const clap_event_note_t *e = (const clap_event_note_t *)header;
    uint32_t v;

    if (header->type == CLAP_EVENT_NOTE_ON)
    {
        g_poly.notes_on++;
        if (e->note_id < 0)
            g_poly.no_id++;
        for (v = 0; v < FAKE_SYNTH_VOICES; v++)
            if (synth->voices[v].on && e->note_id >= 0 && synth->voices[v].note_id == e->note_id)
                g_poly.ids_reused++;
        for (v = 0; v < FAKE_SYNTH_VOICES && synth->voices[v].on; v++)
            ;
        if (v == FAKE_SYNTH_VOICES)
            return;
        synth->voices[v].on = 1;
        synth->voices[v].note_id = e->note_id;
        synth->voices[v].channel = e->channel;
        synth->voices[v].key = e->key;
        synth->voices[v].release_left = 0;
        if (++g_poly.voices > g_poly.voices_max)
            g_poly.voices_max = g_poly.voices;
    }
    else if (header->type == CLAP_EVENT_NOTE_OFF)
    {
        for (v = 0; v < FAKE_SYNTH_VOICES; v++)
        {
            voice_t *voice = &synth->voices[v];

            if (!voice->on || voice->release_left)
                continue;
            if (e->note_id >= 0 ? voice->note_id == e->note_id
                                : (e->channel < 0 || voice->channel == e->channel) && (e->key < 0 || voice->key == e->key))
                voice->release_left = FAKE_SYNTH_RELEASE;
        }
    }
}

/* synth-poly, after frame `frame` of the block: a voice whose release ran out ends, and says so */
static void poly_frame(synth_t *synth, const clap_output_events_t *out, uint32_t frame)
{
    uint32_t v;

    for (v = 0; v < FAKE_SYNTH_VOICES; v++)
    {
        voice_t *voice = &synth->voices[v];
        clap_event_note_t end;

        if (!voice->on || voice->release_left == 0 || --voice->release_left)
            continue;
        memset(&end, 0, sizeof(end));
        end.header.size = sizeof(end);
        end.header.time = frame;
        end.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        end.header.type = CLAP_EVENT_NOTE_END;
        end.note_id = voice->note_id;
        end.port_index = 0;
        end.channel = voice->channel;
        end.key = voice->key;
        if (out->try_push(out, &end.header))
            g_poly.ended++;
        else
            g_poly.end_refused++;
        voice->on = 0;
        g_poly.voices--;
    }
}

static void take_event(synth_t *synth, const clap_event_header_t *header)
{
    if (header->space_id != CLAP_CORE_EVENT_SPACE_ID)
        return;
    if (synth->layout->poly)
    {
        poly_event(synth, header);
        return;
    }
    if (header->type == CLAP_EVENT_NOTE_ON)
    {
        const clap_event_note_t *e = (const clap_event_note_t *)header;
        note_on(synth, e->key, e->velocity);
    }
    else if (header->type == CLAP_EVENT_NOTE_OFF)
    {
        const clap_event_note_t *e = (const clap_event_note_t *)header;
        if (synth->sounding && e->key == synth->key)
            synth->sounding = 0;
    }
    else if (header->type == CLAP_EVENT_MIDI)
    {
        const clap_event_midi_t *e = (const clap_event_midi_t *)header;
        const uint8_t type = e->data[0] & 0xf0;
        if (type == 0x90 && e->data[2] > 0)
            note_on(synth, e->data[1], e->data[2] / 127.0);
        else if ((type == 0x80 || type == 0x90) && synth->sounding && e->data[1] == synth->key)
            synth->sounding = 0;
    }
}

static float sample(const synth_t *synth)
{
    const double hz = 440.0 * pow(2.0, ((double)synth->key - 69.0) / 12.0);
    return (float)(synth->velocity * sin(2.0 * M_PI * hz * (double)synth->age / synth->sample_rate));
}

static bool plugin_init(const clap_plugin_t *plugin)
{
    (void)plugin;
    return true;
}

static void plugin_destroy(const clap_plugin_t *plugin)
{
    free(plugin->plugin_data);
}

static bool plugin_activate(const clap_plugin_t *plugin, double sample_rate, uint32_t min_frames, uint32_t max_frames)
{
    synth_t *synth = plugin->plugin_data;

    (void)min_frames;
    (void)max_frames;
    synth->sample_rate = sample_rate;
    return true;
}

static void plugin_deactivate(const clap_plugin_t *plugin)
{
    (void)plugin;
}

static bool plugin_start_processing(const clap_plugin_t *plugin)
{
    (void)plugin;
    return true;
}

static void plugin_stop_processing(const clap_plugin_t *plugin)
{
    (void)plugin;
}

static void plugin_reset(const clap_plugin_t *plugin)
{
    synth_t *synth = plugin->plugin_data;
    synth->sounding = 0;
    if (synth->layout->poly)
    {
        memset(synth->voices, 0, sizeof(synth->voices));
        g_poly.voices = 0;
    }
}

static clap_process_status plugin_process(const clap_plugin_t *plugin, const clap_process_t *process)
{
    synth_t *synth = plugin->plugin_data;
    clap_audio_buffer_t *out = &process->audio_outputs[0];
    const uint32_t count = process->in_events->size(process->in_events);
    uint32_t next = 0, i, c;

    for (i = 0; i < process->frames_count; i++)
    {
        while (next < count)
        {
            const clap_event_header_t *header = process->in_events->get(process->in_events, next);
            if (header->time > i)
                break;
            take_event(synth, header);
            next++;
        }
        for (c = 0; c < out->channel_count; c++)
            out->data32[c][i] = synth->sounding ? sample(synth) : 0.0f;
        if (synth->sounding)
            synth->age++;
        if (synth->layout->poly)
        {
            for (c = 0; c < out->channel_count; c++)
                out->data32[c][i] = (float)g_poly.voices * 1e-3f;  // a level that says how many voices sound
            poly_frame(synth, process->out_events, i);
        }
    }
    return CLAP_PROCESS_CONTINUE;
}

static const void *plugin_get_extension(const clap_plugin_t *plugin, const char *id)
{
    const synth_t *synth = plugin->plugin_data;

    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &g_audio_ports;
    if (!strcmp(id, CLAP_EXT_NOTE_PORTS) && synth->layout->note_inputs)
        return &g_note_ports;
    return NULL;
}

static void plugin_on_main_thread(const clap_plugin_t *plugin)
{
    (void)plugin;
}

static uint32_t factory_get_plugin_count(const clap_plugin_factory_t *factory)
{
    (void)factory;
    return LAYOUT_COUNT;
}

static const clap_plugin_descriptor_t *factory_get_plugin_descriptor(const clap_plugin_factory_t *factory, uint32_t index)
{
    (void)factory;
    return index < LAYOUT_COUNT ? &g_descriptors[index] : NULL;
}

static const clap_plugin_t *factory_create_plugin(const clap_plugin_factory_t *factory, const clap_host_t *host, const char *plugin_id)
{
    synth_t *synth;
    uint32_t i;

    (void)factory;
    (void)host;
    for (i = 0; i < LAYOUT_COUNT; i++)
        if (!strcmp(g_layouts[i].id, plugin_id))
            break;
    if (i == LAYOUT_COUNT)
        return NULL;

    synth = calloc(1, sizeof(synth_t));
    synth->layout = &g_layouts[i];
    if (synth->layout->poly)
        memset(&g_poly, 0, sizeof(g_poly));
    synth->sample_rate = 48000.0;

    synth->plugin.desc = &g_descriptors[i];
    synth->plugin.plugin_data = synth;
    synth->plugin.init = plugin_init;
    synth->plugin.destroy = plugin_destroy;
    synth->plugin.activate = plugin_activate;
    synth->plugin.deactivate = plugin_deactivate;
    synth->plugin.start_processing = plugin_start_processing;
    synth->plugin.stop_processing = plugin_stop_processing;
    synth->plugin.reset = plugin_reset;
    synth->plugin.process = plugin_process;
    synth->plugin.get_extension = plugin_get_extension;
    synth->plugin.on_main_thread = plugin_on_main_thread;
    return &synth->plugin;
}

static const clap_plugin_factory_t g_factory = {
    factory_get_plugin_count, factory_get_plugin_descriptor, factory_create_plugin
};

static bool entry_init(const char *path)
{
    (void)path;
    descriptors_fill();
    return true;
}

static void entry_deinit(void)
{
}

static const void *entry_get_factory(const char *factory_id)
{
    return !strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) ? &g_factory : NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory
};
