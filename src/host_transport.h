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
 * @brief omx-clap-host's one transport: what set_bpm, set_bpb and transport set, and the record each block hands a plugin. */

/*
 * The host keeps one transport for every plugin it runs. The control thread writes what the commands ask for, an
 * intent: the tempo, the beats per bar, play or stop, and the anchor, the frame of the graph the command arrived on
 * and the song position there. Every audio thread reads the intent whole once a block and works the position of its
 * block's first frame out of the frames played since the anchor, never by adding a block's increment to the last, so
 * the position carries no drift. With the JACK transport followed, the block's record comes from the JACK position
 * instead (host_transport_follow).
 *
 * Header only, not installed. The functions marked RT allocate, lock and call out to nothing.
 */
#ifndef HOST_TRANSPORT_H
#define HOST_TRANSPORT_H

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include <clap/events.h>
#include <clap/fixedpoint.h>

#include "clap_stage.h"

/** mod-host's ranges for set_bpm and set_bpb: a value outside them answers ERR_JACK_VALUE_OUT_OF_RANGE */
#define HOST_TRANSPORT_BPM_MIN      20.0
#define HOST_TRANSPORT_BPM_MAX      280.0
#define HOST_TRANSPORT_BPB_MIN      1.0
#define HOST_TRANSPORT_BPB_MAX      16.0
/** the beats per bar before any command sets them, mod-host's */
#define HOST_TRANSPORT_BPB_DEFAULT  4.0
/** the note value of a beat: the denominator of the time signature the host hands a plugin */
#define HOST_TRANSPORT_BEAT_TYPE    4u

/** What the commands asked for: the control thread's to write, whole, with host_transport_publish. */
struct host_transport_intent
{
    int tempo_set;              ///< a tempo was given: until then a plugin is handed no transport
    int rolling;                ///< playing
    double bpm;                 ///< the tempo
    double bpb;                 ///< the beats per bar
    uint32_t anchor_frame;      ///< the frame of the graph the last change arrived on
    double anchor_beats;        ///< the song position there, in beats
    double anchor_seconds;      ///< the song position there, in seconds
};

/** The published intent: the fields as atomic words, guarded by a sequence that is odd while a publish is under way. */
struct host_transport
{
    _Atomic uint32_t seq;
    _Atomic uint32_t tempo_set;
    _Atomic uint32_t rolling;
    _Atomic uint32_t anchor_frame;
    _Atomic uint64_t bpm_bits;
    _Atomic uint64_t bpb_bits;
    _Atomic uint64_t beats_bits;
    _Atomic uint64_t seconds_bits;
};

/** The JACK position one block reads, as plain values: what host_transport_follow needs of jack_position_t. */
struct host_jack_position
{
    int rolling;                ///< the transport rolls
    uint32_t frame;             ///< its frame
    int bbt;                    ///< a timebase master gives bars and beats: the fields below are valid
    int32_t bar;                ///< the bar, from 1
    int32_t beat;               ///< the beat in the bar, from 1
    int32_t tick;               ///< the tick in the beat
    double ticks_per_beat;      ///< the ticks a beat has
    double bpm;                 ///< the tempo
    double bpb;                 ///< the beats per bar
    double beat_type;           ///< the note value of a beat
};

static inline uint64_t host_transport_bits(double v)
{
    uint64_t b;

    memcpy(&b, &v, sizeof b);
    return b;
}

static inline double host_transport_double(uint64_t b)
{
    double v;

    memcpy(&v, &b, sizeof v);
    return v;
}

/** Control thread: publish `t` whole; seen by the next block that reads it. */
static inline void host_transport_publish(struct host_transport *p, const struct host_transport_intent *t)
{
    const uint32_t seq = atomic_load_explicit(&p->seq, memory_order_relaxed);

    atomic_store_explicit(&p->seq, seq + 1u, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&p->tempo_set, (uint32_t)(t->tempo_set != 0), memory_order_relaxed);
    atomic_store_explicit(&p->rolling, (uint32_t)(t->rolling != 0), memory_order_relaxed);
    atomic_store_explicit(&p->anchor_frame, t->anchor_frame, memory_order_relaxed);
    atomic_store_explicit(&p->bpm_bits, host_transport_bits(t->bpm), memory_order_relaxed);
    atomic_store_explicit(&p->bpb_bits, host_transport_bits(t->bpb), memory_order_relaxed);
    atomic_store_explicit(&p->beats_bits, host_transport_bits(t->anchor_beats), memory_order_relaxed);
    atomic_store_explicit(&p->seconds_bits, host_transport_bits(t->anchor_seconds), memory_order_relaxed);
    atomic_store_explicit(&p->seq, seq + 2u, memory_order_release);
}

/** RT: read the published intent into `out`, whole. 0 when no try reads it whole: `out` is then left as it was, the
 * last intent the caller read. */
static inline int host_transport_read(const struct host_transport *p, struct host_transport_intent *out)
{
    for (int tries = 0; tries < 4; tries++)
    {
        const uint32_t s1 = atomic_load_explicit(&p->seq, memory_order_acquire);
        struct host_transport_intent t;

        if (s1 & 1u)
            continue;
        t.tempo_set = (int)atomic_load_explicit(&p->tempo_set, memory_order_relaxed);
        t.rolling = (int)atomic_load_explicit(&p->rolling, memory_order_relaxed);
        t.anchor_frame = atomic_load_explicit(&p->anchor_frame, memory_order_relaxed);
        t.bpm = host_transport_double(atomic_load_explicit(&p->bpm_bits, memory_order_relaxed));
        t.bpb = host_transport_double(atomic_load_explicit(&p->bpb_bits, memory_order_relaxed));
        t.anchor_beats = host_transport_double(atomic_load_explicit(&p->beats_bits, memory_order_relaxed));
        t.anchor_seconds = host_transport_double(atomic_load_explicit(&p->seconds_bits, memory_order_relaxed));
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&p->seq, memory_order_relaxed) != s1)
            continue;
        *out = t;
        return 1;
    }
    return 0;
}

