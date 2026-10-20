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
 * What a full host offers besides the basics, through libomx-clap-core against tests/fake_ext.clap: timers and file
 * descriptors served by the tick, the thread pool inside process() and refused outside it, render, the tail and the
 * tail changed, the sleep of a plugin that says it needs no processing and its waking, state-context, and
 * audio-ports-activation of the auxiliary output.
 *   clap_ext_test <fake_ext.clap>
 */

#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "../src/clap_host.h"
#include <omxcontract/omx_contract_limits.h>
#include "test_util.h"

#define EXT_ID  "org.omx-clap-host.test.ext"
#define BLK     256u

typedef struct
{
    uint32_t timer_calls;
    clap_id timer_id;
    uint32_t timer_registered, fd_calls, fd_flags, pool_refused_outside, pool_request, pool_ok, pool_done;
    uint32_t pool_nested, pool_nested_ok, pool_nested_refused;
    int32_t render_mode;
    uint32_t hard_rt, tail;
    int32_t status;
    uint32_t process_calls;
    int64_t last_steady;
    uint32_t loaded_context, plain_loads, aux_out_set, aux_out_active, ring;
} ext_stats_t;

static void *hook(const char *path, const char *name)
{
    void *file = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    void *h = file ? dlsym(file, name) : NULL;

    if (file)
        dlclose(file);
    return h;
}

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000u, (long)(ms % 1000u) * 1000000L };

    nanosleep(&ts, NULL);
}

static float g_l[BLK], g_r[BLK];

static void run(struct omx_clap_instance *in, float level)
{
    for (uint32_t i = 0; i < BLK; i++)
        g_l[i] = g_r[i] = level;
    omx_clap_host_run(in, g_l, g_r, BLK);
}

static void transport(struct omx_clap_transport_src *src, uint32_t flags, double bpm, double seconds)
{
    struct omx_clap_transport t;

    memset(&t, 0, sizeof t);
    t.flags = flags | CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE | CLAP_TRANSPORT_HAS_BEATS_TIMELINE;
    t.tempo = bpm;
    t.song_pos_seconds = (clap_sectime)(seconds * (double)CLAP_SECTIME_FACTOR);
    t.song_pos_beats = (clap_beattime)(seconds * bpm / 60.0 * (double)CLAP_BEATTIME_FACTOR);
    omx_clap_transport_publish(src, &t);
}

/* the sleep of an idle plugin and its waking, at one rate */
static void sleep_section(struct omx_clap_instance *in, ext_stats_t *st, int (*set_tail)(uint32_t), void (*wake)(void), double rate,
                          struct omx_clap_transport_src *tsrc)
{
    int64_t asleep_at;
    uint64_t skipped;
    uint32_t calls, i;

    in->sleep.quiet_min_frames = 0;
    in->sleep.asleep = 0;
    st->status = CLAP_PROCESS_CONTINUE;
    st->ring = 0;
    CHECK(set_tail(1000) == 0, "%.0f Hz: a tail of 1000 frames", rate);
    omx_clap_host_tick(in);
    transport(tsrc, 0, 120.0, 0.0);
    run(in, 0.0f);

    /* CONTINUE: every block is processed, silent or not */
    calls = st->process_calls;
    for (i = 0; i < 4; i++)
        run(in, 0.0f);
    CHECK(st->process_calls == calls + 4, "CONTINUE: four silent blocks are four process calls");

