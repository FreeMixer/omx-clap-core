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


/* A compressor built for the tests, whose meters are known from its input. A block whose peak on either channel reaches
 * THRESHOLD is attenuated by exactly 6 dB and reports a gain adjustment of -6 dB; a quieter block passes untouched and
 * reports 0. Stereo in and out.
 *   org.omx-clap-host.test.compressor        both org.openmixer.meters/1 and clap.gain-adjustment-metering/0:
 *                                            meter 0 "gain reduction", the same -6 or 0 as the standard value
 *                                            meter 1 "input level", the input peak of each channel in dBFS
 *                                            meter 2 "2nd-stage GR", -3 while the first stage reduces, 0 otherwise
 *                                            meter 3 "0", the peak of the left output in dBFS
 *   org.omx-clap-host.test.compressor-std    clap.gain-adjustment-metering/0 only
 *   org.omx-clap-host.test.compressor-clash  meter 0 "level" of two channels and meter 1 "level 0" of one, which
 *                                            derive the same symbol
 * The one parameter, 0, read-only, counts the calls of clap.gain-adjustment-metering's get() from a thread other than
 * the one process() runs on; such a call answers +99 dB. */

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <clap/clap.h>
#include <clap/ext/draft/gain-adjustment-metering.h>

#include "omx_clap_ext.h"

#define ID_COMPRESSOR   "org.omx-clap-host.test.compressor"
#define ID_STD          "org.omx-clap-host.test.compressor-std"
#define ID_CLASH        "org.omx-clap-host.test.compressor-clash"
#define THRESHOLD       0.1f
#define REDUCTION_DB    -6.0
#define SECOND_STAGE_DB -3.0f
#define OFF_THREAD_DB   99.0

enum { METER_GR, METER_INPUT_L, METER_INPUT_R, METER_SECOND, METER_OUTPUT_L, METER_VALUES };

typedef struct FAKE_T {
    clap_plugin_t plugin;
    int meters;
    int standard;
    int clash;
    float gain;                         // 10^(REDUCTION_DB / 20)
    _Atomic uint32_t values[METER_VALUES];  // the floats of the last process(), published relaxed
    _Atomic uint32_t processed;
    pthread_t audio_thread;
    _Atomic uint32_t audio_thread_known;
    _Atomic uint32_t off_thread_calls;
} fake_t;

static const char *const g_features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_COMPRESSOR, NULL };

#define DESCRIPTOR(name, plugin_id) \
    { CLAP_VERSION_INIT, plugin_id, name, "omx-clap-host", "", "", "", "0", name, g_features }

static const clap_plugin_descriptor_t g_descriptors[] = {
    DESCRIPTOR("compressor", ID_COMPRESSOR),
    DESCRIPTOR("compressor-std", ID_STD),
    DESCRIPTOR("compressor-clash", ID_CLASH),
};

#define DESCRIPTOR_COUNT (sizeof(g_descriptors) / sizeof(g_descriptors[0]))

static void publish(fake_t *fake, int index, float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    atomic_store_explicit(&fake->values[index], bits, memory_order_relaxed);
}

