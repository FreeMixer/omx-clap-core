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
 * host_transport_test.c: omx-clap-host's transport (src/host_transport.h) against closed forms, at every rate the
 * contract declares (OMX_ORACLE_FULL_RATES_INIT).
 *
 *  1. NONE until a tempo: no command, or a set_bpb alone, hands the plugin no transport (flags 0).
 *  2. PLAYING at 120 bpm from frame 1000: the block that starts half a second later reads beat 1.0 and 0.5 s exactly,
 *     4/4, bar 0, IS_PLAYING; every block of 64 frames over 10^6 blocks that starts on a half second reads its whole
 *     beat exactly, so no block adds drift.
 *  3. STOP holds the position, the next play goes on from it; a tempo change keeps the beats played and moves the
 *     next ones at the new tempo; a frame before the anchor reads the anchor; the frame counter's wrap is counted.
 *  4. BARS: 9 beats in 4/4 is bar 2, starting on beat 8; set_bpb 3 makes it 3/4.
 *  5. FOLLOWING JACK: bars and beats from a timebase master are handed on (bar 3, beat 2, half a tick-beat in 3/4 at
 *     90 bpm is beat 7.5); with none, the beats are the seconds at the host's tempo; with no tempo either, only the
 *     seconds and the play state.
 *  6. PUBLISH: an intent published reads back whole, field for field.
 */
#include <stdio.h>

#include "../src/host_transport.h"
#include "test_util.h"

static double beats_of(const struct omx_clap_transport *r)
{
    return (double)r->song_pos_beats / (double)CLAP_BEATTIME_FACTOR;
}

static double seconds_of(const struct omx_clap_transport *r)
{
    return (double)r->song_pos_seconds / (double)CLAP_SECTIME_FACTOR;
}

static const uint32_t PLAYED = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE |
                               CLAP_TRANSPORT_HAS_TIME_SIGNATURE | CLAP_TRANSPORT_IS_PLAYING;