    /* SLEEP: after it, silent blocks are not processed; the steady time moves on; sound wakes it */
    st->status = CLAP_PROCESS_SLEEP;
    run(in, 0.0f);
    asleep_at = st->last_steady;
    calls = st->process_calls;
    skipped = atomic_load(&in->sleep.skipped);
    for (i = 0; i < 4; i++)
        run(in, 0.0f);
    CHECK(st->process_calls == calls && atomic_load(&in->sleep.skipped) == skipped + 4 && in->sleep.asleep,
          "SLEEP: four silent blocks after it are not processed (%u calls, %llu skipped)", st->process_calls - calls,
          (unsigned long long)(atomic_load(&in->sleep.skipped) - skipped));
    run(in, 0.5f);
    CHECK(st->process_calls == calls + 1 && st->last_steady == asleep_at + 5 * (int64_t)BLK,
          "a block with sound wakes it, and its steady time counts the blocks it slept through (%lld)", (long long)st->last_steady);
    for (i = 0; i < BLK; i++)
        if (g_l[i] != 0.5f)
            break;
    CHECK(i == BLK, "the woken block is the plugin's output");

    /* request_process wakes it for one block */
    run(in, 0.0f);
    calls = st->process_calls;
    run(in, 0.0f);
    CHECK(st->process_calls == calls, "asleep again on silence");
    if (wake)
        wake();
    run(in, 0.0f);
    CHECK(st->process_calls == calls + 1, "request_process wakes it for the next block");

    /* a parameter write would wake it too; a note cannot reach an effect. CONTINUE_IF_NOT_QUIET: sleeps on a silent block */
    st->status = CLAP_PROCESS_CONTINUE_IF_NOT_QUIET;
    run(in, 0.5f);
    calls = st->process_calls;
    run(in, 0.5f);
    CHECK(st->process_calls == calls + 1, "CONTINUE_IF_NOT_QUIET: processed while there is sound");
    run(in, 0.0f);
    run(in, 0.0f);
    CHECK(st->process_calls == calls + 2, "and asleep after the first silent block (%u)", st->process_calls - calls);
    // an output that still sounds on a silent input (an instrument's voice) keeps it awake: the status is about the output
    run(in, 0.5f);
    st->ring = 3;
    calls = st->process_calls;
    for (i = 0; i < 6; i++)
        run(in, 0.0f);
    CHECK(st->process_calls == calls + 4, "CONTINUE_IF_NOT_QUIET: a silent input whose output still sounds is processed until "
          "the output is silent too (%u)", st->process_calls - calls);

    /* TAIL: processed for the tail after the sound stops, then asleep */
    st->status = CLAP_PROCESS_TAIL;
    CHECK(set_tail(3 * BLK) == 0, "a tail of three blocks");
    omx_clap_host_tick(in);
    run(in, 0.5f);
    calls = st->process_calls;
    for (i = 0; i < 6; i++)
        run(in, 0.0f);
    CHECK(st->process_calls == calls + 3, "TAIL: three silent blocks processed, the tail, then none (%u)", st->process_calls - calls);
    run(in, 0.5f);
    st->ring = 5;
    calls = st->process_calls;
    for (i = 0; i < 8; i++)
        run(in, 0.0f);
    CHECK(st->process_calls == calls + 6, "TAIL: a tail that has passed while the output still sounds goes on until it is silent (%u)",
          st->process_calls - calls);
    CHECK(set_tail(UINT32_MAX) == 0, "an infinite tail");
    omx_clap_host_tick(in);
    run(in, 0.5f);
    in->sleep.quiet_frames = UINT32_MAX;    // as if silent for a day: an infinite tail still never ends
    calls = st->process_calls;
    for (i = 0; i < 6; i++)
        run(in, 0.0f);
    CHECK(st->process_calls == calls + 6, "an infinite tail never sleeps, however long the silence (%u)", st->process_calls - calls);

