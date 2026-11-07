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

/* The host's read of a plugin's held meters (docs/meters.md), through the host's own effects.c: this file includes it, so
 * the code under test is outputs_notify, effects_monitor_output and effects_monitor_output_off as the program runs them,
 * against the real core and omx-dsp. Only JACK and the feedback socket are replaced: the plugin's blocks are run by
 * hand, the clock is the test's, and output_set lands in a list.
 *
 * Two plugins process the same blocks. The held one keeps the peak (the deepest reduction, a clip) of every block since
 * the last read, as the omx plugins do; the foreign one gives the last block it processed.
 * 48-frame blocks at 48 kHz are 1 ms; a tick is 20 ms, the host read rate omx-dsp declares. */

#include "../src/effects.c"
#include "test_util.h"

#include <stdatomic.h>
#include <sys/socket.h>

#define HELD_ID     "org.omx-clap-host.test.held-meters"
#define FOREIGN_ID  "org.omx-clap-host.test.foreign-meters"
#define FRAMES      48u
#define MS          0.001

/* ---- the plugins ---- */

typedef struct {
    int held;
    _Atomic uint32_t level_bits;    /* linear peak, as the float's bits: non-negative floats order as their bits */
    _Atomic uint32_t reduction_bits;/* the reduction in dB as a magnitude */
    _Atomic uint32_t clip;
    _Atomic uint32_t processed;
} meter_fixture_t;

static uint32_t bits_of(float f) { uint32_t b; memcpy(&b, &f, sizeof(b)); return b; }
static float float_of(uint32_t b) { float f; memcpy(&f, &b, sizeof(f)); return f; }

static void store_max(_Atomic uint32_t *word, uint32_t bits)
{
    uint32_t seen = atomic_load_explicit(word, memory_order_relaxed);

    while (bits > seen && !atomic_compare_exchange_weak_explicit(word, &seen, bits, memory_order_relaxed, memory_order_relaxed))
        ;
}

static clap_process_status fixture_process(const clap_plugin_t *plugin, const clap_process_t *process)
{
    meter_fixture_t *fx = plugin->plugin_data;
    float peak = 0.0f;
    uint32_t i;

    for (i = 0; i < process->frames_count; i++)
    {
        const float a = fabsf(process->audio_inputs[0].data32[0][i]);

        peak = a > peak ? a : peak;
        process->audio_outputs[0].data32[0][i] = process->audio_inputs[0].data32[0][i];
        process->audio_outputs[0].data32[1][i] = process->audio_inputs[0].data32[1][i];
    }
    if (fx->held)
    {
        store_max(&fx->level_bits, bits_of(peak));
        store_max(&fx->reduction_bits, bits_of(peak >= 0.1f ? 6.0f : 0.0f));
        if (peak >= 1.0f)
            atomic_store_explicit(&fx->clip, 1, memory_order_relaxed);
    }
    else
    {
        atomic_store_explicit(&fx->level_bits, bits_of(peak), memory_order_relaxed);
        atomic_store_explicit(&fx->reduction_bits, bits_of(peak >= 0.1f ? 6.0f : 0.0f), memory_order_relaxed);
    }
    atomic_store_explicit(&fx->processed, 1, memory_order_relaxed);
    return CLAP_PROCESS_CONTINUE;
}

static uint32_t fixture_meters_count(const clap_plugin_t *plugin) { (void)plugin; return 2; }

static bool fixture_meters_info(const clap_plugin_t *plugin, uint32_t index, clap_meter_info_t *info)
{
    const meter_fixture_t *fx = plugin->plugin_data;

    if (index > 1)
        return false;
    memset(info, 0, sizeof(*info));
    info->id = index;
    info->flags = CLAP_METER_IS_COHERENT | (fx->held ? OMX_CLAP_METER_IS_HELD : 0u);
    info->channel_count = 1;
    if (index == 0)
    {
        snprintf(info->name, sizeof(info->name), "level");
        info->kind = CLAP_METER_KIND_LEVEL;
        info->min_value = -60.0;
        info->max_value = 0.0;
        info->flags |= fx->held ? OMX_CLAP_METER_HAS_CLIP : 0u;
    }
    else
    {
        snprintf(info->name, sizeof(info->name), "gain reduction");
        info->kind = CLAP_METER_KIND_GAIN_REDUCTION;
        info->min_value = -24.0;
        info->max_value = 0.0;
    }
    return true;
}