static void at_rate(int rate_hz)
{
    const double rate = (double)rate_hz;
    const uint32_t half = (uint32_t)rate_hz / 2u;
    struct host_transport_intent none = { 0, 0, 0.0, HOST_TRANSPORT_BPB_DEFAULT, 0, 0.0, 0.0 };
    struct host_transport_intent t;
    struct omx_clap_transport r;
    uint32_t b, exact = 0, checked = 0;

    // 1. none until a tempo
    host_transport_own(&none, 5000, rate, &r);
    CHECK(r.flags == 0, "@%d: no command, no transport (flags %#x)", rate_hz, r.flags);
    t = host_transport_change(&none, 100, rate, 0, 0.0, 3.0);
    host_transport_own(&t, 5000, rate, &r);
    CHECK(r.flags == 0 && t.bpb == 3.0, "@%d: set_bpb alone gives no transport yet, and keeps its 3 beats a bar", rate_hz);

    // 2. playing at 120 bpm from frame 1000
    t = host_transport_change(&none, 1000, rate, 1, 120.0, 4.0);
    host_transport_own(&t, 1000 + half, rate, &r);
    CHECK(r.flags == PLAYED && r.tempo == 120.0 && r.song_pos_beats == CLAP_BEATTIME_FACTOR && r.song_pos_seconds == CLAP_SECTIME_FACTOR / 2 &&
              r.tsig_num == 4 && r.tsig_denom == 4 && r.bar_number == 0 && r.bar_start == 0,
          "@%d: half a second in, beat 1.0 and 0.5 s exactly, 4/4, bar 0, playing (flags %#x, beat %.17g, %.17g s)", rate_hz, r.flags,
          beats_of(&r), seconds_of(&r));
    for (b = 0; b < 1000000u; b++)
    {
        const uint32_t start = 1000u + b * 64u;

        if ((b * 64u) % half)
            continue;
        host_transport_own(&t, start, rate, &r);
        checked++;
        exact += r.song_pos_beats == (clap_beattime)((b * 64u) / half) * CLAP_BEATTIME_FACTOR;
    }
    CHECK(checked > 0 && exact == checked, "@%d: over 10^6 blocks of 64 frames, each of the %u that start on a half second reads its whole beat exactly (%u)",
          rate_hz, checked, exact);

    // 3. stop holds, play goes on, a tempo change keeps the beats played
    {
        struct host_transport_intent stopped = host_transport_change(&t, 1000 + 2 * half, rate, 0, 0.0, 0.0);
        struct host_transport_intent again, slower;

        host_transport_own(&stopped, 1000 + 10 * half, rate, &r);
        CHECK(r.song_pos_beats == 2 * CLAP_BEATTIME_FACTOR && !(r.flags & CLAP_TRANSPORT_IS_PLAYING),
              "@%d: stopped at beat 2, it stays at beat 2 and is not playing (%.17g)", rate_hz, beats_of(&r));
        again = host_transport_change(&stopped, 1000 + 10 * half, rate, 1, 0.0, 0.0);
        host_transport_own(&again, 1000 + 11 * half, rate, &r);
        CHECK(r.song_pos_beats == 3 * CLAP_BEATTIME_FACTOR && fabs(seconds_of(&r) - 1.5) < 1e-9 && (r.flags & CLAP_TRANSPORT_IS_PLAYING),
              "@%d: played again, it goes on from beat 2: beat 3 half a second later (%.17g, %.17g s)", rate_hz, beats_of(&r), seconds_of(&r));
        slower = host_transport_change(&again, 1000 + 12 * half, rate, 1, 60.0, 0.0);
        host_transport_own(&slower, 1000 + 14 * half, rate, &r);
        CHECK(r.song_pos_beats == 5 * CLAP_BEATTIME_FACTOR && r.tempo == 60.0,
              "@%d: at beat 4 the tempo halves: a second later it is beat 5, at 60 bpm (%.17g)", rate_hz, beats_of(&r));
        host_transport_own(&slower, 1000 + 11 * half, rate, &r);
        CHECK(r.song_pos_beats == 4 * CLAP_BEATTIME_FACTOR, "@%d: a block that began before the change reads the change's position (%.17g)",
              rate_hz, beats_of(&r));
    }
    t = host_transport_change(&none, 0xffffff00u, rate, 1, 120.0, 4.0);
    host_transport_own(&t, 0xffffff00u + half, rate, &r);
    CHECK(r.song_pos_beats == CLAP_BEATTIME_FACTOR, "@%d: across the wrap of the frame counter, half a second is still beat 1 (%.17g)", rate_hz,
          beats_of(&r));

    // 4. bars
    t = host_transport_change(&none, 0, rate, 1, 120.0, 4.0);
    host_transport_own(&t, 9 * half, rate, &r);
    CHECK(r.bar_number == 2 && r.bar_start == 8 * CLAP_BEATTIME_FACTOR, "@%d: beat 9 in 4/4 is bar 2, from beat 8 (%d, %.17g)", rate_hz,
          r.bar_number, (double)r.bar_start / (double)CLAP_BEATTIME_FACTOR);
    t = host_transport_change(&t, 9 * half, rate, 1, 0.0, 3.0);
    host_transport_own(&t, 9 * half, rate, &r);
    CHECK(r.bar_number == 3 && r.bar_start == 9 * CLAP_BEATTIME_FACTOR && r.tsig_num == 3 && r.song_pos_beats == 9 * CLAP_BEATTIME_FACTOR,
          "@%d: set_bpb 3 there: 3/4, beat 9 is bar 3 (%d)", rate_hz, r.bar_number);

    // 5. following JACK
    {
        const struct host_jack_position bbt = { 1, 3u * (uint32_t)rate_hz, 1, 3, 2, 960, 1920.0, 90.0, 3.0, 4.0 };
        struct host_jack_position plain = bbt;

        t = host_transport_change(&none, 0, rate, 0, 100.0, 4.0);
        host_transport_follow(&bbt, &none, rate, &r);
        CHECK(r.flags == PLAYED && r.song_pos_beats == (clap_beattime)(7.5 * (double)CLAP_BEATTIME_FACTOR) && r.tempo == 90.0 && r.tsig_num == 3 &&
                  r.tsig_denom == 4 && r.bar_number == 2 && r.song_pos_seconds == 3 * CLAP_SECTIME_FACTOR,
              "@%d: a timebase master's bar 3, beat 2 and half a beat in 3/4 at 90 bpm is beat 7.5, bar 2, 3 s, playing (%.17g)", rate_hz,
              beats_of(&r));
        plain.bbt = 0;
        host_transport_follow(&plain, &t, rate, &r);
        CHECK(r.flags == PLAYED && r.song_pos_beats == 5 * CLAP_BEATTIME_FACTOR && r.tempo == 100.0,
              "@%d: with no master, 3 s at the host's 100 bpm is beat 5 (%.17g)", rate_hz, beats_of(&r));
        plain.rolling = 0;
        host_transport_follow(&plain, &none, rate, &r);
        CHECK(r.flags == CLAP_TRANSPORT_HAS_SECONDS_TIMELINE && r.song_pos_seconds == 3 * CLAP_SECTIME_FACTOR,
              "@%d: with no tempo either, 3 s and stopped, nothing else (flags %#x)", rate_hz, r.flags);
    }
}

int main(void)
{
    static const int rates[] = OMX_ORACLE_FULL_RATES_INIT;
    struct host_transport p;
    struct host_transport_intent in = { 1, 1, 133.5, 7.0, 4242u, 12.25, 6.5 }, out = { 0 };
    size_t i;

    for (i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
        at_rate(rates[i]);

    // 6. publish and read back
    memset(&p, 0, sizeof p);
    host_transport_publish(&p, &in);
    CHECK(host_transport_read(&p, &out) == 1 && out.tempo_set == 1 && out.rolling == 1 && out.bpm == 133.5 && out.bpb == 7.0 &&
              out.anchor_frame == 4242u && out.anchor_beats == 12.25 && out.anchor_seconds == 6.5,
          "an intent published reads back whole");
    atomic_store(&p.seq, 1u);
    out.bpm = -1.0;
    CHECK(host_transport_read(&p, &out) == 0 && out.bpm == -1.0, "a publish under way is not read: the reader keeps what it had");
    return report("host transport test ok");
}