    /* a change of the transport wakes a sleeping plugin: a play start, a tempo, a locate; a steady playhead does not */
    in->sleep.quiet_min_frames = 0;
    st->status = CLAP_PROCESS_SLEEP;
    transport(tsrc, 0, 120.0, 0.0);
    run(in, 0.0f);
    run(in, 0.0f);
    calls = st->process_calls;
    run(in, 0.0f);
    transport(tsrc, 0, 120.0, 0.0);
    run(in, 0.0f);
    CHECK(st->process_calls == calls && in->sleep.asleep, "stopped and unchanged: it stays asleep (%u)", st->process_calls - calls);
    transport(tsrc, CLAP_TRANSPORT_IS_PLAYING, 120.0, 0.0);
    run(in, 0.0f);
    CHECK(st->process_calls == calls + 1, "a plugin asleep while stopped gets the play start (%u)", st->process_calls - calls);
    run(in, 0.0f);
    run(in, 0.0f);
    calls = st->process_calls;
    for (i = 1; i <= 4; i++)
    {
        transport(tsrc, CLAP_TRANSPORT_IS_PLAYING, 120.0, (double)i * (double)BLK / rate);
        run(in, 0.0f);
    }
    CHECK(st->process_calls == calls && in->sleep.asleep, "playing with a steady playhead: it stays asleep (%u)", st->process_calls - calls);
    transport(tsrc, CLAP_TRANSPORT_IS_PLAYING, 90.0, 5.0 * (double)BLK / rate);
    run(in, 0.0f);
    CHECK(st->process_calls == calls + 1, "a tempo change wakes it (%u)", st->process_calls - calls);
    run(in, 0.0f);
    run(in, 0.0f);
    calls = st->process_calls;
    transport(tsrc, CLAP_TRANSPORT_IS_PLAYING, 90.0, 40.0);
    run(in, 0.0f);
    CHECK(st->process_calls == calls + 1, "a locate wakes it (%u)", st->process_calls - calls);
    run(in, 0.0f);
    run(in, 0.0f);
    calls = st->process_calls;
    transport(tsrc, CLAP_TRANSPORT_IS_PLAYING | CLAP_TRANSPORT_IS_LOOP_ACTIVE, 90.0, 40.0 + (double)BLK / rate);
    run(in, 0.0f);
    CHECK(st->process_calls == calls + 1, "the loop switched on wakes it (%u)", st->process_calls - calls);

    /* a sysex queued for the block wakes it, as a note does */
    run(in, 0.0f);
    run(in, 0.0f);
    calls = st->process_calls;
    in->stage.n_sysex = 1;
    run(in, 0.0f);
    in->stage.n_sysex = 0;
    CHECK(st->process_calls == calls + 1, "a queued sysex wakes it (%u)", st->process_calls - calls);

    /* the window: a plugin that says SLEEP sleeps only after its output has been silent that long */
    in->sleep.quiet_min_frames = 3 * BLK;
    st->status = CLAP_PROCESS_SLEEP;
    run(in, 0.5f);
    calls = st->process_calls;
    for (i = 0; i < 6; i++)
        run(in, 0.0f);
    CHECK(st->process_calls == calls + 3, "SLEEP waits for three blocks of silent output, the window, then sleeps (%u)",
          st->process_calls - calls);

}

