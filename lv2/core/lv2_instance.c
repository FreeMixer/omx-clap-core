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
* One instance of an LV2 plugin (see lv2_core.h): instantiate, the ports connected once to the instance's own memory,
* the default state, activate, the worker thread and its two rings, and the LV2 half of a block. Moved from the
* console's mix_host_backend.c (the LV2 add, the worker thread) and mix_lv2.h (the run half, the worker callbacks).
*
* Every port is connected to memory the instance owns, once, on the main thread, before activate: connect_port is not
* promised real-time safe, and an LV2 plugin may dereference any port it declares. The audio buffers are the
* instance's too, so a plugin flagged lv2:inPlaceBroken is safe by construction.
*
* The worker. One non-RT thread per instance whose plugin has a worker interface, started at the end of activate and
* joined at deactivate, before LV2 deactivate. Two rings (omx_msgring.h), requests and responses. schedule_work never waits:
* a full request ring answers LV2_WORKER_ERR_NO_SPACE and is counted. respond is legal only inside work(), which a
* thread-local marker says; a stray one is refused and struck. Nothing signals the worker from the RT: it polls its
* request ring every worker_poll_us.
*
************************************************************************************************************************
*/


/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
#include <lv2/time/time.h>
#include <lv2/worker/worker.h>

#include "hosted_stage.h"
#include "omx_msgring.h"
#include "lv2_core_internal.h"


/*
************************************************************************************************************************
*           LOCAL DEFINES
************************************************************************************************************************
*/

// how often a quiesce looks at the request ring
#define QUIESCE_POLL_US                 50u

// the smallest time:Position buffer: an object with every field is about 300 bytes, so one always fits
#define TIME_MIN_BYTES                  512u


/*
************************************************************************************************************************
*           LOCAL DATA TYPES
************************************************************************************************************************
*/

// the URIDs a time:Position object is forged with, mapped at instantiate
struct lv2_time_urids
{
    LV2_URID frame_time, position, frame, speed, bar, bar_beat, beat_unit, beats_per_bar, bpm;
};

struct lv2_instance
{
    const struct lv2_plugin *plugin;
    LV2_Handle handle;
    const LV2_Worker_Interface *worker;     // NULL: the plugin has no worker, or the list provides none
    int lv2_active;
    double rate;
    uint32_t max_frames;

    // the instance's own memory every port is connected to
    float *audio;                           // in L, in R, out L, out R, max_frames each
    float *controls;                        // one per plugin->controls
    _Atomic uint32_t *shown;                // the same as bits, for a reader on another thread
    float latency_port;
    _Atomic uint32_t latency_frames;        // the latency port's last valid reading

    // the features `instantiate` got, each the instance's own over the process's table, so a call names its instance
    LV2_URID_Map map;
    LV2_URID_Unmap unmap;
    LV2_Log_Log log;
    _Atomic(lv2_audio_role_fn) is_audio;
    _Atomic(void *) role_ctx;
    _Atomic uint32_t map_on_audio;
    _Atomic uint32_t schedule_off_audio;
    _Atomic uint32_t log_on_audio;
    struct
    {
        float rate;
        int32_t min_block, max_block;
        LV2_Options_Option opts[5];
    } options;
    LV2_Worker_Schedule schedule;
    LV2_Feature features[LV2_FEATURE_KINDS];
    const LV2_Feature *feature_ptrs[LV2_FEATURE_KINDS + 1];

    // the worker
    uint8_t *ring_mem;                      // 2 x worker_ring_bytes; NULL: the list provides no worker
    struct omx_msgring requests, responses;
    pthread_t thread;
    int worker_running;
    _Atomic int worker_quit;
    _Atomic uint32_t schedule_refused;
    _Atomic uint32_t responses_refused;
    _Atomic uint32_t respond_strikes;

    // the time:Position input: a sequence buffer of its own, the forge over it, and the transport last forged, so an
    // unchanged transport forges nothing. The forge maps through lv2_urid_lookup, which every URID it names was mapped by
    // at instantiate, so the audio role never maps one
    LV2_Atom_Sequence *time_buf;            // NULL: the plugin has no time port
    uint32_t time_bytes;
    LV2_URID_Map time_map;
    LV2_Atom_Forge time_forge;
    struct lv2_time_urids time_urid;
    struct lv2_time time_last;
    int time_have;
};


