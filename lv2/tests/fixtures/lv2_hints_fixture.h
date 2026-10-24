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
 * lv2_hints_fixture.h — the plugins of the resource hints' LV2 face (omx-contract's RESOURCE_HINTS as omxres
 * properties), one mono pad each, its bundle stating what the URI says: omx-hints-fixture.lv2 the two hints and none,
 * omx-hints-refused.lv2 the two refusals. The refused bundle is malformed on purpose and never validated.
 */
#ifndef OMX_TEST_LV2_HINTS_FIXTURE_H
#define OMX_TEST_LV2_HINTS_FIXTURE_H

#define HINTS_TWO           "urn:omx:test:hints:two"            // footprintAtRest 3145728, threadsSpawned 2: the rest unknown
#define HINTS_NONE          "urn:omx:test:hints:none"           // no omxres property
#define HINTS_WRONG_UNIT    "urn:omx:test:hints:wrong-unit"     // streamsFromDisk 1, an integer where a flag is a boolean
#define HINTS_OUT_OF_RANGE  "urn:omx:test:hints:out-of-range"   // threadsSpawned 4294967296, past a uint32_t
#define HINTS_AT_REST       3145728u
#define HINTS_THREADS       2u

#endif