int main(int argc, char **argv)
{
    struct omx_clap_instance *in = NULL;
    char why[OMX_CLAP_WHY_MAX], buf[64];
    ext_stats_t *st;
    int (*set_tail)(uint32_t);
    void (*wake)(void);
    int pipefd[2], idle[2];
    int (*ask_pool)(void);
    size_t len = 0;

    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <fake_ext.clap>\n", argv[0]);
        return 2;
    }
    CHECK(pipe(pipefd) == 0 && pipe(idle) == 0, "two pipes for the plugin's descriptors");
    snprintf(buf, sizeof(buf), "%d", pipefd[0]);
    setenv("FAKE_EXT_FD", buf, 1);
    snprintf(buf, sizeof(buf), "%d", idle[0]);
    setenv("FAKE_EXT_FD2", buf, 1);
    CHECK(omx_clap_host_open(argv[1], EXT_ID, &in, why) == 0, "fake_ext opens (%s)", why);
    if (!in)
        return report("clap ext test");
    st = (ext_stats_t *)((void *(*)(void))hook(argv[1], "fake_ext_stats"))();
    set_tail = (int (*)(uint32_t))hook(argv[1], "fake_ext_set_tail");
    wake = (void (*)(void))hook(argv[1], "fake_ext_wake");
    ask_pool = (int (*)(void))hook(argv[1], "fake_ext_ask_pool");

    /* timers: registered at init, called by the tick once due, not before */
    CHECK(st->timer_registered, "the plugin registers a timer at init");
    omx_clap_host_tick(in);
    CHECK(st->timer_calls == 0, "the tick calls no timer before its period");
    sleep_ms(120);
    omx_clap_host_tick(in);
    CHECK(st->timer_calls == 1, "after 120 ms one tick calls the 50 ms timer once (%u)", st->timer_calls);
    sleep_ms(60);
    omx_clap_host_tick(in);
    CHECK(st->timer_calls == 2, "and the next tick a period later calls it again (%u)", st->timer_calls);

    /* descriptors: nothing to read, no call; a byte written, one call for reading */
    omx_clap_host_tick(in);
    CHECK(st->fd_calls == 0, "an idle descriptor is not called");
    CHECK(write(pipefd[1], "x", 1) == 1, "a byte written to the plugin's pipe");
    omx_clap_host_tick(in);
    CHECK(st->fd_calls == 1 && (st->fd_flags & CLAP_POSIX_FD_READ), "the tick calls on_fd once, readable, and not for the idle one (%u, %#x)", st->fd_calls,
          st->fd_flags);
    omx_clap_host_tick(in);
    CHECK(st->fd_calls == 1, "the byte read, the next tick calls nothing (%u)", st->fd_calls);

    /* the thread pool refused outside process(), from init */
    CHECK(st->pool_refused_outside, "request_exec from init is refused");
    CHECK(ask_pool && ask_pool() == 0, "request_exec from the control thread, off the audio role, is refused");

    /* render: realtime and offline told; offline refused to a plugin with a hard real-time requirement */
    CHECK(omx_clap_host_set_render(in, CLAP_RENDER_OFFLINE) == 0 && st->render_mode == CLAP_RENDER_OFFLINE,
          "render.set(OFFLINE) reaches the plugin before an offline run (%d)", st->render_mode);
    st->hard_rt = 1;
    CHECK(omx_clap_host_set_render(in, CLAP_RENDER_OFFLINE) == -1 && st->render_mode == CLAP_RENDER_OFFLINE,
          "a plugin with a hard real-time requirement is not asked to render offline: -1");
    CHECK(omx_clap_host_set_render(in, CLAP_RENDER_REALTIME) == 0 && st->render_mode == CLAP_RENDER_REALTIME,
          "realtime is still told");
    CHECK(omx_clap_host_set_render(in, 7) == -1, "an unknown mode: -1");

    /* state-context: each context saves what the plugin says for it; load hands the context on */
    CHECK(omx_clap_host_state_save_context(in, CLAP_STATE_CONTEXT_FOR_PRESET, buf, sizeof(buf), &len) == 0 && len == 6 &&
              !memcmp(buf, "preset", 6),
          "a save for a preset is the plugin's preset state");
    CHECK(omx_clap_host_state_save_context(in, CLAP_STATE_CONTEXT_FOR_PROJECT, buf, sizeof(buf), &len) == 0 && len == 7 &&
              !memcmp(buf, "project", 7),
          "a save for a project differs from it");
    CHECK(omx_clap_host_state_save(in, buf, sizeof(buf), &len) == 0 && len == 5 && !memcmp(buf, "plain", 5), "the plain save is the state's");
    CHECK(omx_clap_host_state_load_context(in, CLAP_STATE_CONTEXT_FOR_DUPLICATE, "x", 1) == 0 &&
              st->loaded_context == CLAP_STATE_CONTEXT_FOR_DUPLICATE && st->plain_loads == 0,
          "a load for a duplicate goes through state-context with its context");
    CHECK(omx_clap_host_state_save_context(in, 9, buf, sizeof(buf), &len) == -1, "an unknown context: -1");

    /* the tail, read at activate; the plugin's auxiliary output deactivated before it */
    st->tail = 1000;
    CHECK(omx_clap_host_activate(in, 48000.0, BLK, why) == 0, "fake_ext activates (%s)", why);
    CHECK(omx_clap_host_tail(in) == 1000, "the tail is read at activate (%u)", omx_clap_host_tail(in));
    CHECK(st->aux_out_set >= 1 && st->aux_out_active == 0, "the auxiliary output, never connected, is set inactive (%u, %u)",
          st->aux_out_set, st->aux_out_active);
    CHECK(set_tail && set_tail(2000) == 0, "the plugin says its tail changed");
    omx_clap_host_tick(in);
    CHECK(omx_clap_host_tail(in) == 2000, "the tick reads it again (%u)", omx_clap_host_tail(in));
    CHECK(in->sleep.quiet_min_frames == (uint32_t)(48000 * OMX_CLAP_SLEEP_QUIET_MS / 1000), "a sleep needs 200 ms of silent output (%u)",
          in->sleep.quiet_min_frames);
    in->sleep.quiet_min_frames = 0;     // the statuses below are each read on their own; the window is checked last
    omx_clap_host_publish(in, pthread_self());
    run(in, 0.0f);      // the first cycle after arming fades in

    /* the thread pool inside process(): every task done before request_exec returns */
    st->pool_request = 12;
    run(in, 0.5f);
    CHECK(st->pool_ok && st->pool_done == 12, "inside process() request_exec runs all 12 tasks before it returns (%u, %u)", st->pool_ok,
          st->pool_done);
    st->pool_request = 0;
    /* a request made from inside exec is refused, and the outer request still runs every task */
    st->pool_nested = 1;
    st->pool_request = 5;
    run(in, 0.5f);
    CHECK(st->pool_ok && st->pool_done == 5, "nested: the outer request ran all 5 tasks (%u, %u)", st->pool_ok, st->pool_done);
    CHECK(st->pool_nested_ok == 0 && st->pool_nested_refused == 5, "nested: request_exec from inside exec is refused every time (%u ran, %u refused)",
          st->pool_nested_ok, st->pool_nested_refused);
    st->pool_nested = 0;
    st->pool_request = 0;

    /* the sleep at the floor rates: the 200 ms window and the tail are rate-scaled */
    {
        static const double rates[] = OMX_ORACLE_FLOOR_RATES_INIT;
        struct omx_clap_transport_src tsrc;

        memset(&tsrc, 0, sizeof tsrc);
        in->transport_src = &tsrc;
        for (size_t r = 0; r < sizeof rates / sizeof rates[0]; r++)
        {
            if (rates[r] != 48000.0)
            {
                omx_clap_request_stop(&in->stage);
                run(in, 0.0f);      // the cycle that stops it
                CHECK(omx_clap_host_unpublish(in, 10, 1000000) == 0, "%.0f Hz: unpublished", rates[r]);
                CHECK(omx_clap_host_set_rate(in, rates[r], BLK, why) == 0, "%.0f Hz: re-activated (%s)", rates[r], why);
                omx_clap_host_publish(in, pthread_self());
                CHECK(in->sleep.quiet_min_frames == (uint32_t)(rates[r] * OMX_CLAP_SLEEP_QUIET_MS / 1000.0),
                      "%.0f Hz: a sleep needs 200 ms of silent output (%u)", rates[r], in->sleep.quiet_min_frames);
                run(in, 0.0f);
            }
            sleep_section(in, st, set_tail, wake, rates[r], &tsrc);
        }
    }

    omx_clap_host_unpublish(in, 10, 1000000);
    omx_clap_host_close(in);
    close(pipefd[0]);
    close(pipefd[1]);
    close(idle[0]);
    close(idle[1]);
    return report("clap ext test");
}
