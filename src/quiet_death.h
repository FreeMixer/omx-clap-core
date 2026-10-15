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
* A host that crashes dies at once, without a core dump.
*
* When a plugin aborts or faults, the kernel writes the process's core before the process is gone. With a core_pattern
* that pipes to systemd-coredump that takes 100 to 170 ms for a host with a few plugins, and every plugin of the host
* is silent for all of it, while its supervisor cannot respawn it yet. A SIGKILLed host is gone in 3 to 5 ms.
*
* So on a crash signal the host marks itself not dumpable (PR_SET_DUMPABLE 0, which skips the core whatever the core
* limit and the core_pattern say) and the signal then does what it would have done: the process dies of the same
* signal, so the supervisor reads the same exit status. Nothing changes while the host runs: the flag is only set on
* the way out. OMX_CLAP_HOST_CORE_DUMPS=1 in the environment keeps the core dumps, for a plugin crash to debug.
*
************************************************************************************************************************
*/

#ifndef QUIET_DEATH_H
#define QUIET_DEATH_H

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>

/** The environment variable that keeps core dumps when it is set to anything but empty or "0". */
#define QUIET_DEATH_KEEP_CORES_ENV "OMX_CLAP_HOST_CORE_DUMPS"

/* the signals whose default action dumps core, but SIGQUIT: that one is asked for, and the core is the point */
static const int quiet_death_signals[] = { SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGSYS };

/* SA_RESETHAND has put the default action back; the signal raised again stays pending while this handler runs and
 * kills the process when it returns, so a signal raised on purpose ends the process as it would have without us */
static void quiet_death_handler(int sig)
{
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    raise(sig);
}

/** Install the handler on every crash signal, unless the environment keeps core dumps. 1 when installed. */
static inline int quiet_death_install(void)
{
    const char *keep = getenv(QUIET_DEATH_KEEP_CORES_ENV);
    struct sigaction sa;
    size_t i;

    if (keep && keep[0] != '\0' && strcmp(keep, "0") != 0)
        return 0;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = quiet_death_handler;
    sa.sa_flags = SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (i = 0; i < sizeof(quiet_death_signals) / sizeof(quiet_death_signals[0]); i++)
        sigaction(quiet_death_signals[i], &sa, NULL);
    return 1;
}

#endif
