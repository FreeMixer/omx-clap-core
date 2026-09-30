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
* The format-free RT stage of a hosted plugin, part of libomx-clap-core: the private bounce, the commanded bypass, the
* one-block constant-gain crossfade and the deliver, the clamp, the non-finite strike, the finite scan, the preamble
* and the tail of a run, the warm-up drive and the parameter queue. clap_stage.h is the CLAP body over it.
*
* Every function here is called from the RT body of a stage and from nowhere else: no allocation, no lock, no system
* call, pure arithmetic over the caller's buffers. The functions are inline so the consumer's own RT thread runs them
* without a call through the library; the layout of the structures is what a consumer and the library agree on.
*
************************************************************************************************************************
*/

#ifndef HOSTED_STAGE_H
#define HOSTED_STAGE_H


/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <math.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "clap_host_limits.h"


/*
************************************************************************************************************************
*           CONFIGURATION DEFINES
************************************************************************************************************************
*/

/* A consumer that states contracts defines this before including the library's headers: the condition is a
 * postcondition of the function that evaluates it, the label its name. Without it nothing is evaluated. */
#ifndef CLAP_HOST_POST
#define CLAP_HOST_POST(cond, label) ((void)0)
#endif

/* what a stage guards, per stage: the clamp at CLAP_HOST_CLAMP_DBFS and the scan for non-finite output with its strike */
#define OMX_HOSTED_GUARD_CLAMP          1u
#define OMX_HOSTED_GUARD_NONFINITE      2u
#define OMX_HOSTED_GUARDS_ALL           (OMX_HOSTED_GUARD_CLAMP | OMX_HOSTED_GUARD_NONFINITE)


/*
************************************************************************************************************************
*           DATA TYPES
************************************************************************************************************************
*/

/* Why a hosted stage stopped running its plugin for good. Sticky; the control thread reads it. */
enum omx_hosted_fault
{
    OMX_HOSTED_FAULT_NONE = 0,
    OMX_HOSTED_FAULT_NONFINITE = 1      // a non-finite block, or a CLAP_PROCESS_ERROR block, struck the counter out
};

/*
 * The core of a hosted stage, embedded by each format's stage as `h` at offset 0: the private bounce, the legs, the
 * commanded bypass, the body the lane carried, the sticky fault and the counters every hosted body keeps. The
 * format's own state lives beside it, never in it.
 */
struct omx_hosted_stage
{
    // bound by the control thread before publish
    uint32_t n_in, n_out;               // audio legs: n_in 0, 1 or 2 (0 is an instrument), n_out 1 or 2
    uint32_t max_block;                 // the bounce capacity, in frames
    float *in_l, *in_r, *out_l, *out_r; // the private bounce, max_block each, four distinct buffers
    uint32_t guards;                    // OMX_HOSTED_GUARD_*, set before publish

    // control thread -> RT
    _Atomic uint32_t bypass;            // commanded bypass: nonzero = the lane carries dry

    // RT-owned; the control thread reads them relaxed
    _Atomic uint32_t rendered_wet;      // the body the lane carried on the last block
    _Atomic uint32_t fault;             // enum omx_hosted_fault
    _Atomic uint32_t runs;              // live plugin calls
    _Atomic uint32_t nonfinite_blocks;  // discarded blocks: non-finite output, a CLAP process error
    _Atomic uint32_t clamped_samples;
    _Atomic uint32_t oversize_blocks;   // a block longer than the bounce: passed through, not run
};

/* The private bounce a hosted stage is bound to: four distinct buffers of `max_block` frames. */
struct omx_hosted_bounce
{
    float *in_l, *in_r, *out_l, *out_r;
    uint32_t max_block;
};

/* What the RT body does with a block: omx_hosted_run_begin's verdict. */
enum omx_hosted_verdict
{
    OMX_HOSTED_SKIP = 0,    // oversize: passed through untouched and counted; the lane carried dry
    OMX_HOSTED_IDLE = 1,    // steady bypass: plugin idle, lane bit-identical
    OMX_HOSTED_GO = 2       // the dry block is in the bounce: run the plugin, then omx_hosted_run_end
};

/* One queued parameter write: a CLAP param id (or an LV2 control's index), its value in the plugin's own units, the
 * cookie get_info returned for the id. */
struct omx_hosted_param_record
{
    uint32_t id;
    double value;
    void *cookie;
};

/* Lock-free single producer (control thread), single consumer (RT); cap a power of two; owns no memory. */
struct omx_hosted_param_queue
{
    struct omx_hosted_param_record *recs;
    uint32_t cap;
    _Atomic uint32_t head;              // consumer: the next record to pop
    _Atomic uint32_t tail;              // producer: the next slot to push
};


