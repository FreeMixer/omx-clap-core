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
#include <lv2/atom/util.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
#include <lv2/patch/patch.h>
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

// the atom sequence header: the atom's own 8 bytes, and the sequence body's unit and pad
#define SEQ_HEADER_BYTES                16u

// one patch:Set or patch:Put on the wire: the event's frame and atom header (16), the object's id and type (8), and its
// two properties, patch:property (an atom:URID) and patch:value (up to 8 bytes), 24 bytes each
#define PATCH_PROP_BYTES                24u
#define PATCH_EVENT_BYTES               (16u + 8u + 2u * PATCH_PROP_BYTES)


/*
************************************************************************************************************************
*           LOCAL DATA TYPES
************************************************************************************************************************
*/

/* the URIDs the patch ports are written and read with, mapped once at creation */
struct patch_urids
{
    LV2_URID set, put, property, value, object, sequence, urid, time_frame;
    LV2_URID types[5];              // by enum lv2_patch_type
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

    // the patch ports: one atom buffer each of atom_cap bytes, connected at instantiate and written at every block
    uint8_t *atom_in, *atom_out;
    uint32_t atom_cap;
    struct patch_urids urids;
    LV2_URID *patch_key;            // per patch parameter: the URID of its property
    float *patch_pending;           // the value a host write carries into the next block
    uint8_t *patch_dirty;           // 1 when the next block carries patch_pending
    _Atomic uint32_t *patch_shown;  // per patch parameter, as bits: the host's value, or the last output's
    uint8_t *patch_out_flag;        // the last run's output set the parameter
    float *patch_out_value;
    _Atomic uint32_t patch_dropped;
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
*           LOCAL FUNCTIONS: THE PATCH PORTS
************************************************************************************************************************
*/

static void put32(uint8_t *at, uint32_t v)
{
    memcpy(at, &v, sizeof(v));
}

static void put64(uint8_t *at, uint64_t v)
{
    memcpy(at, &v, sizeof(v));
}

static uint32_t get32(const uint8_t *at)
{
    uint32_t v;

    memcpy(&v, at, sizeof(v));
    return v;
}

static uint32_t pad8(uint32_t n)
{
    return (n + 7u) & ~7u;
}

/* one property of a patch object: the key, no context, and the value atom (its size and type, then the data) */
static void put_prop(uint8_t *at, LV2_URID key, LV2_URID type, const void *data, uint32_t size)
{
    put32(at, key);
    put32(at + 4, 0);
    put32(at + 8, size);
    put32(at + 12, type);
    memset(at + 16, 0, PATCH_PROP_BYTES - 16u);
    memcpy(at + 16, data, size);
}

/* The value a host write carries, in the type the plugin's property takes. */
static void patch_value_bytes(enum lv2_patch_type type, float v, int64_t *i64, int32_t *i32, double *f64, float *f32, uint32_t *size)
{
    switch (type)
    {
    case LV2_PATCH_FLOAT: *f32 = v; *size = sizeof(*f32); break;
    case LV2_PATCH_DOUBLE: *f64 = (double)v; *size = sizeof(*f64); break;
    case LV2_PATCH_INT: *i32 = (int32_t)lrintf(v); *size = sizeof(*i32); break;
    case LV2_PATCH_LONG: *i64 = (int64_t)llrintf(v); *size = sizeof(*i64); break;
    case LV2_PATCH_BOOL: *i32 = v != 0.0f ? 1 : 0; *size = sizeof(*i32); break;
    }
}

/* Forge the block's input sequence: one patch:Set for every parameter written since the last block, at frame 0, in
 * parameter order. Each block rewrites the whole sequence, so an empty block is an empty sequence. An event that does
 * not fit is dropped and counted; the sequence never waits. */
static void patch_forge_inputs(struct lv2_instance *in)
{
    const struct lv2_plugin *p = in->plugin;
    uint8_t *buf = in->atom_in;
    uint32_t used = SEQ_HEADER_BYTES, j;

    if (!buf)
        return;
    put32(buf, 0);
    put32(buf + 4, in->urids.sequence);
    put32(buf + 8, in->urids.time_frame);
    put32(buf + 12, 0);
    for (j = 0; j < p->n_patches; j++)
    {
        const struct lv2_patch *pp = &p->patches[j];
        uint8_t *ev = buf + used;
        int64_t i64 = 0;
        int32_t i32 = 0;
        double f64 = 0.0;
        float f32 = 0.0f;
        uint32_t size = 0;

        if (!in->patch_dirty[j])
            continue;
        in->patch_dirty[j] = 0;
        if (used + PATCH_EVENT_BYTES > in->atom_cap)
        {
            atomic_fetch_add_explicit(&in->patch_dropped, 1u, memory_order_relaxed);
            continue;
        }
        patch_value_bytes(pp->type, in->patch_pending[j], &i64, &i32, &f64, &f32, &size);
        put64(ev, 0);
        put32(ev + 8, 8u + 2u * PATCH_PROP_BYTES);
        put32(ev + 12, in->urids.object);
        put32(ev + 16, 0);
        put32(ev + 20, in->urids.set);
        put_prop(ev + 24, in->urids.property, in->urids.urid, &in->patch_key[j], sizeof(LV2_URID));
        put_prop(ev + 24 + PATCH_PROP_BYTES, in->urids.value, in->urids.types[pp->type],
                 pp->type == LV2_PATCH_FLOAT ? (const void *)&f32 : pp->type == LV2_PATCH_DOUBLE ? (const void *)&f64
                 : pp->type == LV2_PATCH_LONG ? (const void *)&i64 : (const void *)&i32, size);
        used += PATCH_EVENT_BYTES;
    }
    put32(buf, used - 8u);
}

/* The value of an output patch:value at the type its parameter takes: 1, or 0 when the atom has another type or size. */
static int patch_read_value(enum lv2_patch_type type, uint32_t atom_type, uint32_t expect_type, uint32_t size, const uint8_t *data, float *out)
{
    if (atom_type != expect_type)
        return 0;
    switch (type)
    {
    case LV2_PATCH_FLOAT:
        if (size != 4u)
            return 0;
        memcpy(out, data, 4u);
        return 1;
    case LV2_PATCH_DOUBLE:
    {
        double d;

        if (size != 8u)
            return 0;
        memcpy(&d, data, 8u);
        *out = (float)d;
        return 1;
    }
    case LV2_PATCH_INT:
    {
        int32_t i;

        if (size != 4u)
            return 0;
        memcpy(&i, data, 4u);
        *out = (float)i;
        return 1;
    }
    case LV2_PATCH_LONG:
    {
        int64_t l;

        if (size != 8u)
            return 0;
        memcpy(&l, data, 8u);
        *out = (float)l;
        return 1;
    }
    case LV2_PATCH_BOOL:
    {
        int32_t b;

        if (size != 4u)
            return 0;
        memcpy(&b, data, 4u);
        *out = b ? 1.0f : 0.0f;
        return 1;
    }
    }
    return 0;
}

/* The properties of one patch object: the patch:property URID and the patch:value atom, each when present. Returns
 * 0 when the object's properties run past its size. */
static int patch_object_props(const struct lv2_instance *in, const uint8_t *props, uint32_t plen, LV2_URID *prop, uint32_t *vtype,
                              uint32_t *vsize, const uint8_t **value)
{
    uint32_t q = 0;

    *prop = 0;
    *value = NULL;
    while (q + 16u <= plen)
    {
        const uint8_t *pb = props + q;
        const uint32_t key = get32(pb), size = get32(pb + 8), type = get32(pb + 12);
        uint32_t step;

        if (size > plen - q - 16u)      // the value runs past the object: the object is refused, never read further
            return 0;
        step = 16u + pad8(size);
        if (key == in->urids.property && type == in->urids.urid && size == sizeof(LV2_URID))
            *prop = get32(pb + 16);
        else if (key == in->urids.value)
        {
            *vtype = type;
            *vsize = size;
            *value = pb + 16;
        }
        q += step;
    }
    return 1;
}

/* Read the output sequence the plugin wrote: every patch:Set or patch:Put of a numeric parameter lands in its flag and
 * value, the last one of a block winning. Other objects, other properties and values of another type are ignored. */
static void patch_read_outputs(struct lv2_instance *in)
{
    const struct lv2_plugin *p = in->plugin;
    const uint8_t *buf = in->atom_out;
    uint32_t end, off;

    if (!buf || p->patch_out < 0 || get32(buf + 4) != in->urids.sequence)
        return;
    end = get32(buf) + 8u;
    if (end > in->atom_cap)
        end = in->atom_cap;
    for (off = SEQ_HEADER_BYTES; off + 16u <= end; )
    {
        const uint8_t *body = buf + off + 8u;
        const uint32_t size = get32(body), type = get32(body + 4);
        uint32_t next;
        LV2_URID prop = 0;
        uint32_t vtype = 0, vsize = 0, j;
        const uint8_t *value = NULL;

        if (size < 8u || size > end - off - 16u)     // the event runs past the sequence: the rest is not read
            break;
        next = off + 16u + pad8(size);
        if (type == in->urids.object && (get32(body + 12) == in->urids.set || get32(body + 12) == in->urids.put)
            && patch_object_props(in, body + 16u, size - 8u, &prop, &vtype, &vsize, &value) && value)
        {
            for (j = 0; j < p->n_patches; j++)
            {
                float v;

                if (in->patch_key[j] != prop)
                    continue;
                if (patch_read_value(p->patches[j].type, vtype, in->urids.types[p->patches[j].type], vsize, value, &v))
                {
                    in->patch_out_flag[j] = 1;
                    in->patch_out_value[j] = v;
                    in->patch_pending[j] = v;
                    atomic_store_explicit(&in->patch_shown[j], float_bits(v), memory_order_relaxed);
                }
                break;
            }
        }
        off = next;
    }
}

/*
************************************************************************************************************************
*           LOCAL FUNCTIONS: INSTANTIATE
************************************************************************************************************************
*/

/* The two atom buffers of the patch ports, max(atom_buffer_bytes, rsz:minimumSize) bytes each, zeroed. They are sized here,
 * at instantiate, and never again on the audio thread. 0 when a buffer cannot be had. */
static int patch_buffers(struct lv2_instance *in)
{
    const struct lv2_core_config *config = lv2_core_config();
    const struct lv2_plugin *p = in->plugin;
    uint32_t cap = config->atom_buffer_bytes > p->atom_min ? config->atom_buffer_bytes : p->atom_min;

    free(in->atom_in);
    free(in->atom_out);
    in->atom_in = in->atom_out = NULL;
    in->atom_cap = 0;
    if (p->patch_in < 0 && p->patch_out < 0)
        return 1;
    if (cap < SEQ_HEADER_BYTES + PATCH_EVENT_BYTES)
        cap = SEQ_HEADER_BYTES + PATCH_EVENT_BYTES;
    cap = (cap + 7u) & ~7u;
    in->atom_cap = cap;
    if (p->patch_in >= 0 && !(in->atom_in = calloc(1, cap)))
        return 0;
    if (p->patch_out >= 0 && !(in->atom_out = calloc(1, cap)))
        return 0;
    return 1;
}

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
    if (!patch_buffers(in))
    {
        lv2_why_set(why, LV2_CODE_NO_REALISATION);
        return -1;
    }
    in->max_frames = max_frames;
    in->rate = rate;
    options_build(in, rate, min_frames, max_frames);
    features_build(in);
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
        if (!at && p->patch_in >= 0 && (uint32_t)p->patch_in == i)
            at = in->atom_in;
        if (!at && p->patch_out >= 0 && (uint32_t)p->patch_out == i)
            at = in->atom_out;
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
    // a new handle has never seen a patch value: the first block sends every parameter's value, as the host last had it
    if (p->n_patches)
        memset(in->patch_dirty, 1, p->n_patches);
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
    in->patch_key = calloc(p->n_patches ? p->n_patches : 1u, sizeof(*in->patch_key));
    in->patch_pending = calloc(p->n_patches ? p->n_patches : 1u, sizeof(*in->patch_pending));
    in->patch_dirty = calloc(p->n_patches ? p->n_patches : 1u, sizeof(*in->patch_dirty));
    in->patch_shown = calloc(p->n_patches ? p->n_patches : 1u, sizeof(*in->patch_shown));
    in->patch_out_flag = calloc(p->n_patches ? p->n_patches : 1u, sizeof(*in->patch_out_flag));
    in->patch_out_value = calloc(p->n_patches ? p->n_patches : 1u, sizeof(*in->patch_out_value));
    if (!in->controls || !in->shown || (lv2_feature_on(F_WORKER) && !in->ring_mem) || !in->patch_key
        || !in->patch_pending || !in->patch_dirty || !in->patch_shown || !in->patch_out_flag || !in->patch_out_value)
    {
        lv2_instance_free(in);
        return NULL;
    }
    in->urids.set = lv2_urid_lookup(LV2_PATCH__Set);
    in->urids.put = lv2_urid_lookup(LV2_PATCH__Put);
    in->urids.property = lv2_urid_lookup(LV2_PATCH__property);
    in->urids.value = lv2_urid_lookup(LV2_PATCH__value);
    in->urids.object = lv2_urid_lookup(LV2_ATOM__Object);
    in->urids.sequence = lv2_urid_lookup(LV2_ATOM__Sequence);
    in->urids.urid = lv2_urid_lookup(LV2_ATOM__URID);
    in->urids.time_frame = lv2_urid_lookup(LV2_TIME__frame);
    in->urids.types[LV2_PATCH_FLOAT] = lv2_urid_lookup(LV2_ATOM__Float);
    in->urids.types[LV2_PATCH_DOUBLE] = lv2_urid_lookup(LV2_ATOM__Double);
    in->urids.types[LV2_PATCH_INT] = lv2_urid_lookup(LV2_ATOM__Int);
    in->urids.types[LV2_PATCH_LONG] = lv2_urid_lookup(LV2_ATOM__Long);
    in->urids.types[LV2_PATCH_BOOL] = lv2_urid_lookup(LV2_ATOM__Bool);
    for (k = 0; k < p->n_patches; k++)
    {
        in->patch_key[k] = lv2_urid_lookup(p->patches[k].uri);
        in->patch_pending[k] = p->patches[k].def;
        atomic_store_explicit(&in->patch_shown[k], float_bits(p->patches[k].def), memory_order_relaxed);
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
    free(in->atom_in);
    free(in->atom_out);
    free(in->patch_key);
    free(in->patch_pending);
    free(in->patch_dirty);
    free(in->patch_shown);
    free(in->patch_out_flag);
    free(in->patch_out_value);
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

void lv2_instance_patch_write(struct lv2_instance *in, uint32_t j, float value)
{
    if (j >= in->plugin->n_patches)
        return;
    in->patch_pending[j] = value;
    in->patch_dirty[j] = 1;
    atomic_store_explicit(&in->patch_shown[j], float_bits(value), memory_order_relaxed);
}

float lv2_instance_patch_get(const struct lv2_instance *in, uint32_t j)
{
    if (j >= in->plugin->n_patches)
        return 0.0f;
    return bits_float(atomic_load_explicit(&in->patch_shown[j], memory_order_relaxed));
}

int lv2_instance_patch_changed(const struct lv2_instance *in, uint32_t j, float *value)
{
    if (j >= in->plugin->n_patches || !in->patch_out_flag[j])
        return 0;
    *value = in->patch_out_value[j];
    return 1;
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
    memset(in->patch_out_flag, 0, p->n_patches);
    if (before)
        before(ctx);
    patch_forge_inputs(in);
    if (in->atom_out)
    {
        put32(in->atom_out, in->atom_cap - 8u);
        put32(in->atom_out + 4, in->urids.sequence);
        put32(in->atom_out + 8, in->urids.time_frame);
        put32(in->atom_out + 12, 0);
    }
    p->desc->run(in->handle, n);
    if (in->worker && in->worker->end_run)
        in->worker->end_run(in->handle);
    patch_read_outputs(in);
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
    out->patch_dropped = atomic_load_explicit(&in->patch_dropped, memory_order_relaxed);
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
