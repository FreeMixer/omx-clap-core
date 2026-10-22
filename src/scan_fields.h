/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * This file is part of omx-clap-host.
 *
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 */

#ifndef SCAN_FIELDS_H
#define SCAN_FIELDS_H

/* omx-clap-scan --fields <target>: the field dump of the plugin clap:<path>#<id> or lv2:<uri> on stdout. 0 written; 1
 * the plugin could not be read (nothing written); 2 usage. */
int scan_fields(const char *target);

/* omx-clap-scan --fields --list: every name the dump spells a CLAP value by, every LV2 field it asks and every prefix it
 * writes a URI under, one a line: what tests/scan-fields-check.mjs holds to the TERM files and the tables. 0. */
int scan_fields_list(void);

#endif