/*
************************************************************************************************************************
*           THE DSP WORDS THE CORE SPELLS
*
* libomx-clap-core cannot include a DSP library of its own: each word below is the arithmetic of the word of the same
* meaning in the console's primitives library, and the console proves it bit-identical with a test of its own.
************************************************************************************************************************
*/

/* decibels to linear amplitude, 10^(dB/20) */
static inline float omx_hosted_db_to_lin(float db)
{
    return powf(10.0f, db * 0.05f);
}

/* whether every sample of a block is finite; a NULL block is */
static inline int omx_hosted_block_finite(const float *b, uint32_t n)
{
    uint32_t i;

    if (b == NULL)
        return 1;
    for (i = 0; i < n; i++)
    {
        const float x = b[i];
        if (!(x - x == 0.0f))   // false for NaN and for both infinities
            return 0;
    }
    return 1;
}

/* flush-to-zero and denormals-are-zero for the calling thread; the mode is per thread */
static inline void omx_hosted_denormals_off(void)
{
#if defined(__i386__) || defined(__x86_64__)
    unsigned int mxcsr = __builtin_ia32_stmxcsr();
    __builtin_ia32_ldmxcsr(mxcsr | 0x8040);
#elif defined(__aarch64__)
    uint64_t cw;
    __asm__ __volatile__ (
        "mrs    %0, fpcr                            \n"
        "orr    %0, %0, #0x1000000                  \n"
        "msr    fpcr, %0                            \n"
        "isb                                        \n"
        : "=r"(cw) :: "memory");
#elif defined(__arm__)
    uint32_t cw;
    __asm__ __volatile__ (
        "vmrs   %0, fpscr                           \n"
        "orr    %0, %0, #0x1000000                  \n"
        "vmsr   fpscr, %0                           \n"
        : "=r"(cw) :: "memory");
#endif
}


/*
************************************************************************************************************************
*           THE STAGE
************************************************************************************************************************
*/

/* The counter store: one writer, so a relaxed load-add-store needs no read-modify-write. */
static inline void omx_hosted_count(_Atomic uint32_t *c, uint32_t by)
{
    atomic_store_explicit(c, atomic_load_explicit(c, memory_order_relaxed) + by, memory_order_relaxed);
}

/*
 * The one-block crossfade from `from` to `to`, constant-gain linear: at sample i the gains are 1 - (1/n)*i and
 * (1/n)*i and they sum to exactly 1, so two identical bodies read back bit-identical (from + (to - from) * g makes that
 * exact, not merely close). `dst` may not alias `from` or `to`.
 */
static inline void omx_hosted_xfade(float *dst, const float *from, const float *to, uint32_t n)
{
    const float step = (1.0f - 0.0f) / (float)n;
    uint32_t i;

    for (i = 0; i < n; i++)
        dst[i] = from[i] + (to[i] - from[i]) * (0.0f + step * (float)i);
}

/* Clamp one leg to the ceiling, returning how many samples it moved. Branch-free. */
static inline uint32_t omx_hosted_clamp(float *b, uint32_t n)
{
    const float c = omx_hosted_db_to_lin(CLAP_HOST_CLAMP_DBFS);
    uint32_t moved = 0;
    uint32_t i;

    for (i = 0; i < n; i++)
    {
        const float x = b[i];
        moved += (uint32_t)(x > c) + (uint32_t)(x < -c);
        b[i] = fminf(fmaxf(x, -c), c);
    }
    return moved;
}

/* A discarded block: count it, and at the strike count fault the stage for good. The lane already holds the dry
 * block; the caller marks the body dry so the next usable block fades back in. */
static inline void omx_hosted_strike(_Atomic uint32_t *blocks, _Atomic uint32_t *fault)
{
    omx_hosted_count(blocks, 1);
    if (atomic_load_explicit(blocks, memory_order_relaxed) >= CLAP_HOST_NON_FINITE_STRIKES)
        atomic_store_explicit(fault, OMX_HOSTED_FAULT_NONFINITE, memory_order_relaxed);
}

/*
 * Deliver the block onto the lane, choosing the body: a steady wet block is copied, and every change of body, wet after
 * dry (the first block after publish, a recovery) or dry after wet (a commanded bypass), is one crossfade between the dry
 * input (in_*) and the wet output (out_l, wet_r). `r` is NULL on a mono lane, and then only `l` is written.
 */
