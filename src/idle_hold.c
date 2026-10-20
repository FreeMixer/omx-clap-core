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
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "clap_host.h"
#include "idle_hold.h"


/*
************************************************************************************************************************
*           LOCAL DEFINES
************************************************************************************************************************
*/

#define IDLE_HOLD_DEVICE        "/dev/cpu_dma_latency"
#define IDLE_HOLD_DEVICE_ENV    "OMX_CLAP_IDLE_HOLD_DEVICE"


/*
************************************************************************************************************************
*           LOCAL GLOBAL VARIABLES
************************************************************************************************************************
*/

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int32_t g_target = CLAP_HOST_IDLE_HOLD_US;
static int g_fd = -1;               // the one descriptor, -1 while nothing is held
static uint32_t g_active;           // the instances active in this process
static char g_code[OMX_CLAP_WHY_MAX];   // the last refusal, empty once a hold succeeded


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS
************************************************************************************************************************
*/

/* the device: /dev/cpu_dma_latency, or what OMX_CLAP_IDLE_HOLD_DEVICE names (a test's regular file) */
static const char *device_path(void)
{
    const char *path = getenv(IDLE_HOLD_DEVICE_ENV);

    return path && *path ? path : IDLE_HOLD_DEVICE;
}

/* the target to the descriptor: at offset 0, so a regular file reads back what the device would answer */
static int write_target(int fd)
{
    return pwrite(fd, &g_target, sizeof(g_target), 0) == (ssize_t)sizeof(g_target) ? 0 : -1;
}

static void idle_hold_close(void)
{
    if (g_fd >= 0)
        close(g_fd);
    g_fd = -1;
}

/* open the device and write the target; a refusal (no permission, no device) is recorded by code and holds nothing */
static int idle_hold_open(void)
{
    int fd = open(device_path(), O_RDWR | O_CLOEXEC);

    if (fd < 0 || write_target(fd) != 0)
    {
        if (fd >= 0)
            close(fd);
        snprintf(g_code, sizeof(g_code), "%s", CLAP_HOST_CODE_IDLE_HOLD_REFUSED);
        return -1;
    }
    g_fd = fd;
    g_code[0] = '\0';
    return 0;
}

/* what the kernel holds across every requester, read off the device: the held descriptor, or a read-only open; -1
 * when it cannot be read */
static int32_t aggregate_read(void)
{
    int32_t value = -1;
    int fd = g_fd >= 0 ? g_fd : open(device_path(), O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return -1;
    if (pread(fd, &value, sizeof(value), 0) != (ssize_t)sizeof(value))
        value = -1;
    if (fd != g_fd)
        close(fd);
    return value;
}


/*
************************************************************************************************************************
*           GLOBAL FUNCTIONS
************************************************************************************************************************
*/

int32_t idle_hold_target_default(void)
{
    return (int32_t)CLAP_HOST_IDLE_HOLD_US;
}

void idle_hold_configure(int32_t target_us)
{
    pthread_mutex_lock(&g_lock);
    g_target = target_us < OMX_CLAP_IDLE_HOLD_OFF ? OMX_CLAP_IDLE_HOLD_OFF : target_us;
    pthread_mutex_unlock(&g_lock);
}

void idle_hold_acquire(void)
{
    pthread_mutex_lock(&g_lock);
    g_active++;
    if (g_target != OMX_CLAP_IDLE_HOLD_OFF && g_fd < 0)
        idle_hold_open();
    pthread_mutex_unlock(&g_lock);
}

void idle_hold_release(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_active > 0)
        g_active--;
    if (g_active == 0)
        idle_hold_close();
    pthread_mutex_unlock(&g_lock);
}

int omx_clap_host_idle_hold_set(int32_t target_us)
{
    if (target_us < OMX_CLAP_IDLE_HOLD_OFF)
        return -1;
    pthread_mutex_lock(&g_lock);
    g_target = target_us;
    if (target_us == OMX_CLAP_IDLE_HOLD_OFF)
        idle_hold_close();
    else if (g_fd >= 0)
    {
        // held already: the open request is updated in place
        if (write_target(g_fd) != 0)
        {
            idle_hold_close();
            snprintf(g_code, sizeof(g_code), "%s", CLAP_HOST_CODE_IDLE_HOLD_REFUSED);
        }
    }
    else if (g_active > 0)
        idle_hold_open();
    pthread_mutex_unlock(&g_lock);
    return 0;
}

void omx_clap_host_idle_hold(struct omx_clap_idle_hold *out)
{
    if (!out)
        return;
    pthread_mutex_lock(&g_lock);
    out->target_us = g_target;
    out->held_us = g_fd >= 0 ? g_target : OMX_CLAP_IDLE_HOLD_OFF;
    out->aggregate_us = aggregate_read();
    out->active = g_active;
    snprintf(out->code, sizeof(out->code), "%s", g_code);
    pthread_mutex_unlock(&g_lock);
}
