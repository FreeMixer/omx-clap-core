/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */

/*
 * What lv2_path_fixture.c records for a test to read: the process's one probe, exported by the fixture's binary as
 * `lv2_path_probe`. A test fixture, never a shipped plugin.
 */

#ifndef OMX_TEST_LV2_PATH_PROBE_H
#define OMX_TEST_LV2_PATH_PROBE_H

#define LV2_PATH_FIXTURE_URI "urn:openmixer:test:path-fixture"

struct lv2_path_probe
{
    int had_make_path;          /* instantiate was handed state:makePath */
    char made[512];             /* the path the last save had made, "" when makePath answered NULL */
    char stored[512];           /* the path the last save stored, as mapPath abstracted it */
    char restored_from[512];    /* the path the last restore read, as mapPath made it absolute */
    float restored;             /* the level the last restore read from that file, -1 when it read none */
    int escape_null;            /* #escape: makePath answered NULL for "../outside.txt" */
    int absolute_null;          /* #escape: makePath answered NULL for an absolute path */
    void *make_path;            /* #escape: the LV2_State_Make_Path it was handed, for a test to call */
    int had_map_path;           /* #mappath: instantiate was handed state:mapPath */
    int had_free_path;          /* #mappath: instantiate was handed state:freePath */
    int freed_by_host;          /* #mappath: paths handed back through freePath */
};

#endif
