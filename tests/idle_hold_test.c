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

/* The CPU idle hold of the core (#296): while an instance is active the library holds the device named by
 * OMX_CLAP_IDLE_HOLD_DEVICE (/dev/cpu_dma_latency by default) at the configured target, one descriptor for the whole
 * process, and closes it when the last instance deactivates. The device here is a regular file, so the bytes written
 * and the descriptors kept open are read back exactly; the refusal arm points the library at a path that does not exist.
 * One argument: tests/fake.clap. */

#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "../src/clap_host.h"
#include "test_util.h"

static char g_device[256];

/* what the device file holds, as the kernel would answer a read: the int32 written last, or -2 for none */
static int32_t device_value(void)
{
    int32_t v = -2;
    int fd = open(g_device, O_RDONLY);

    if (fd >= 0)
    {
        if (read(fd, &v, sizeof(v)) != (ssize_t)sizeof(v))
            v = -2;
        close(fd);
    }
    return v;
}

/* how many descriptors of this process name the device file */
static int device_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    struct dirent *e;
    char path[300], target[256];
    ssize_t n;
    int count = 0;

    while (d && (e = readdir(d)))
    {
        snprintf(path, sizeof(path), "/proc/self/fd/%s", e->d_name);
        n = readlink(path, target, sizeof(target) - 1);
        if (n > 0)
        {
            target[n] = '\0';
            if (strcmp(target, g_device) == 0)
                count++;
        }
    }
    if (d)
        closedir(d);
    return count;
}

static struct omx_clap_instance *open_active(const char *path, const char *id)
{
    struct omx_clap_instance *in = NULL;
    char why[OMX_CLAP_WHY_MAX];

    if (omx_clap_host_open(path, id, &in, why) != 0)
    {
        printf("FAIL open %s: %s\n", id, why);
        g_failures++;
        return NULL;
    }
    if (omx_clap_host_activate(in, SAMPLE_RATE, BLOCK, why) != 0)
    {
        printf("FAIL activate %s: %s\n", id, why);
        g_failures++;
        omx_clap_host_close(in);
        return NULL;
    }
    return in;
}