/*
************************************************************************************************************************
*           LOCAL GLOBAL VARIABLES
************************************************************************************************************************
*/

/* the instance whose work() runs on THIS thread: the only context in which respond is legal */
static _Thread_local const struct lv2_instance *g_in_work;


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS: THE WORKER
************************************************************************************************************************
*/

static uint32_t float_bits(float v)
{
    uint32_t u;

    memcpy(&u, &v, sizeof(u));
    return u;
}

static float bits_float(uint32_t u)
{
    float v;

    memcpy(&v, &u, sizeof(v));
    return v;
}

/* whether the calling thread holds the instance's audio role, by the host's predicate; -1 without one */
static int on_audio(const struct lv2_instance *in)
{
    const lv2_audio_role_fn is_audio = atomic_load_explicit(&in->is_audio, memory_order_acquire);

    return is_audio ? is_audio(atomic_load_explicit(&in->role_ctx, memory_order_relaxed)) != 0 : -1;
}

/* urid:map and urid:unmap over the process's table; a map on the audio role is counted */
static LV2_URID inst_map(LV2_URID_Map_Handle h, const char *uri)
{
    struct lv2_instance *in = h;

    if (on_audio(in) == 1)
        omx_hosted_count(&in->map_on_audio, 1);
    return lv2_urid_lookup(uri);
}

static const char *inst_unmap(LV2_URID_Unmap_Handle h, LV2_URID id)
{
    (void)h;
    return lv2_urid_unlookup(id);
}

/* log:log formats nothing and writes nothing: a line from run() must never reach a system call */
static int inst_log_vprintf(LV2_Log_Handle h, LV2_URID type, const char *fmt, va_list ap)
{
    struct lv2_instance *in = h;

    (void)type;
    (void)fmt;
    (void)ap;
    if (on_audio(in) == 1)
        omx_hosted_count(&in->log_on_audio, 1);
    return 0;
}

static int inst_log_printf(LV2_Log_Handle h, LV2_URID type, const char *fmt, ...)
{
    struct lv2_instance *in = h;

    (void)type;
    (void)fmt;
    if (on_audio(in) == 1)
        omx_hosted_count(&in->log_on_audio, 1);
    return 0;
}

/* [audio] the LV2 schedule_work callback */
static LV2_Worker_Status schedule_work(LV2_Worker_Schedule_Handle h, uint32_t size, const void *data)
{
    struct lv2_instance *in = h;

    if (!in || !in->ring_mem)
        return LV2_WORKER_ERR_UNKNOWN;
    if (on_audio(in) == 0)
        omx_hosted_count(&in->schedule_off_audio, 1);
    if (omx_msgring_push(&in->requests, data, size) != 0)
    {
        omx_hosted_count(&in->schedule_refused, 1);
        return LV2_WORKER_ERR_NO_SPACE;
    }
    return LV2_WORKER_SUCCESS;
}

/* [worker] the respond callback handed to work() */
static LV2_Worker_Status respond(LV2_Worker_Respond_Handle h, uint32_t size, const void *data)
{
    struct lv2_instance *in = h;

    if (!in || g_in_work != in)
    {
        if (in)
            omx_hosted_count(&in->respond_strikes, 1);
        return LV2_WORKER_ERR_UNKNOWN;
    }
    if (omx_msgring_push(&in->responses, data, size) != 0)
    {
        omx_hosted_count(&in->responses_refused, 1);
        return LV2_WORKER_ERR_NO_SPACE;
    }
    return LV2_WORKER_SUCCESS;
}

/* [worker] every queued request through work(); how many */
static uint32_t worker_service(struct lv2_instance *in)
{
    uint32_t done = 0, size;
    const void *p;

    while ((p = omx_msgring_peek(&in->requests, &size)) != NULL)
    {
        g_in_work = in;
        in->worker->work(in->handle, respond, in, size, p);
        g_in_work = NULL;
        omx_msgring_pop(&in->requests);
        done++;
    }
    return done;
}