static float published(const fake_t *fake, int index)
{
    const uint32_t bits = atomic_load_explicit(&fake->values[index], memory_order_relaxed);
    float value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

static float dbfs(float peak)
{
    return peak > 0.0f ? 20.0f * log10f(peak) : -HUGE_VALF;
}

static uint32_t audio_ports_count(const clap_plugin_t *plugin, bool is_input)
{
    (void)plugin; (void)is_input;
    return 1;
}

static bool audio_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_audio_port_info_t *info)
{
    (void)plugin;
    if (index != 0)
        return false;
    memset(info, 0, sizeof(*info));
    info->id = 0;
    strcpy(info->name, is_input ? "in" : "out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t g_audio_ports = { audio_ports_count, audio_ports_get };

static uint32_t params_count(const clap_plugin_t *plugin)
{
    (void)plugin;
    return 1;
}

static bool params_get_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info)
{
    (void)plugin;
    if (index != 0)
        return false;
    memset(info, 0, sizeof(*info));
    info->id = 0;
    info->flags = CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_READONLY;
    info->max_value = 1e9;
    strcpy(info->name, "gain adjustment reads off the audio thread");
    return true;
}

static bool params_get_value(const clap_plugin_t *plugin, clap_id id, double *value)
{
    const fake_t *fake = plugin->plugin_data;

    if (id != 0)
        return false;
    *value = atomic_load(&fake->off_thread_calls);
    return true;
}

static bool params_value_to_text(const clap_plugin_t *plugin, clap_id id, double value, char *out, uint32_t size)
{
    (void)plugin; (void)id; (void)value; (void)out; (void)size;
    return false;
}

static bool params_text_to_value(const clap_plugin_t *plugin, clap_id id, const char *text, double *value)
{
    (void)plugin; (void)id; (void)text; (void)value;
    return false;
}

static void params_flush(const clap_plugin_t *plugin, const clap_input_events_t *in, const clap_output_events_t *out)
{
    (void)plugin; (void)in; (void)out;
}

static const clap_plugin_params_t g_params = {
    params_count, params_get_info, params_get_value, params_value_to_text, params_text_to_value, params_flush
};

/* [audio-thread]: the thread process() last ran on, or the call is counted and answers OFF_THREAD_DB */
static double gain_adjustment_get(const clap_plugin_t *plugin)
{
    fake_t *fake = plugin->plugin_data;

    if (!atomic_load(&fake->audio_thread_known) || !pthread_equal(pthread_self(), fake->audio_thread))
    {
        atomic_fetch_add(&fake->off_thread_calls, 1);
        return OFF_THREAD_DB;
    }
    return published(fake, METER_GR);
}

static const clap_plugin_gain_adjustment_metering_t g_gain_adjustment = { gain_adjustment_get };

static uint32_t meters_count(const clap_plugin_t *plugin)
{
    const fake_t *fake = plugin->plugin_data;
    return fake->clash ? 2 : 4;
}

static bool meters_get_info(const clap_plugin_t *plugin, uint32_t index, omx_clap_meter_info_t *info)
{
    static const struct { const char *name; uint32_t kind, channels; } meters[] = {
        { "gain reduction", OMX_CLAP_METER_GAIN_REDUCTION_DB, 1 },
        { "input level", OMX_CLAP_METER_LEVEL_DBFS, 2 },
        { "2nd-stage GR", OMX_CLAP_METER_GAIN_REDUCTION_DB, 1 },
        { "0", OMX_CLAP_METER_LEVEL_DBFS, 1 },
    }, clash[] = {
        { "level", OMX_CLAP_METER_LEVEL_DBFS, 2 },
        { "level 0", OMX_CLAP_METER_LEVEL_DBFS, 1 },
    };
    const fake_t *fake = plugin->plugin_data;

    if (index >= meters_count(plugin))
        return false;
    memset(info, 0, sizeof(*info));
    info->id = index;
    snprintf(info->name, sizeof(info->name), "%s", fake->clash ? clash[index].name : meters[index].name);
    info->kind = fake->clash ? clash[index].kind : meters[index].kind;
    info->channel_count = fake->clash ? clash[index].channels : meters[index].channels;
    return true;
}

/* [thread-safe] */
static bool meters_read(const clap_plugin_t *plugin, clap_id id, float *values, uint32_t capacity)
{
    const fake_t *fake = plugin->plugin_data;

    if (id >= meters_count(plugin) || !atomic_load_explicit(&fake->processed, memory_order_relaxed) || capacity == 0)
        return false;
    switch (id)
    {
        case 0:
            values[0] = published(fake, METER_GR);
            break;
        case 1:
            values[0] = published(fake, METER_INPUT_L);
            if (capacity > 1)
                values[1] = published(fake, METER_INPUT_R);
            break;
        case 2:
            values[0] = published(fake, METER_SECOND);
            break;
        default:
            values[0] = published(fake, METER_OUTPUT_L);
            break;
    }
    return true;
}

static const omx_clap_plugin_meters_t g_meters = { meters_count, meters_get_info, meters_read };

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
    fake_t *fake = plugin->plugin_data;

    (void)sample_rate; (void)min_frames; (void)max_frames;
    atomic_store(&fake->processed, 0);
    publish(fake, METER_GR, 0.0f);
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
    fake_t *fake = plugin->plugin_data;
    publish(fake, METER_GR, 0.0f);
}

static void plugin_reset(const clap_plugin_t *plugin)
{
    (void)plugin;
}

