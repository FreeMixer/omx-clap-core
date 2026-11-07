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
 * @brief What the reader of a plugin's meters does with a value once the plugin has handed it over (docs/meters.md).
 *
 * The plugin holds: a meter that declares OMX_CLAP_METER_IS_HELD keeps the deepest (the peak, for a level) of every
 * block since the last read. The reader recovers: it reads at the host read rate omx-dsp declares (data/primitives/
 * meter.json, hostReadHz), steps a gain reduction by omx_meter_gr_step over the time elapsed since its last read, and
 * keeps the plugin's clip word lit until it is cleared. Control thread only: nothing here is called from the audio role.
 */

#ifndef METER_READER_H
#define METER_READER_H

#include <math.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <omxdsp/mix/omx_meter_view.h>

/* the rate a host reads a plugin's held meter at, and the idle interval that gives it: both from omx-dsp's declaration,
 * METER_VIEW.hostReadHz in data/primitives/meter.json, which the installed omxdsp_definitions.h carries */
#define METER_READ_HZ           OMX_METER_VIEW_HOST_READ_HZ
#define METER_READ_INTERVAL_MS  (1000 / METER_READ_HZ)
_Static_assert(METER_READ_HZ > 0 && 1000 % METER_READ_HZ == 0, "the host read rate divides a second into whole milliseconds");

/* the release pole of a gain reduction the reader steps: 0 lands on the held value as soon as the hold is spent. The
 * plugin's own gain recovery is already in what it holds, so the reader adds the hold and no second release */
#define METER_GR_RELEASE_POLE   0.0f
/* the deepest reduction the reader carries, dB */
#define METER_GR_FLOOR_DB       (-200.0f)

/* the controller connection a monitor belongs to: its socket, and the kernel's cookie of that socket, which no later
 * socket repeats (a descriptor number is reused by the next connection, the cookie is not) */
typedef struct METER_CONN_T {
    int fd;
    uint64_t cookie;
} meter_conn_t;

/* the connection on `fd` now; fd < 0 is none, which is never alive */
static inline meter_conn_t meter_conn_of(int fd)
{
    meter_conn_t conn = { -1, 0 };

#ifdef SO_COOKIE
    uint64_t cookie = 0;
    socklen_t size = sizeof(cookie);

    if (fd >= 0 && getsockopt(fd, SOL_SOCKET, SO_COOKIE, &cookie, &size) == 0)
    {
        conn.fd = fd;
        conn.cookie = cookie;
    }
#else
    conn.fd = fd;
#endif
    return conn;
}

/* whether the connection a monitor was set on is the one open on that descriptor still: a closed one, or the descriptor
 * taken by a later connection, is not */
static inline int meter_conn_alive(const meter_conn_t *conn)
{
    const meter_conn_t now = meter_conn_of(conn->fd);

    return conn->fd >= 0 && now.fd == conn->fd && now.cookie == conn->cookie;
}

/* the state of one value the reader sends: a channel of a meter, or the meter's clip word */
typedef struct METER_TAP_T {
    int gain_reduction;         /* the meter's kind: stepped by omx_meter_gr_step */
    int held;                   /* the plugin holds the value until it is read; 0: it gives what it publishes (unheld) */
    int clip_word;              /* this value is the meter's clip word: 0 or 1, latched here until cleared */
    int clip_latched;
    int started;                /* a read has stepped the reduction */
    double last_s;              /* when it did */
    struct omx_meter_gr gr;
    meter_conn_t owner;         /* the connection that monitored it */
} meter_tap_t;

/* seconds on a clock that does not step back */
static inline double meter_reader_now_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* a tap that starts again: no reduction, no hold, no clip, the next read the first */
static inline void meter_tap_reset(meter_tap_t *tap)
{
    tap->clip_latched = 0;
    tap->started = 0;
    tap->last_s = 0.0;
    memset(&tap->gr, 0, sizeof(tap->gr));
}

/* a monitor set on `conn`, or the end of one */
static inline void meter_tap_watch(meter_tap_t *tap, meter_conn_t conn)
{
    meter_tap_reset(tap);
    tap->owner = conn;
}

/* the clip word's clear (the operator's gesture); a reduction and a level are not touched */
static inline void meter_tap_clip_clear(meter_tap_t *tap)
{
    tap->clip_latched = 0;
}

/* The value to send for what the plugin gave at `now_s`. A clip word is lit by any value of 0.5 or more and stays lit.
 * A gain reduction is the deepest the plugin held since the last read (a foreign plugin's is what it publishes), held to
 * 0 dB or less, stepped by omx_meter_gr_step over the time since the last read at the instance's `rate`. A level is
 * what it is. */
static inline float meter_tap_value(meter_tap_t *tap, double raw, double now_s, double rate)
{
    if (tap->clip_word)
    {
        if (raw >= 0.5)
            tap->clip_latched = 1;
        return tap->clip_latched ? 1.0f : 0.0f;
    }
    if (!tap->gain_reduction)
        return (float)raw;
    {
        /* held to a finite reduction: a plugin that writes -inf for none is read as the deepest the kernel can mean */
        const float deepest = (raw < 0.0) ? (raw > METER_GR_FLOOR_DB ? (float)raw : METER_GR_FLOOR_DB) : 0.0f;
        const double dt = tap->started ? now_s - tap->last_s : 0.0;
        const float sr = rate > 0.0 ? (float)rate : 48000.0f;

        tap->started = 1;
        tap->last_s = now_s;
        return omx_meter_view_gr(&tap->gr, deepest, dt, sr, METER_GR_RELEASE_POLE);
    }
}

#endif /* METER_READER_H */