/* the worker thread: services the request ring by polling; nothing signals it from the RT */
static void *worker_main(void *arg)
{
    struct lv2_instance *in = arg;
    const uint32_t us = lv2_core_config()->worker_poll_us;
    const struct timespec poll = { (time_t)(us / 1000000u), (long)(us % 1000000u) * 1000L };

    while (!atomic_load_explicit(&in->worker_quit, memory_order_acquire))
        if (worker_service(in) == 0)
            nanosleep(&poll, NULL);
    return NULL;
}

static int worker_start(struct lv2_instance *in)
{
    if (!in->worker || in->worker_running)
        return 0;
    atomic_store_explicit(&in->worker_quit, 0, memory_order_relaxed);
    if (pthread_create(&in->thread, NULL, worker_main, in) != 0)
        return -1;
    pthread_setname_np(in->thread, "omx-lv2-worker");
    in->worker_running = 1;
    return 0;
}

static void worker_stop(struct lv2_instance *in)
{
    if (!in->worker_running)
        return;
    atomic_store_explicit(&in->worker_quit, 1, memory_order_release);
    pthread_join(in->thread, NULL);
    in->worker_running = 0;
}


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS: INSTANTIATE
************************************************************************************************************************
*/

/* options:options: the rate, and the block lengths exactly as this activation's min and max say (the nominal is the
 * max, the block the host runs) */
static void options_build(struct lv2_instance *in, double rate, uint32_t min_frames, uint32_t max_frames)
{
    const LV2_URID t_int = lv2_urid_lookup(LV2_ATOM__Int), t_float = lv2_urid_lookup(LV2_ATOM__Float);
    const LV2_Options_Option o[5] =
    {
        { LV2_OPTIONS_INSTANCE, 0, lv2_urid_lookup(LV2_PARAMETERS__sampleRate), sizeof(float), t_float, &in->options.rate },
        { LV2_OPTIONS_INSTANCE, 0, lv2_urid_lookup(LV2_BUF_SIZE__minBlockLength), sizeof(int32_t), t_int, &in->options.min_block },
        { LV2_OPTIONS_INSTANCE, 0, lv2_urid_lookup(LV2_BUF_SIZE__maxBlockLength), sizeof(int32_t), t_int, &in->options.max_block },
        { LV2_OPTIONS_INSTANCE, 0, lv2_urid_lookup(LV2_BUF_SIZE__nominalBlockLength), sizeof(int32_t), t_int, &in->options.max_block },
        { LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, NULL },
    };

    in->options.rate = (float)rate;
    in->options.min_block = (int32_t)min_frames;
    in->options.max_block = (int32_t)max_frames;
    memcpy(in->options.opts, o, sizeof(o));
}

/* the feature array, in the providers' order, exactly the configured kinds */
static void features_build(struct lv2_instance *in)
{
    uint32_t n = 0;
    int k;

    for (k = 0; k < LV2_FEATURE_KINDS; k++)
    {
        void *data = NULL;

        if (!lv2_feature_on((enum lv2_feature_kind)k))
            continue;
        switch ((enum lv2_feature_kind)k)
        {
        case F_MAP: data = &in->map; break;
        case F_UNMAP: data = &in->unmap; break;
        case F_OPTIONS: data = in->options.opts; break;
        case F_WORKER: data = &in->schedule; break;
        case F_LOG: data = &in->log; break;
        case F_BOUNDED:
        case F_DEFAULT_STATE:
        case LV2_FEATURE_KINDS: break;      // flags: the promise is the presence
        }
        in->features[n].URI = lv2_feature_uri[k];
        in->features[n].data = data;
        in->feature_ptrs[n] = &in->features[n];
        n++;
    }
    in->feature_ptrs[n] = NULL;
}

/* the forge's map, over the process's table: every URID here is mapped before the audio role runs */
static LV2_URID time_map_fn(LV2_URID_Map_Handle h, const char *uri)
{
    (void)h;
    return lv2_urid_lookup(uri);
}