static clap_process_status plugin_process(const clap_plugin_t *plugin, const clap_process_t *process)
{
    fake_t *fake = plugin->plugin_data;
    const clap_audio_buffer_t *in = &process->audio_inputs[0];
    clap_audio_buffer_t *out = &process->audio_outputs[0];
    float peak[2] = { 0.0f, 0.0f }, gain, out_peak = 0.0f;
    uint32_t c, i;

    fake->audio_thread = pthread_self();
    atomic_store(&fake->audio_thread_known, 1);

    for (c = 0; c < 2; c++)
        for (i = 0; i < process->frames_count; i++)
            if (fabsf(in->data32[c][i]) > peak[c])
                peak[c] = fabsf(in->data32[c][i]);
    gain = peak[0] >= THRESHOLD || peak[1] >= THRESHOLD ? fake->gain : 1.0f;
    for (c = 0; c < 2; c++)
        for (i = 0; i < process->frames_count; i++)
            out->data32[c][i] = in->data32[c][i] * gain;
    for (i = 0; i < process->frames_count; i++)
        if (fabsf(out->data32[0][i]) > out_peak)
            out_peak = fabsf(out->data32[0][i]);

    publish(fake, METER_GR, gain < 1.0f ? (float)REDUCTION_DB : 0.0f);
    publish(fake, METER_INPUT_L, dbfs(peak[0]));
    publish(fake, METER_INPUT_R, dbfs(peak[1]));
    publish(fake, METER_SECOND, gain < 1.0f ? SECOND_STAGE_DB : 0.0f);
    publish(fake, METER_OUTPUT_L, dbfs(out_peak));
    atomic_store_explicit(&fake->processed, 1, memory_order_relaxed);
    return CLAP_PROCESS_CONTINUE;
}

static const void *plugin_get_extension(const clap_plugin_t *plugin, const char *id)
{
    const fake_t *fake = plugin->plugin_data;

    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &g_audio_ports;
    if (!strcmp(id, CLAP_EXT_PARAMS))
        return &g_params;
    if (!strcmp(id, CLAP_EXT_GAIN_ADJUSTMENT_METERING) && fake->standard)
        return &g_gain_adjustment;
    if (!strcmp(id, OMX_CLAP_EXT_METERS) && fake->meters)
        return &g_meters;
    return NULL;
}

static void plugin_on_main_thread(const clap_plugin_t *plugin)
{
    (void)plugin;
}

static uint32_t factory_get_plugin_count(const clap_plugin_factory_t *factory)
{
    (void)factory;
    return DESCRIPTOR_COUNT;
}

static const clap_plugin_descriptor_t *factory_get_plugin_descriptor(const clap_plugin_factory_t *factory, uint32_t index)
{
    (void)factory;
    return index < DESCRIPTOR_COUNT ? &g_descriptors[index] : NULL;
}

static const clap_plugin_t *factory_create_plugin(const clap_plugin_factory_t *factory, const clap_host_t *host, const char *plugin_id)
{
    fake_t *fake;
    uint32_t i;

    (void)factory; (void)host;
    for (i = 0; i < DESCRIPTOR_COUNT; i++)
        if (!strcmp(g_descriptors[i].id, plugin_id))
            break;
    if (i == DESCRIPTOR_COUNT)
        return NULL;

    fake = calloc(1, sizeof(fake_t));
    fake->meters = strcmp(plugin_id, ID_STD) != 0;
    fake->standard = strcmp(plugin_id, ID_CLASH) != 0;
    fake->clash = !strcmp(plugin_id, ID_CLASH);
    fake->gain = powf(10.0f, (float)REDUCTION_DB / 20.0f);

    fake->plugin.desc = &g_descriptors[i];
    fake->plugin.plugin_data = fake;
    fake->plugin.init = plugin_init;
    fake->plugin.destroy = plugin_destroy;
    fake->plugin.activate = plugin_activate;
    fake->plugin.deactivate = plugin_deactivate;
    fake->plugin.start_processing = plugin_start_processing;
    fake->plugin.stop_processing = plugin_stop_processing;
    fake->plugin.reset = plugin_reset;
    fake->plugin.process = plugin_process;
    fake->plugin.get_extension = plugin_get_extension;
    fake->plugin.on_main_thread = plugin_on_main_thread;
    return &fake->plugin;
}

static const clap_plugin_factory_t g_factory = {
    factory_get_plugin_count, factory_get_plugin_descriptor, factory_create_plugin
};

static bool entry_init(const char *path)
{
    (void)path;
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