static inline void omx_hosted_deliver(int want_wet, int was_wet, float *l, float *r, const float *in_l, const float *in_r,
                                      const float *out_l, const float *wet_r, uint32_t n)
{
    if (want_wet && was_wet)
    {
        memcpy(l, out_l, n * sizeof(float));
        if (r)
            memcpy(r, wet_r, n * sizeof(float));
    }
    else if (want_wet)
    {
        omx_hosted_xfade(l, in_l, out_l, n);
        if (r)
            omx_hosted_xfade(r, in_r, wet_r, n);
    }
    else
    {
        omx_hosted_xfade(l, out_l, in_l, n);
        if (r)
            omx_hosted_xfade(r, wet_r, in_r, n);
    }
}

/*
 * Control thread, before the plugin exists: zero the whole format stage (`size` bytes, of which the hosted core is the
 * head: every format header asserts offsetof(..., h) == 0), bind its bounce and guard everything. Returns -1 (stage
 * unusable) unless the four buffers are present and distinct, out-of-place being the stage's first guarantee, and the
 * capacity is non-zero.
 */
static inline int omx_hosted_stage_init(void *stage, size_t size, const struct omx_hosted_bounce *bounce)
{
    struct omx_hosted_stage *h;
    const float *b[4];
    int i, j;

    if (stage == NULL || bounce == NULL)
        return -1;
    memset(stage, 0, size);
    b[0] = bounce->in_l;
    b[1] = bounce->in_r;
    b[2] = bounce->out_l;
    b[3] = bounce->out_r;
    for (i = 0; i < 4; i++)
    {
        if (b[i] == NULL)
            return -1;
        for (j = 0; j < i; j++)
            if (b[i] == b[j])
                return -1;
    }
    if (bounce->max_block == 0)
        return -1;
    h = (struct omx_hosted_stage *)stage;
    h->in_l = bounce->in_l;
    h->in_r = bounce->in_r;
    h->out_l = bounce->out_l;
    h->out_r = bounce->out_r;
    h->max_block = bounce->max_block;
    h->guards = OMX_HOSTED_GUARDS_ALL;
    return 0;
}

/* RT: are the plugin's output legs finite? A mono body writes only out_l. */
static inline int omx_hosted_out_finite(const struct omx_hosted_stage *h, uint32_t n)
{
    if (!(h->guards & OMX_HOSTED_GUARD_NONFINITE))
        return 1;
    return omx_hosted_block_finite(h->out_l, n) && omx_hosted_block_finite(h->n_out == 2 ? h->out_r : NULL, n);
}

/*
 * RT, the preamble both bodies share: a block longer than the bounce is passed through untouched and counted (the
 * plugin never sees a block it was not sized for); `want_wet` reads the commanded bypass and the sticky fault; a steady
 * bypass touches nothing; otherwise the dry block is copied into the bounce (in_r the dry R, or the mono mirror).
 */
static inline enum omx_hosted_verdict omx_hosted_run_begin(struct omx_hosted_stage *h, const float *l, const float *r,
                                                           uint32_t n, uint32_t *want_wet)
{
    if (n > h->max_block)
    {
        omx_hosted_count(&h->oversize_blocks, 1);
        atomic_store_explicit(&h->rendered_wet, 0, memory_order_release);   // the lane carried dry: the next block fades back in
        return OMX_HOSTED_SKIP;
    }
    *want_wet = atomic_load_explicit(&h->bypass, memory_order_acquire) == 0u
                && atomic_load_explicit(&h->fault, memory_order_relaxed) == OMX_HOSTED_FAULT_NONE;
    if (!*want_wet && !atomic_load_explicit(&h->rendered_wet, memory_order_relaxed))
        return OMX_HOSTED_IDLE;
    memcpy(h->in_l, l, n * sizeof(float));
    memcpy(h->in_r, r ? r : l, n * sizeof(float));
    return OMX_HOSTED_GO;
}

/*
 * RT, the tail both bodies share: count the run; an unusable block is discarded (the lane already holds the dry block:
 * counted, and struck when the stage guards for non-finite output); a usable one is clamped on both legs when the stage
 * clamps, counted, and delivered as the body `want_wet` names, one crossfade on every change of body; the lane is finite
 * afterwards.
 */