/* the time port's buffer, its URIDs and its forge, made on the main thread at instantiate: 0, or -1 */
static int time_make(struct lv2_instance *in)
{
    const struct lv2_core_config *config = lv2_core_config();

    free(in->time_buf);
    in->time_buf = NULL;
    in->time_have = 0;
    if (in->plugin->time_port < 0)
        return 0;
    in->time_bytes = config->atom_buffer_bytes > TIME_MIN_BYTES ? config->atom_buffer_bytes : TIME_MIN_BYTES;
    in->time_buf = calloc(1, in->time_bytes);
    if (!in->time_buf)
        return -1;
    in->time_map.handle = NULL;
    in->time_map.map = time_map_fn;
    in->time_urid.frame_time = lv2_urid_lookup(LV2_ATOM__frameTime);
    in->time_urid.position = lv2_urid_lookup(LV2_TIME__Position);
    in->time_urid.frame = lv2_urid_lookup(LV2_TIME__frame);
    in->time_urid.speed = lv2_urid_lookup(LV2_TIME__speed);
    in->time_urid.bar = lv2_urid_lookup(LV2_TIME__bar);
    in->time_urid.bar_beat = lv2_urid_lookup(LV2_TIME__barBeat);
    in->time_urid.beat_unit = lv2_urid_lookup(LV2_TIME__beatUnit);
    in->time_urid.beats_per_bar = lv2_urid_lookup(LV2_TIME__beatsPerBar);
    in->time_urid.bpm = lv2_urid_lookup(LV2_TIME__beatsPerMinute);
    lv2_atom_forge_init(&in->time_forge, &in->time_map);
    return 0;
}

static void instance_drop(struct lv2_instance *in)
{
    if (!in->handle)
        return;
    lv2_instance_deactivate(in);
    in->plugin->desc->cleanup(in->handle);
    in->handle = NULL;
    in->worker = NULL;
}

/* instantiate, connect every port, restore the default state: -1 with nothing left instantiated */
static int instance_make(struct lv2_instance *in, double rate, uint32_t min_frames, uint32_t max_frames, char why[LV2_CORE_WHY_MAX])
{
    const struct lv2_plugin *p = in->plugin;
    const LV2_Descriptor *d = p->desc;
    float *audio;
    uint32_t leg, k, i;

    audio = calloc(4u * (size_t)max_frames, sizeof(*audio));
    if (!audio)
    {
        lv2_why_set(why, LV2_CODE_NO_REALISATION);
        return -1;
    }
    free(in->audio);
    in->audio = audio;
    in->max_frames = max_frames;
    in->rate = rate;
    options_build(in, rate, min_frames, max_frames);
    features_build(in);
    if (time_make(in) != 0)
    {
        lv2_why_set(why, LV2_CODE_NO_REALISATION);
        return -1;
    }
    in->handle = d->instantiate(d, rate, p->bundle_path, in->feature_ptrs);
    if (!in->handle)
    {
        lv2_why_set(why, LV2_CODE_NO_REALISATION);
        return -1;
    }
    in->worker = in->ring_mem && d->extension_data ? d->extension_data(LV2_WORKER__interface) : NULL;
    if (in->worker && !in->worker->work)
    {
        d->cleanup(in->handle);
        in->handle = NULL;
        in->worker = NULL;
        lv2_why_set(why, LV2_CODE_NO_REALISATION);
        return -1;
    }
    // every port in index order, to the instance's own memory: an LV2 plugin may dereference ANY port it declares, its
    // own bypass held at the value that keeps it processing, the rows at their values, every other output into memory
    // the host reads
    in->latency_port = 0.0f;
    for (i = 0, k = 0; i < p->n_ports; i++)
    {
        void *at = NULL;

        for (leg = 0; leg < p->legs && !at; leg++)
        {
            if (p->in_ports[leg] == i)
                at = in->audio + leg * (size_t)max_frames;
            else if (p->out_ports[leg] == i)
                at = in->audio + (2u + leg) * (size_t)max_frames;
        }
        if (!at && p->time_port >= 0 && (uint32_t)p->time_port == i)
            at = in->time_buf;
        if (!at && p->latency_port >= 0 && (uint32_t)p->latency_port == i)
            at = &in->latency_port;
        if (!at && k < p->n_controls && p->controls[k].port == i)
            at = &in->controls[k++];
        if (at)
            d->connect_port(in->handle, i, at);
    }
    if (lv2_feature_on(F_DEFAULT_STATE) && lv2_plugin_default_state(p, in->handle, in->feature_ptrs) != 0)
    {
        d->cleanup(in->handle);
        in->handle = NULL;
        in->worker = NULL;
        lv2_why_set(why, LV2_CODE_NO_REALISATION);
        return -1;
    }
    return 0;
}