int main(int argc, char **argv)
{
    struct omx_clap_host_config config;
    struct omx_clap_idle_hold hold;
    struct omx_clap_instance *a, *b;
    int fd;

    if (argc != 2)
    {
        fprintf(stderr, "usage: idle_hold_test <fake.clap>\n");
        return 2;
    }
    snprintf(g_device, sizeof(g_device), "/tmp/omx-idle-hold-XXXXXX");
    fd = mkstemp(g_device);
    if (fd < 0)
        return 2;
    close(fd);
    setenv("OMX_CLAP_IDLE_HOLD_DEVICE", g_device, 1);

    omx_clap_host_config_default(&config);
    CHECK(config.idle_hold_us == (int32_t)CLAP_HOST_IDLE_HOLD_US && CLAP_HOST_IDLE_HOLD_US == 10u,
          "the default target is the declared CLAP_HOST_IDLE_HOLD_US, 10 us: C1 only");
    CHECK(omx_clap_host_configure(&config) == 0, "configured with the defaults");

    omx_clap_host_idle_hold(&hold);
    CHECK(hold.target_us == 10 && hold.held_us == OMX_CLAP_IDLE_HOLD_OFF && hold.active == 0 && hold.code[0] == '\0',
          "before any instance: target 10, nothing held, no instance, no code (%d %d %u '%s')", hold.target_us, hold.held_us, hold.active, hold.code);
    CHECK(device_fds() == 0, "and the device is not open");

    a = open_active(argv[1], FAKE_MONO_STEREO);
    if (!a)
        return report("idle_hold_test");
    omx_clap_host_idle_hold(&hold);
    CHECK(hold.active == 1 && hold.held_us == 10 && hold.code[0] == '\0', "one active instance: the target is held (%u %d '%s')", hold.active, hold.held_us, hold.code);
    CHECK(device_value() == 10, "the device read 10, the int32 the library wrote (%d)", device_value());
    CHECK(hold.aggregate_us == 10, "the status carries the device's own read-back (%d)", hold.aggregate_us);
    CHECK(device_fds() == 1, "one descriptor holds it (%d)", device_fds());

    b = open_active(argv[1], FAKE_MONO_STEREO);
    if (!b)
        return report("idle_hold_test");
    omx_clap_host_idle_hold(&hold);
    CHECK(hold.active == 2 && device_fds() == 1, "a second instance: counted, the one descriptor stays (%u %d)", hold.active, device_fds());
    omx_clap_host_deactivate(a);
    omx_clap_host_idle_hold(&hold);
    CHECK(hold.active == 1 && hold.held_us == 10 && device_fds() == 1, "the first deactivated: the hold stands for the second (%u %d %d)", hold.active, hold.held_us, device_fds());
    omx_clap_host_deactivate(b);
    omx_clap_host_idle_hold(&hold);
    CHECK(hold.active == 0 && hold.held_us == OMX_CLAP_IDLE_HOLD_OFF && device_fds() == 0,
          "the last deactivated: the descriptor is closed, so the kernel drops the request (%u %d %d)", hold.active, hold.held_us, device_fds());
    omx_clap_host_deactivate(b);
    omx_clap_host_idle_hold(&hold);
    CHECK(hold.active == 0, "a second deactivate of an inactive instance counts nothing (%u)", hold.active);

    // the target changes at run time: at once while held, for the next hold while not
    CHECK(omx_clap_host_idle_hold_set(0) == 0, "set 0 while nothing is active");
    omx_clap_host_idle_hold(&hold);
    CHECK(hold.target_us == 0 && hold.held_us == OMX_CLAP_IDLE_HOLD_OFF && device_fds() == 0, "accepted, nothing opened until an instance activates (%d %d %d)", hold.target_us, hold.held_us, device_fds());
    CHECK(omx_clap_host_activate(a, SAMPLE_RATE, BLOCK, NULL) == 0, "the first instance activates again");
    CHECK(device_value() == 0 && device_fds() == 1, "and the device read 0: no idle state at all (%d %d)", device_value(), device_fds());
    CHECK(omx_clap_host_idle_hold_set(25) == 0 && device_value() == 25, "set 25 while held: written to the open descriptor at once (%d)", device_value());
    omx_clap_host_idle_hold(&hold);
    CHECK(hold.held_us == 25 && hold.aggregate_us == 25, "and reported as held (%d %d)", hold.held_us, hold.aggregate_us);
    CHECK(omx_clap_host_idle_hold_set(OMX_CLAP_IDLE_HOLD_OFF) == 0 && device_fds() == 0, "set off while held: the descriptor is closed (%d)", device_fds());
    omx_clap_host_idle_hold(&hold);
    CHECK(hold.target_us == OMX_CLAP_IDLE_HOLD_OFF && hold.held_us == OMX_CLAP_IDLE_HOLD_OFF && hold.active == 1, "off: an active instance holds nothing (%d %d %u)", hold.target_us, hold.held_us, hold.active);
    CHECK(omx_clap_host_idle_hold_set(10) == 0 && device_value() == 10 && device_fds() == 1, "set 10 again while active: held again (%d %d)", device_value(), device_fds());
    CHECK(omx_clap_host_idle_hold_set(-7) == -1, "a target below off is refused");
    omx_clap_host_idle_hold(&hold);
    CHECK(hold.target_us == 10, "and the target stands (%d)", hold.target_us);
    omx_clap_host_deactivate(a);
    CHECK(device_fds() == 0, "deactivated: closed");

    // the refusal: a device that cannot be opened is reported by code, and the instance runs on
    setenv("OMX_CLAP_IDLE_HOLD_DEVICE", "/nonexistent/cpu_dma_latency", 1);
    CHECK(omx_clap_host_activate(a, SAMPLE_RATE, BLOCK, NULL) == 0, "with no device to open the instance still activates");
    omx_clap_host_idle_hold(&hold);
    CHECK(hold.active == 1 && hold.held_us == OMX_CLAP_IDLE_HOLD_OFF && strcmp(hold.code, CLAP_HOST_CODE_IDLE_HOLD_REFUSED) == 0,
          "the status names the refusal by code and holds nothing (%u %d '%s')", hold.active, hold.held_us, hold.code);
    CHECK(hold.aggregate_us == -1, "no read-back either (%d)", hold.aggregate_us);
    omx_clap_host_deactivate(a);
    setenv("OMX_CLAP_IDLE_HOLD_DEVICE", g_device, 1);
    CHECK(omx_clap_host_activate(a, SAMPLE_RATE, BLOCK, NULL) == 0, "the device back: the instance activates");
    omx_clap_host_idle_hold(&hold);
    CHECK(hold.held_us == 10 && hold.code[0] == '\0', "held again and the code cleared (%d '%s')", hold.held_us, hold.code);

    omx_clap_host_close(a);
    omx_clap_host_close(b);
    omx_clap_host_idle_hold(&hold);
    CHECK(hold.active == 0 && device_fds() == 0, "both closed: nothing active, nothing open (%u %d)", hold.active, device_fds());
    unlink(g_device);
    return report("idle_hold_test");
}
