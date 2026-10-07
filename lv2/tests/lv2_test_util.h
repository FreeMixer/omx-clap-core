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

/* What the tests of the LV2 adapter share: the check, the declared rates, the tone, a child process for an arm that
 * needs a configuration of its own (the LV2 host is configured once per process), and the binary a host loaded. */

#ifndef LV2_TEST_UTIL_H
#define LV2_TEST_UTIL_H

#include <dlfcn.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* the declared rates, the rig's last */
static const double RATES[] = { 44100.0, 48000.0, 96000.0, 192000.0 };
#define N_RATES (sizeof(RATES) / sizeof(RATES[0]))

static int g_failures;

#define CHECK(cond, ...) \
    do { \
        if (cond) { printf("ok   " __VA_ARGS__); printf("\n"); } \
        else { printf("FAIL " __VA_ARGS__); printf(" (%s:%d)\n", __FILE__, __LINE__); g_failures++; } \
    } while (0)

static inline void tone(float *b, uint32_t n, float amp, uint32_t phase)
{
    uint32_t i;

    for (i = 0; i < n; i++)
        b[i] = amp * sinf(0.0613f * (float)(i + phase));
}

static inline int bits_equal(const float *a, const float *b, uint32_t n)
{
    return memcmp(a, b, n * sizeof(float)) == 0;
}

static inline float peak_diff(const float *a, const float *b, uint32_t n)
{
    float m = 0.0f;
    uint32_t i;

    for (i = 0; i < n; i++)
        m = fmaxf(m, fabsf(a[i] - b[i]));
    return m;
}

/* run `arm` in a child of its own: its failures come back as its exit status */
static inline void in_child(const char *label, void (*arm)(void))
{
    int status = 0;
    pid_t pid;

    fflush(stdout);
    pid = fork();
    if (pid == 0)
    {
        g_failures = 0;
        arm();
        fflush(stdout);
        _exit(g_failures > 255 ? 255 : g_failures);
    }
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "%s: the child arm passed (status %d)", label, status);
}

/* `name` of the binary already loaded from `path`, without loading it */
static inline void *loaded_symbol(const char *path, const char *name)
{
    void *so = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    void *sym = so ? dlsym(so, name) : NULL;

    if (so)
        dlclose(so);
    return sym;
}

/* lilv in the process at all, read off the kernel's map */
static inline int lilv_mapped(void)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char line[PATH_MAX + 128];
    int found = 0;

    while (f && !found && fgets(line, sizeof(line), f))
        found = strstr(line, "liblilv-0") != NULL || strstr(line, "lilv-late") != NULL;
    if (f)
        fclose(f);
    return found;
}

static inline int abs_dir(const char *rel, char out[PATH_MAX])
{
    char buf[PATH_MAX];

    if (!realpath(rel, buf))
        return -1;
    return snprintf(out, PATH_MAX, "%s/", buf) < PATH_MAX ? 0 : -1;
}

#endif
