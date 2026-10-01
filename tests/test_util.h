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

/* What the two programs that test the core share: the check macro, the tone and the block checks, the crash check. */

#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#define SAMPLE_RATE     48000.0
#define BLOCK           256

#define FAKE_WIDE           "org.omx-clap-host.test.wide"
#define FAKE_SIDECHAIN      "org.omx-clap-host.test.sidechain"
#define FAKE_NOTES          "org.omx-clap-host.test.notes"
#define FAKE_PASSTHROUGH    "org.omx-clap-host.test.passthrough"
#define FAKE_WIDEN          "org.omx-clap-host.test.widen"
#define FAKE_AUXOUT         "org.omx-clap-host.test.auxout"
#define SYNTH               "org.omx-clap-host.test.synth"
#define SYNTH_MIDI          "org.omx-clap-host.test.synth-midi"
#define SYNTH_AUX           "org.omx-clap-host.test.synth-aux"
#define SYNTH_WIDE          "org.omx-clap-host.test.synth-wide"
#define SYNTH_NOTES         "org.omx-clap-host.test.synth-notes"
#define SYNTH_MPE           "org.omx-clap-host.test.synth-mpe"
#define SILENT              "org.omx-clap-host.test.silent"

// the read-only parameters every fake effect answers, and how many rows it has (parameter 0 is the only writable one)
#define FAKE_PARAM_ACTIVATIONS      1
#define FAKE_PARAM_PROCESS_CALLS    2
#define FAKE_PARAM_HOST_PRESET      3

static int g_failures;

#define CHECK(cond, ...) \
    do { \
        if (cond) { printf("ok   " __VA_ARGS__); printf("\n"); } \
        else { printf("FAIL " __VA_ARGS__); printf("\n"); g_failures++; } \
    } while (0)

static inline void fill_tone(float *buffer, uint32_t nframes, uint32_t offset, float amplitude)
{
    uint32_t i;

    for (i = 0; i < nframes; i++)
        buffer[i] = amplitude * sinf(2.0f * (float)M_PI * 1000.0f * (float)(offset + i) / (float)SAMPLE_RATE);
}

static inline int all_finite(const float *buffer, uint32_t nframes)
{
    uint32_t i;

    for (i = 0; i < nframes; i++)
        if (!isfinite(buffer[i]))
            return 0;
    return 1;
}

static inline int is_silent(const float *buffer, uint32_t from, uint32_t to)
{
    uint32_t i;

    for (i = from; i < to; i++)
        if (buffer[i] != 0.0f)
            return 0;
    return 1;
}

static inline float max_abs(const float *buffer, uint32_t nframes)
{
    float m = 0.0f;
    uint32_t i;

    for (i = 0; i < nframes; i++)
        if (fabsf(buffer[i]) > m)
            m = fabsf(buffer[i]);
    return m;
}

/* a write to `at` in a child of its own dies on a guard page; the child has no core dump to leave */
static inline int dies_writing(volatile float *at)
{
    struct rlimit no_core = { 0, 0 };
    int status;
    pid_t pid;

    fflush(stdout);
    pid = fork();
    if (pid == 0)
    {
        setrlimit(RLIMIT_CORE, &no_core);
        *at = 1.0f;
        _exit(0);
    }
    waitpid(pid, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
}

static inline int report(const char *name)
{
    printf("%s\n", g_failures == 0 ? name : "FAILED");
    return g_failures == 0 ? 0 : 1;
}

#endif