/** Any thread: the song position of `t` at frame `frame` of a graph at `rate`, in beats and in seconds. The frames
 * since the anchor are counted modulo 2^32, as JACK's frame counter wraps; a frame before the anchor (a block that
 * began before the command arrived) is the anchor's position. */
static inline void host_transport_at(const struct host_transport_intent *t, uint32_t frame, double rate, double *beats, double *seconds)
{
    const int32_t since = (int32_t)(frame - t->anchor_frame);
    const double elapsed = t->rolling && since > 0 ? (double)since : 0.0;

    *seconds = t->anchor_seconds + elapsed / rate;
    *beats = t->anchor_beats + elapsed / rate * t->bpm / 60.0;
}

/** The record a plugin is handed for a song position of `beats` and `seconds`: tempo, beats and seconds timelines and
 * the time signature, and IS_PLAYING while `rolling`. CLAP's fixed point, the bar from 0. */
static inline void host_transport_fill(struct omx_clap_transport *rec, double beats, double seconds, double bpm, double bpb,
                                       uint32_t beat_type, int rolling)
{
    const double bar = floor(beats / bpb + 1e-9);

    memset(rec, 0, sizeof *rec);
    rec->flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE |
                 CLAP_TRANSPORT_HAS_TIME_SIGNATURE | (rolling ? CLAP_TRANSPORT_IS_PLAYING : 0u);
    rec->tempo = bpm;
    rec->song_pos_beats = (clap_beattime)llround(beats * (double)CLAP_BEATTIME_FACTOR);
    rec->song_pos_seconds = (clap_sectime)llround(seconds * (double)CLAP_SECTIME_FACTOR);
    rec->bar_number = (int32_t)bar;
    rec->bar_start = (clap_beattime)llround(bar * bpb * (double)CLAP_BEATTIME_FACTOR);
    rec->tsig_num = (uint16_t)lround(bpb);
    rec->tsig_denom = (uint16_t)beat_type;
}

/** RT: the record of the host's own transport `t` for the block that starts at frame `frame`; none (flags 0, the
 * plugin is handed no transport) until a tempo is given. */
static inline void host_transport_own(const struct host_transport_intent *t, uint32_t frame, double rate, struct omx_clap_transport *rec)
{
    double beats, seconds;

    if (!t->tempo_set)
    {
        memset(rec, 0, sizeof *rec);
        return;
    }
    host_transport_at(t, frame, rate, &beats, &seconds);
    host_transport_fill(rec, beats, seconds, t->bpm, t->bpb, HOST_TRANSPORT_BEAT_TYPE, t->rolling);
}

/** RT: the record of the JACK transport at `pos` for a graph at `rate`. With bars and beats from a timebase master, the
 * master's tempo, signature and position; without, the beats are the seconds at the host's tempo `t`, and with no
 * tempo given only the seconds and the play state are handed on. */
static inline void host_transport_follow(const struct host_jack_position *pos, const struct host_transport_intent *t, double rate,
                                         struct omx_clap_transport *rec)
{
    const double seconds = (double)pos->frame / rate;

    if (pos->bbt && pos->bpm > 0.0 && pos->bpb > 0.0 && pos->ticks_per_beat > 0.0)
    {
        const double beats = (double)(pos->bar - 1) * pos->bpb + (double)(pos->beat - 1) + (double)pos->tick / pos->ticks_per_beat;

        host_transport_fill(rec, beats, seconds, pos->bpm, pos->bpb, pos->beat_type > 0.0 ? (uint32_t)lround(pos->beat_type) : HOST_TRANSPORT_BEAT_TYPE,
                            pos->rolling);
    }
    else if (t->tempo_set)
    {
        host_transport_fill(rec, seconds * t->bpm / 60.0, seconds, t->bpm, t->bpb, HOST_TRANSPORT_BEAT_TYPE, pos->rolling);
    }
    else
    {
        memset(rec, 0, sizeof *rec);
        rec->flags = CLAP_TRANSPORT_HAS_SECONDS_TIMELINE | (pos->rolling ? CLAP_TRANSPORT_IS_PLAYING : 0u);
        rec->song_pos_seconds = (clap_sectime)llround(seconds * (double)CLAP_SECTIME_FACTOR);
    }
}

/** Control thread: the intent after a command that plays (`rolling`), stops, or sets the tempo `bpm` or the beats per
 * bar `bpb` (0 for one it leaves as it was), arriving at frame `now`: re-anchored where `t` has the song by then, so the
 * beats played stay as they were. The first tempo given makes the transport exist, at 0. */
static inline struct host_transport_intent host_transport_change(const struct host_transport_intent *t, uint32_t now, double rate,
                                                                 int rolling, double bpm, double bpb)
{
    struct host_transport_intent next = *t;

    host_transport_at(t, now, rate, &next.anchor_beats, &next.anchor_seconds);
    if (!t->tempo_set)
        next.anchor_beats = next.anchor_seconds = 0.0;
    next.anchor_frame = now;
    next.rolling = rolling;
    if (bpm > 0.0)
    {
        next.bpm = bpm;
        next.tempo_set = 1;
    }
    if (bpb > 0.0)
        next.bpb = bpb;
    if (next.bpb <= 0.0)
        next.bpb = HOST_TRANSPORT_BPB_DEFAULT;
    return next;
}

#endif /* HOST_TRANSPORT_H */