/*
************************************************************************************************************************
*           GLOBAL FUNCTIONS
************************************************************************************************************************
*/

struct lv2_instance *lv2_instance_new(const struct lv2_plugin *p)
{
    const struct lv2_core_config *config = lv2_core_config();
    struct lv2_instance *in;
    uint32_t k, n;

    if (!p || !p->desc || !config)
        return NULL;
    in = calloc(1, sizeof(*in));
    if (!in)
        return NULL;
    in->plugin = p;
    n = p->n_controls ? p->n_controls : 1u;
    in->controls = calloc(n, sizeof(*in->controls));
    in->shown = calloc(n, sizeof(*in->shown));
    if (lv2_feature_on(F_WORKER))
        in->ring_mem = malloc(2u * (size_t)config->worker_ring_bytes);
    if (!in->controls || !in->shown || (lv2_feature_on(F_WORKER) && !in->ring_mem))
    {
        lv2_instance_free(in);
        return NULL;
    }
    for (k = 0; k < p->n_controls; k++)
    {
        in->controls[k] = p->controls[k].kind == LV2_CONTROL_OUTPUT ? 0.0f : p->controls[k].def;
        atomic_store_explicit(&in->shown[k], float_bits(in->controls[k]), memory_order_relaxed);
    }
    in->schedule.handle = in;
    in->schedule.schedule_work = schedule_work;
    in->map.handle = in;
    in->map.map = inst_map;
    in->unmap.handle = in;
    in->unmap.unmap = inst_unmap;
    in->log.handle = in;
    in->log.printf = inst_log_printf;
    in->log.vprintf = inst_log_vprintf;
    return in;
}

int lv2_instance_activate(struct lv2_instance *in, double rate, uint32_t min_frames, uint32_t max_frames, char why[LV2_CORE_WHY_MAX])
{
    const struct lv2_core_config *config = lv2_core_config();

    if (!in || max_frames == 0 || min_frames > max_frames)
    {
        lv2_why_set(why, LV2_CODE_NO_REALISATION);
        return -1;
    }
    if (in->lv2_active)
        lv2_instance_deactivate(in);
    if (in->handle && (rate != in->rate || max_frames != in->max_frames))
        instance_drop(in);
    // both ends of the rings are stopped: they start empty
    if (in->ring_mem
        && (omx_msgring_init(&in->requests, in->ring_mem, config->worker_ring_bytes) != 0
            || omx_msgring_init(&in->responses, in->ring_mem + config->worker_ring_bytes, config->worker_ring_bytes) != 0))
    {
        lv2_why_set(why, LV2_CODE_NO_REALISATION);
        return -1;
    }
    if (!in->handle && instance_make(in, rate, min_frames, max_frames, why) != 0)
        return -1;
    if (in->plugin->desc->activate)
        in->plugin->desc->activate(in->handle);
    in->lv2_active = 1;
    if (worker_start(in) != 0)
    {
        lv2_instance_deactivate(in);
        lv2_why_set(why, LV2_CODE_NO_REALISATION);
        return -1;
    }
    lv2_why_set(why, "");
    return 0;
}

void lv2_instance_deactivate(struct lv2_instance *in)
{
    if (!in || !in->lv2_active)
        return;
    worker_stop(in);
    if (in->plugin->desc->deactivate)
        in->plugin->desc->deactivate(in->handle);
    in->lv2_active = 0;
}

