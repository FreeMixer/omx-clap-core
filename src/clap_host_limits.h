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

#ifndef CLAP_HOST_LIMITS_H
#define CLAP_HOST_LIMITS_H
/*
 * GENERATED — DO NOT EDIT BY HAND.
 * Rendered from HOSTING_IDLE_CODES and HOSTING_WINDOW_CODES of data/hosting.json, where this tree declares the codes
 * of the dimensions it owns. The stage's numbers are hosted_stage_limits.h's, spelled once. The rendered file is kept in
 * this repository so the library builds on its own; the codes are changed where they are declared and the file is
 * re-rendered, never edited here. Every other refusal code is declared in data/hosting/ and rendered as
 * omx_clap_core_vocabulary.h (clap_host.h's CLAP_HOST_CODE_*).
 */

/** @file
 * @brief The codes of the idle and window dimensions. */

#include <stddef.h>

/** the idle dimension, what the library answers about its hold of /dev/cpu_dma_latency (#296), and the window dimension, why omx-clap-host shows no plugin window (#180) */
#define CLAP_HOST_CODE_IDLE_HOLD_REFUSED "hosting.idle.hold-refused"  ///< the device could not be opened for writing (no permission, or no such device): the host runs with no hold
#define CLAP_HOST_CODE_WINDOW_NO_DISPLAY "hosting.window.no-display"  ///< no display to show it on: none is named, or its server refused the connection; the plugin runs on
#define CLAP_HOST_CODE_WINDOW_UNSUPPORTED "hosting.window.unsupported"  ///< the plugin has no clap.gui, or its clap.gui offers no window API the host shows, embedded or floating
#define CLAP_HOST_CODE_WINDOW_REFUSED "hosting.window.refused"  ///< the plugin's clap.gui answered false when asked to create, embed or show its window

#endif /* CLAP_HOST_LIMITS_H */
