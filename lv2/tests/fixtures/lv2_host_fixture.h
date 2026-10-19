/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/*
 * The host fixture's witnesses, read by the tests through the fixture's own symbols. A test fixture, never a shipped
 * plugin.
 */
#ifndef LV2_HOST_FIXTURE_H
#define LV2_HOST_FIXTURE_H

/* the #inplace twin's: runs, and the runs whose input and output were one buffer (never, under this host) */
struct lv2_host_fixture_inplace
{
    unsigned runs;
    unsigned aliased;
};
extern struct lv2_host_fixture_inplace lv2_host_fixture_inplace;

#endif