void lv2_instance_reset(struct lv2_instance *in)
{
    const struct lv2_core_config *config = lv2_core_config();
    const LV2_Descriptor *d;

    if (!in || !in->lv2_active)
        return;
    d = in->plugin->desc;
    // deactivate and activate are LV2 instantiation-class calls: no work() may run beside them, so the worker is
    // stopped and joined first. A request or a response from before the reset belongs to the state it clears: both
    // rings start empty, as at activate
    worker_stop(in);
    if (d->deactivate)
        d->deactivate(in->handle);
    if (in->ring_mem)
    {
        omx_msgring_init(&in->requests, in->ring_mem, config->worker_ring_bytes);
        omx_msgring_init(&in->responses, in->ring_mem + config->worker_ring_bytes, config->worker_ring_bytes);
    }
    if (d->activate)
        d->activate(in->handle);
    worker_start(in);
}

void lv2_instance_free(struct lv2_instance *in)
{
    if (!in)
        return;
    instance_drop(in);
    free(in->audio);
    free(in->controls);
    free(in->shown);
    free(in->ring_mem);
    free(in->time_buf);
    free(in);
}

float *lv2_instance_audio_in(struct lv2_instance *in, uint32_t leg)
{
    return in->audio + leg * (size_t)in->max_frames;
}

float *lv2_instance_audio_out(struct lv2_instance *in, uint32_t leg)
{
    return in->audio + (2u + leg) * (size_t)in->max_frames;
}

void lv2_instance_control_set(struct lv2_instance *in, uint32_t k, float value)
{
    if (k >= in->plugin->n_controls || in->plugin->controls[k].kind != LV2_CONTROL_INPUT)
        return;
    in->controls[k] = value;
    atomic_store_explicit(&in->shown[k], float_bits(value), memory_order_relaxed);
}

float lv2_instance_control_get(const struct lv2_instance *in, uint32_t k)
{
    if (k >= in->plugin->n_controls)
        return 0.0f;
    return bits_float(atomic_load_explicit(&in->shown[k], memory_order_relaxed));
}

/* whether two transports read the same, every field but the frame, which the object carries at each block */
static int time_same(const struct lv2_time *a, const struct lv2_time *b)
{
    return a->playing == b->playing && a->has == b->has && a->bpm == b->bpm && a->bar == b->bar && a->bar_beat == b->bar_beat
           && a->beat_unit == b->beat_unit && a->beats_per_bar == b->beats_per_bar;
}

/* one time:Position object at frame 0 of the open sequence: 0 when it does not fit */
static int time_object(struct lv2_instance *in, const struct lv2_time *t)
{
    LV2_Atom_Forge *f = &in->time_forge;
    const struct lv2_time_urids *u = &in->time_urid;
    LV2_Atom_Forge_Frame obj;

    if (!lv2_atom_forge_frame_time(f, 0) || !lv2_atom_forge_object(f, &obj, 0, u->position))
        return 0;
    if (!lv2_atom_forge_key(f, u->frame) || !lv2_atom_forge_long(f, t->frame))
        return 0;
    if (!lv2_atom_forge_key(f, u->speed) || !lv2_atom_forge_float(f, t->playing ? 1.0f : 0.0f))
        return 0;
    if ((t->has & LV2_TIME_HAS_BEATS)
        && (!lv2_atom_forge_key(f, u->bar) || !lv2_atom_forge_long(f, t->bar)
            || !lv2_atom_forge_key(f, u->bar_beat) || !lv2_atom_forge_float(f, (float)t->bar_beat)))
        return 0;
    if ((t->has & LV2_TIME_HAS_METER)
        && (!lv2_atom_forge_key(f, u->beat_unit) || !lv2_atom_forge_int(f, (int32_t)t->beat_unit)
            || !lv2_atom_forge_key(f, u->beats_per_bar) || !lv2_atom_forge_float(f, (float)t->beats_per_bar)))
        return 0;
    if ((t->has & LV2_TIME_HAS_BPM) && (!lv2_atom_forge_key(f, u->bpm) || !lv2_atom_forge_float(f, (float)t->bpm)))
        return 0;
    lv2_atom_forge_pop(f, &obj);
    return 1;
}

