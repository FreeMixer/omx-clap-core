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

/* The CPU idle hold, the library's and internal (#296): one descriptor on /dev/cpu_dma_latency for the whole process,
 * opened at the configured target when the first instance activates and closed when the last deactivates. The kernel
 * drops the request with the descriptor, so a crash restores the machine's default with no cleanup. The public calls
 * are clap_host.h's omx_clap_host_idle_hold_set and omx_clap_host_idle_hold; these are the core's own. */

#ifndef IDLE_HOLD_H
#define IDLE_HOLD_H

#include <stdint.h>

/* the target a fresh configuration carries: CLAP_HOST_IDLE_HOLD_US, as data/hosting.json declares it */
int32_t idle_hold_target_default(void);

/* the target the configuration settled on, before any instance */
void idle_hold_configure(int32_t target_us);

/* one more active instance: the device is opened and written on the first, when the target is not off */
void idle_hold_acquire(void);

/* one active instance fewer: the descriptor is closed on the last */
void idle_hold_release(void);

#endif /* IDLE_HOLD_H */