static inline void omx_hosted_run_end(struct omx_hosted_stage *h, int usable, uint32_t want_wet, float *l, float *r, uint32_t n)
{
    const float *wet_r;

    omx_hosted_count(&h->runs, 1);
    if (!usable)
    {
        if (h->guards & OMX_HOSTED_GUARD_NONFINITE)
            omx_hosted_strike(&h->nonfinite_blocks, &h->fault);
        else
            omx_hosted_count(&h->nonfinite_blocks, 1);
        atomic_store_explicit(&h->rendered_wet, 0, memory_order_release);
        return;
    }
    if (h->guards & OMX_HOSTED_GUARD_CLAMP)
    {
        uint32_t moved = omx_hosted_clamp(h->out_l, n);

        if (h->n_out == 2)
            moved += omx_hosted_clamp(h->out_r, n);
        if (moved)
            omx_hosted_count(&h->clamped_samples, moved);
    }
    wet_r = h->n_out == 2 ? h->out_r : h->out_l;
    omx_hosted_deliver((int)want_wet, (int)atomic_load_explicit(&h->rendered_wet, memory_order_relaxed), l, r, h->in_l,
                       h->in_r, h->out_l, wet_r, n);
    atomic_store_explicit(&h->rendered_wet, want_wet, memory_order_release);
    CLAP_HOST_POST((omx_hosted_block_finite(l, n) && omx_hosted_block_finite(r, n)) || !(h->guards & OMX_HOSTED_GUARD_NONFINITE), "finite");
}

/*
 * Control thread, the warm-up: CLAP_HOST_WARMUP_BLOCKS blocks of `n` frames through `block`, the format's own drain, run
 * and MXCSR path, the first half held at CLAP_HOST_WARMUP_LEVEL_DBFS, the second half silent, touching every bounce page.
 * Returns how many blocks `block` reported unusable (anything but 0 means do not publish), or UINT32_MAX when `n` does
 * not fit the bounce. The live counters are untouched.
 */
static inline uint32_t omx_hosted_prime(struct omx_hosted_stage *h, uint32_t n, int (*block)(void *ctx, uint32_t n), void *ctx)
{
    uint32_t bad = 0;
    uint32_t b, i;

    if (n == 0 || n > h->max_block)
        return UINT32_MAX;
    for (b = 0; b < CLAP_HOST_WARMUP_BLOCKS; b++)
    {
        const float v = b < CLAP_HOST_WARMUP_BLOCKS / 2u ? omx_hosted_db_to_lin(CLAP_HOST_WARMUP_LEVEL_DBFS) : 0.0f;

        for (i = 0; i < n; i++)
            h->in_l[i] = h->in_r[i] = v;
        if (!block(ctx, n))
            bad++;
    }
    return bad;
}

static inline int omx_hosted_queue_init(struct omx_hosted_param_queue *q, struct omx_hosted_param_record *recs, uint32_t cap)
{
    if (q == NULL || recs == NULL || cap == 0 || (cap & (cap - 1u)) != 0)
        return -1;
    q->recs = recs;
    q->cap = cap;
    atomic_store_explicit(&q->head, 0, memory_order_relaxed);
    atomic_store_explicit(&q->tail, 0, memory_order_relaxed);
    return 0;
}

/* Control thread: enqueue one write. -1 when the ring is full or unbound. */
static inline int omx_hosted_queue_push(struct omx_hosted_param_queue *q, uint32_t id, double value, void *cookie)
{
    const uint32_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    const uint32_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    struct omx_hosted_param_record *r;

    if (q->recs == NULL || tail - head >= q->cap)
        return -1;
    r = &q->recs[tail & (q->cap - 1u)];
    r->id = id;
    r->value = value;
    r->cookie = cookie;
    atomic_store_explicit(&q->tail, tail + 1u, memory_order_release);
    return 0;
}

/* RT: pop up to `max` records, handing each to `sink(ctx, record)` in order. Returns how many were handed over; the
 * surplus stays queued. One load of tail, one store of head. An unbound ring drains nothing. */
static inline uint32_t omx_hosted_queue_drain(struct omx_hosted_param_queue *q, uint32_t max,
                                              void (*sink)(void *ctx, const struct omx_hosted_param_record *r), void *ctx)
{
    uint32_t head, tail, n = 0;

    if (q->recs == NULL)
        return 0;
    head = atomic_load_explicit(&q->head, memory_order_relaxed);
    tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    while (head != tail && n < max)
    {
        sink(ctx, &q->recs[head & (q->cap - 1u)]);
        head++;
        n++;
    }
    atomic_store_explicit(&q->head, head, memory_order_release);
    return n;
}

/* How many records wait in the ring (either thread, advisory). */
static inline uint32_t omx_hosted_queue_pending(const struct omx_hosted_param_queue *q)
{
    if (q->recs == NULL)
        return 0;
    return atomic_load_explicit(&q->tail, memory_order_acquire) - atomic_load_explicit(&q->head, memory_order_acquire);
}


/*
************************************************************************************************************************
*           END HEADER
************************************************************************************************************************
*/

#endif