/* [thread-safe] a held meter hands over what it kept and starts again; a foreign one gives the last block */
static uint32_t fixture_meters_values(const clap_plugin_t *plugin, clap_id id, double *values, uint32_t capacity)
{
    meter_fixture_t *fx = plugin->plugin_data;
    uint32_t n = 0;

    if (!atomic_load_explicit(&fx->processed, memory_order_relaxed) || capacity == 0 || id > 1)
        return 0;
    if (id == 0)
    {
        const float peak = float_of(fx->held ? atomic_exchange_explicit(&fx->level_bits, 0, memory_order_relaxed)
                                             : atomic_load_explicit(&fx->level_bits, memory_order_relaxed));

        values[n++] = peak > 0.0f ? 20.0 * log10(peak) : -INFINITY;
        if (fx->held && capacity > 1)
            values[n++] = atomic_exchange_explicit(&fx->clip, 0, memory_order_relaxed) ? 1.0 : 0.0;
        return n;
    }
    {
        const float mag = float_of(fx->held ? atomic_exchange_explicit(&fx->reduction_bits, 0, memory_order_relaxed)
                                            : atomic_load_explicit(&fx->reduction_bits, memory_order_relaxed));

        values[0] = -(double)mag;
        return 1;
    }
}

static const clap_plugin_meters_t FIXTURE_METERS = { fixture_meters_count, fixture_meters_info, fixture_meters_values };

