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
* The xrun count a worker reports (#371).
*
* JACK tells a client about an xrun, not a plugin, and plugin-hostd holds no JACK client: the worker, the JACK client,
* writes `xruns <count>` on its feedback port, the count since its start, when it changes and at most once a second.
* The daemon attributes a window's xruns to the members of the worker (plugin-hostd's grouping planner).
*
* JACK's xrun callback runs on a thread of its own and only counts. The control thread's idle tick asks whether a line
* is due and writes it: the callback never touches a socket. The first line is written for the first xrun, not at
* start: a worker that has had none writes none, as an older worker writes none, and the daemon reads both as no xruns.
*
************************************************************************************************************************
*/

#ifndef XRUN_REPORT_H
#define XRUN_REPORT_H

#include <stdatomic.h>
#include <stdint.h>

/** The shortest time between two lines, in milliseconds. */
#define XRUN_REPORT_MIN_INTERVAL_MS 1000u

typedef struct
{
    atomic_uint_fast64_t count; /* the xruns JACK has reported, written by JACK's thread */
    uint64_t written;           /* the count of the last line, the control thread's */
    uint64_t written_at_ms;     /* when it was written */
    int has_written;            /* a line has been written */
} xrun_report_t;

static inline void xrun_report_init(xrun_report_t *report)
{
    atomic_init(&report->count, 0);
    report->written = 0;
    report->written_at_ms = 0;
    report->has_written = 0;
}

/** One xrun: JACK's callback. Counts and does nothing else. */
static inline void xrun_report_note(xrun_report_t *report)
{
    atomic_fetch_add_explicit(&report->count, 1, memory_order_relaxed);
}

/** The control thread's tick at now_ms (a monotonic clock): 1 and the count to write when a line is due, else 0.
 * Due is a count that is not the last one written, and at least XRUN_REPORT_MIN_INTERVAL_MS since that line. A count
 * that waits for the interval is not lost: the next tick that finds it due writes it. */
static inline int xrun_report_due(xrun_report_t *report, uint64_t now_ms, uint64_t *count)
{
    uint64_t seen = (uint64_t)atomic_load_explicit(&report->count, memory_order_relaxed);

    if (seen == report->written)
        return 0;
    if (report->has_written && now_ms - report->written_at_ms < XRUN_REPORT_MIN_INTERVAL_MS)
        return 0;
    report->written = seen;
    report->written_at_ms = now_ms;
    report->has_written = 1;
    *count = seen;
    return 1;
}

#endif /* XRUN_REPORT_H */