void lv2_instance_time(struct lv2_instance *in, const struct lv2_time *t)
{
    LV2_Atom_Forge *f = &in->time_forge;
    LV2_Atom_Forge_Frame seq;
    const int forge = t && !(in->time_have && time_same(t, &in->time_last));

    if (!in->time_buf)
        return;
    in->time_have = t != NULL;
    if (t)
        in->time_last = *t;
    lv2_atom_forge_set_buffer(f, (uint8_t *)in->time_buf, in->time_bytes);
    if (lv2_atom_forge_sequence_head(f, &seq, in->time_urid.frame_time) && (!forge || time_object(in, t)))
    {
        lv2_atom_forge_pop(f, &seq);
        return;
    }
    // no room for the object (it never happens at TIME_MIN_BYTES): an empty sequence, never half of an object
    lv2_atom_forge_set_buffer(f, (uint8_t *)in->time_buf, in->time_bytes);
    if (lv2_atom_forge_sequence_head(f, &seq, in->time_urid.frame_time))
        lv2_atom_forge_pop(f, &seq);
}

void lv2_instance_run(struct lv2_instance *in, uint32_t n, void (*before)(void *ctx), void *ctx)
{
    const struct lv2_plugin *p = in->plugin;
    uint32_t k;

    if (in->worker && in->worker->work_response)
    {
        uint32_t size;
        const void *msg;

        while ((msg = omx_msgring_peek(&in->responses, &size)) != NULL)
        {
            in->worker->work_response(in->handle, size, msg);
            omx_msgring_pop(&in->responses);
        }
    }
    if (before)
        before(ctx);
    p->desc->run(in->handle, n);
    if (in->worker && in->worker->end_run)
        in->worker->end_run(in->handle);
    omx_hosted_denormals_off();
    if (p->latency_port >= 0)
    {
        const float v = in->latency_port;

        if (isfinite(v) && v >= 0.0f && v < 16777216.0f)       // 2^24: exact in a float
            atomic_store_explicit(&in->latency_frames, (uint32_t)lrintf(v), memory_order_relaxed);
    }
    for (k = 0; k < p->n_controls; k++)
        if (p->controls[k].kind == LV2_CONTROL_OUTPUT)
            atomic_store_explicit(&in->shown[k], float_bits(in->controls[k]), memory_order_relaxed);
}

uint32_t lv2_instance_latency(const struct lv2_instance *in)
{
    return atomic_load_explicit(&in->latency_frames, memory_order_relaxed);
}

void lv2_instance_counters(const struct lv2_instance *in, struct lv2_counters *out)
{
    out->schedule_refused = atomic_load_explicit(&in->schedule_refused, memory_order_relaxed);
    out->responses_refused = atomic_load_explicit(&in->responses_refused, memory_order_relaxed);
    out->respond_strikes = atomic_load_explicit(&in->respond_strikes, memory_order_relaxed);
    out->map_on_audio = atomic_load_explicit(&in->map_on_audio, memory_order_relaxed);
    out->schedule_off_audio = atomic_load_explicit(&in->schedule_off_audio, memory_order_relaxed);
    out->log_on_audio = atomic_load_explicit(&in->log_on_audio, memory_order_relaxed);
}

void lv2_instance_set_role(struct lv2_instance *in, lv2_audio_role_fn is_audio, void *ctx)
{
    atomic_store_explicit(&in->role_ctx, ctx, memory_order_relaxed);
    atomic_store_explicit(&in->is_audio, is_audio, memory_order_release);
}

void lv2_instance_worker_quiesce(struct lv2_instance *in)
{
    const struct timespec poll = { 0, (long)QUIESCE_POLL_US * 1000L };

    // the worker pops a request only after its work() returned and responded: an empty ring is a serviced one
    while (in && in->worker_running && omx_msgring_used(&in->requests) != 0)
        nanosleep(&poll, NULL);
}

int lv2_instance_has_worker(const struct lv2_instance *in)
{
    return in && in->worker_running;
}