static bool fixture_init(const clap_plugin_t *p) { (void)p; return true; }
static void fixture_destroy(const clap_plugin_t *p) { free(p->plugin_data); free((void *)p); }
static bool fixture_activate(const clap_plugin_t *p, double sr, uint32_t a, uint32_t b) { (void)p, (void)sr, (void)a, (void)b; return true; }
static void fixture_nop(const clap_plugin_t *p) { (void)p; }
static bool fixture_start(const clap_plugin_t *p) { (void)p; return true; }
static uint32_t fixture_ports_count(const clap_plugin_t *p, bool in) { (void)p, (void)in; return 1; }
static bool fixture_ports_get(const clap_plugin_t *p, uint32_t i, bool in, clap_audio_port_info_t *info)
{
    (void)p, (void)in;
    if (i != 0)
        return false;
    memset(info, 0, sizeof(*info));
    snprintf(info->name, sizeof(info->name), "main");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
static const clap_plugin_audio_ports_t FIXTURE_PORTS = { fixture_ports_count, fixture_ports_get };
static const void *fixture_ext(const clap_plugin_t *p, const char *id)
{
    (void)p;
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
        return &FIXTURE_PORTS;
    return strcmp(id, CLAP_EXT_METERS) == 0 ? &FIXTURE_METERS : NULL;
}

static const char *const FIXTURE_FEATURES[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL };
static const clap_plugin_descriptor_t FIXTURE_DESCS[2] = {
    { CLAP_VERSION_INIT, HELD_ID, "held meters", "omx-clap-host", "", "", "", "0", "", FIXTURE_FEATURES },
    { CLAP_VERSION_INIT, FOREIGN_ID, "foreign meters", "omx-clap-host", "", "", "", "0", "", FIXTURE_FEATURES },
};
static uint32_t fixture_count(const clap_plugin_factory_t *f) { (void)f; return 2; }
static const clap_plugin_descriptor_t *fixture_desc(const clap_plugin_factory_t *f, uint32_t i) { (void)f; return i < 2 ? &FIXTURE_DESCS[i] : NULL; }
static const clap_plugin_t *fixture_create(const clap_plugin_factory_t *f, const clap_host_t *host, const char *id)
{
    clap_plugin_t *p;
    meter_fixture_t *fx;
    uint32_t i;

    (void)f, (void)host;
    for (i = 0; i < 2 && strcmp(id, FIXTURE_DESCS[i].id) != 0; i++)
        ;
    if (i == 2)
        return NULL;
    fx = calloc(1, sizeof(*fx));
    fx->held = i == 0;
    p = calloc(1, sizeof(*p));
    *p = (clap_plugin_t){ &FIXTURE_DESCS[i], fx, fixture_init, fixture_destroy, fixture_activate, fixture_nop, fixture_start,
                          fixture_nop, fixture_nop, fixture_process, fixture_ext, fixture_nop };
    return p;
}
static const clap_plugin_factory_t FIXTURE_FACTORY = { fixture_count, fixture_desc, fixture_create };
static bool fixture_entry_init(const char *path) { (void)path; return true; }
static void fixture_entry_deinit(void) {}
static const void *fixture_factory(const char *id) { return strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &FIXTURE_FACTORY : NULL; }
static const clap_plugin_entry_t FIXTURE_ENTRY = { CLAP_VERSION_INIT, fixture_entry_init, fixture_entry_deinit, fixture_factory };

/* ---- the feedback socket: output_set lands in a list ---- */

typedef struct { int instance; char symbol[OUTPUT_SYMBOL_SIZE]; float value; } sent_t;
static sent_t g_sent[4096];
static int g_sent_count;

int host_dispatch_output_set(int instance, const char *symbol, float value)
{
    if (g_sent_count == (int)(sizeof(g_sent) / sizeof(g_sent[0])))
        return -1;
    g_sent[g_sent_count].instance = instance;
    snprintf(g_sent[g_sent_count].symbol, sizeof(g_sent[0].symbol), "%s", symbol);
    g_sent[g_sent_count].value = value;
    g_sent_count++;
    return 0;
}

/* the lowest value of a symbol sent from `from` on (a held reduction is negative), the highest for a level */
static int sent_since(int from, int instance, const char *symbol, float *lowest, float *highest)
{
    int i, n = 0;

    *lowest = INFINITY;
    *highest = -INFINITY;
    for (i = from; i < g_sent_count; i++)
        if (g_sent[i].instance == instance && strcmp(g_sent[i].symbol, symbol) == 0)
        {
            n++;
            if (g_sent[i].value < *lowest) *lowest = g_sent[i].value;
            if (g_sent[i].value > *highest) *highest = g_sent[i].value;
        }
    return n;
}

static float last_sent(int instance, const char *symbol)
{
    int i;

    for (i = g_sent_count - 1; i >= 0; i--)
        if (g_sent[i].instance == instance && strcmp(g_sent[i].symbol, symbol) == 0)
            return g_sent[i].value;
    return NAN;
}

/* ---- the instances ---- */

static int g_dummy_client;
static float g_in[2][FRAMES], g_out[2][FRAMES];

static int add_fixture(int id, const char *plugin_id)
{
    effect_t *effect = &g_effects[id];
    char why[OMX_CLAP_WHY_MAX];

    memset(effect, 0, sizeof(*effect));
    effect->instance = id;
    effect->jack_client = (jack_client_t *)&g_dummy_client;
    atomic_store(&effect->rate, 48000.0);
    if (omx_clap_host_open_entry(&FIXTURE_ENTRY, plugin_id, &effect->clap, why) != 0 ||
        omx_clap_host_activate(effect->clap, 48000.0, 64, why) != 0)
        return -1;
    {
        /* activation warms the plugin up on a tone: what a held meter kept of it is the first read's, and is dropped here */
        double drop[2];

        fixture_meters_values(effect->clap->plugin, 0, drop, 2);
        fixture_meters_values(effect->clap->plugin, 1, drop, 2);
    }
    return outputs_build(effect);
}

/* one 1 ms block of the plugin: a constant on the left */
static void block(int id, float amplitude)
{
    float *in[2] = { g_in[0], g_in[1] }, *out[2] = { g_out[0], g_out[1] };
    clap_audio_buffer_t ai = { in, NULL, 2, 0, 0 }, ao = { out, NULL, 2, 0, 0 };
    clap_process_t pr;
    uint32_t i;

    for (i = 0; i < FRAMES; i++)
        g_in[0][i] = g_in[1][i] = amplitude;
    memset(&pr, 0, sizeof(pr));
    pr.frames_count = FRAMES;
    pr.audio_inputs = &ai;
    pr.audio_outputs = &ao;
    pr.audio_inputs_count = pr.audio_outputs_count = 1;
    g_effects[id].clap->plugin->process(g_effects[id].clap->plugin, &pr);
}

static void blocks(int id, float amplitude, int n)
{
    while (n-- > 0)
        block(id, amplitude);
}

/* between two reads, 20 ms of 1 ms blocks: 5 quiet, a 10 ms burst, 5 quiet. Every instance gets the same ones. */
static void tick_with_burst(float burst, int ids[], int n)
{
    int k;

    for (k = 0; k < n; k++)
        blocks(ids[k], 0.001f, 5);
    for (k = 0; k < n; k++)
        blocks(ids[k], burst, 10);
    for (k = 0; k < n; k++)
        blocks(ids[k], 0.001f, 5);
}

static double g_now;
static void tick(void)
{
    int i;

    g_now += 1.0 / METER_READ_HZ;
    for (i = 0; i < MAX_INSTANCES; i++)
        if (g_effects[i].jack_client)
            outputs_notify_at(&g_effects[i], g_now);
}

static int g_socks[2];
static void new_connection(void)
{
    if (g_socks[0] > 0) close(g_socks[0]);
    if (g_socks[1] > 0) close(g_socks[1]);
    socketpair(AF_UNIX, SOCK_STREAM, 0, g_socks);
    effects_connection(g_socks[0]);
}

int main(void)
{
    float lo, hi;
    int held = 0, foreign = 1, ids[2], from, n, i;
    int64_t reads;

    CHECK(add_fixture(held, HELD_ID) == 0 && add_fixture(foreign, FOREIGN_ID) == 0, "both plugins open and name their outputs");
    ids[0] = held;
    ids[1] = foreign;
    new_connection();

    /* the read tick is the declared host read rate, and the idle interval is its period */
    CHECK(METER_READ_HZ == 50 && METER_READ_INTERVAL_MS * METER_READ_HZ == 1000, "the host read rate is omx-dsp's hostReadHz, 50, and the interval 20 ms");

    CHECK(effects_meter_held(held, "level") == 1 && effects_meter_held(held, "gain_reduction") == 1 &&
          effects_meter_held(foreign, "level") == 0 && effects_meter_held(foreign, "gain_reduction") == 0,
          "the held plugin's meters are named held, the foreign plugin's unheld");
    CHECK(effects_meter_held(held, "nothing") == -1 && effects_meter_held(9, "level") == -1, "an output or an instance that is none is neither");
    CHECK(effects_meter_held(held, "level_clip") == 1 && effects_meter_held(foreign, "level_clip") == -1, "only the held level has a clip word");

    /* nothing monitored, nothing read */
    blocks(held, 0.001f, 20);
    blocks(foreign, 0.001f, 20);
    for (i = 0; i < 10; i++) tick();
    CHECK(effects_meter_reads(held) == 0 && effects_meter_reads(foreign) == 0 && g_sent_count == 0,
          "with nothing monitored no meter is read and nothing is sent (reads %lld, %lld)",
          (long long)effects_meter_reads(held), (long long)effects_meter_reads(foreign));

    /* a 10 ms burst between two reads */
    for (i = 0; i < 2; i++)
    {
        CHECK(effects_monitor_output(i, "level") == SUCCESS && effects_monitor_output(i, "gain_reduction") == SUCCESS, "instance %d monitors its level and its reduction", i);
    }
    CHECK(effects_monitor_output(held, "level_clip") == SUCCESS && effects_monitor_output(foreign, "level_clip") == ERR_LV2_INVALID_PARAM_SYMBOL,
          "the held level's clip word is monitored; the foreign plugin has none");
    tick_with_burst(0.5f, ids, 2);
    from = g_sent_count;
    tick();
    CHECK(sent_since(from, held, "level", &lo, &hi) == 1 && fabsf(hi - 20.0f * log10f(0.5f)) < 0.01f,
          "the held level reaches output_set with the burst's peak, -6.02 dBFS (sent %.3f)", hi);
    CHECK(sent_since(from, foreign, "level", &lo, &hi) == 1 && fabsf(hi - 20.0f * log10f(0.001f)) < 0.01f,
          "the foreign level gives the last block, -60 dBFS: the burst is missed and its meter is named unheld (sent %.3f)", hi);
    CHECK(sent_since(from, held, "gain_reduction", &lo, &hi) == 1 && lo == -6.0f,
          "the held reduction is the deepest of the interval, -6 dB (sent %.3f)", lo);
    CHECK(sent_since(from, foreign, "gain_reduction", &lo, &hi) == 1 && lo == 0.0f,
          "the foreign reduction is the last block's, 0 dB (sent %.3f)", lo);
    CHECK(last_sent(held, "level_clip") == 0.0f, "no clip word lit by a burst under full scale");

    /* the reduction recovers by elapsed time: held 300 ms, then it lands; at one tick rate or another */
    for (i = 0; i < 14; i++)    /* 280 ms after the tick that took it */
    {
        blocks(held, 0.001f, 20);
        tick();
    }
    CHECK(last_sent(held, "gain_reduction") == -6.0f, "the reduction is held for 300 ms, still -6 dB after 280 ms (%.3f)", last_sent(held, "gain_reduction"));
    for (i = 0; i < 3; i++)
    {
        blocks(held, 0.001f, 20);
        tick();
    }
    CHECK(last_sent(held, "gain_reduction") == 0.0f, "and it recovers once the hold is spent (%.3f)", last_sent(held, "gain_reduction"));

    {
        /* the same seconds at another tick rate: ticks 4 ms apart land at the same time, 300 ms after the last deepest read */
        double t0;
        int slow;

        blocks(held, 0.5f, 5);
        tick();
        t0 = g_now;
        CHECK(last_sent(held, "gain_reduction") == -6.0f, "a new burst takes the reduction at once");
        for (slow = 0; slow < 200; slow++)
        {
            blocks(held, 0.001f, 4);
            g_now += 0.004;
            outputs_notify_at(&g_effects[held], g_now);
            if (last_sent(held, "gain_reduction") == 0.0f)
                break;
        }
        CHECK(slow < 200 && g_now - t0 >= 0.296 && g_now - t0 <= 0.31,
              "ticks 4 ms apart recover it after the same 300 ms (%.3f s)", g_now - t0);
    }

    /* the clip word: lit by a clip, kept lit by the reader, cleared on request */
    blocks(held, 1.0f, 3);
    tick();
    CHECK(last_sent(held, "level_clip") == 1.0f, "a clip between two reads lights the clip word");
    blocks(held, 0.001f, 20);
    tick();
    tick();
    CHECK(last_sent(held, "level_clip") == 1.0f, "the reader keeps it lit while the plugin reads clean again");
    CHECK(effects_meter_clip_clear(held, "level_clip") == SUCCESS && effects_meter_clip_clear(held, "level") == ERR_LV2_INVALID_PARAM_SYMBOL,
          "a clip word is cleared; a level is no clip word");
    blocks(held, 0.001f, 20);
    tick();
    CHECK(last_sent(held, "level_clip") == 0.0f, "and reads dark after the clear");

    /* monitor_output_off stops the reads of that output and no other */
    reads = effects_meter_reads(held);
    CHECK(effects_monitor_output_off(held, "gain_reduction") == SUCCESS && effects_monitor_output_off(held, "level_clip") == SUCCESS, "monitor_output_off stops two outputs");
    CHECK(effects_monitor_output_off(held, "nothing") == ERR_LV2_INVALID_PARAM_SYMBOL && effects_monitor_output_off(9, "level") == ERR_INSTANCE_NON_EXISTS,
          "it refuses a symbol or an instance that is none, as monitor_output does");
    tick_with_burst(0.5f, ids, 2);
    from = g_sent_count;
    for (i = 0; i < 10; i++) tick();
    CHECK(sent_since(from, held, "gain_reduction", &lo, &hi) == 0 && sent_since(from, held, "level_clip", &lo, &hi) == 0,
          "a stopped output sends nothing");
    CHECK(effects_meter_reads(held) - reads <= 10 + 1, "the meter is read once a tick for the one output still monitored (%lld reads in 11 ticks)", (long long)(effects_meter_reads(held) - reads));
    CHECK(effects_monitor_output_off(held, "level") == SUCCESS && effects_monitor_output_off(held, "level") == SUCCESS, "the last one stops, twice without harm");
    reads = effects_meter_reads(held);
    for (i = 0; i < 25; i++) { blocks(held, 0.5f, 20); tick(); }
    CHECK(effects_meter_reads(held) == reads, "with no watcher left the plugin is read zero times (%lld)", (long long)(effects_meter_reads(held) - reads));

    /* a monitor ends with the connection that set it */
    CHECK(effects_monitor_output(held, "level") == SUCCESS, "monitored again on the same connection");
    reads = effects_meter_reads(held);
    tick();
    CHECK(effects_meter_reads(held) == reads + 1, "it is read");
    from = g_sent_count;
    new_connection();   /* the controller reconnects; its descriptor is the old one's number again */
    reads = effects_meter_reads(held);
    n = effects_meter_reads(foreign);
    for (i = 0; i < 10; i++) { blocks(held, 0.5f, 20); blocks(foreign, 0.5f, 20); tick(); }
    CHECK(effects_meter_reads(held) == reads && effects_meter_reads(foreign) == n && g_sent_count == from,
          "after its connection ended no monitor is read and nothing is sent, though the new connection has the old descriptor number");
    CHECK(effects_monitor_output(held, "level") == SUCCESS, "the new connection monitors again");
    tick();
    CHECK(effects_meter_reads(held) == reads + 1 && sent_since(from, held, "level", &lo, &hi) == 1, "and it is read from then");

    for (i = 0; i < 2; i++)
    {
        omx_clap_host_close(g_effects[i].clap);
        free(g_effects[i].outputs);
    }
    printf("%s\n", g_failures ? "FAILED" : "all ok");
    return g_failures ? 1 : 0;
}
