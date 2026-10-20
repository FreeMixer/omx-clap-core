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
 * @brief The host extensions libomx-clap-core offers a hosted plugin. */

/*
 * clap_host_extensions.h: the host extensions the core's host object offers a plugin. This file is their home: the list
 * is written here by hand and nowhere else. A consumer that names them (a qualifier profile judging which extensions a
 * plugin may ask for) mirrors this list and tests its mirror against it. The configuration adds clap.preset-load,
 * clap.track-info and clap.remote-controls on request (struct omx_clap_host_config); this is the list it starts from.
 */

#ifndef CLAP_HOST_EXTENSIONS_H
#define CLAP_HOST_EXTENSIONS_H

/** how many extensions the host object offers: the ids of CLAP_HOST_EXTENSIONS_INIT */
#define CLAP_HOST_EXTENSION_COUNT 16u
/** the host object offers exactly these extensions, NULL-terminated */
#define CLAP_HOST_EXTENSIONS_INIT { "clap.log", "clap.thread-check", "clap.latency", "clap.params", "clap.audio-ports", "clap.state", \
                                    "clap.note-ports", "clap.note-name", "clap.voice-info", "clap.event-registry", \
                                    "clap.timer-support", "clap.posix-fd-support", "clap.thread-pool", "clap.tail", \
                                    "clap.surround/4", "clap.ambisonic/3", NULL }

#endif ///< CLAP_HOST_EXTENSIONS_H
