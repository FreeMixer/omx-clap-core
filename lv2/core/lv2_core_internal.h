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

/* What the units of the LV2 host share among themselves and nobody else. */

#ifndef LV2_CORE_INTERNAL_H
#define LV2_CORE_INTERNAL_H

#include <lv2/urid/urid.h>
#include <lv2/log/log.h>

#include "lv2_core.h"

/* the providers, in the order a feature array lists them */
enum lv2_feature_kind
{
    F_MAP, F_UNMAP, F_OPTIONS, F_BOUNDED, F_WORKER, F_DEFAULT_STATE, F_LOG, LV2_FEATURE_KINDS
};

/* the URI of a provider */
extern const char *const lv2_feature_uri[LV2_FEATURE_KINDS];

/* write `code` into `why`, NULL being no-op */
void lv2_why_set(char why[LV2_CORE_WHY_MAX], const char *code);

/* whether `uri` is on the configured list (and so has a provider) */
int lv2_feature_configured(const char *uri);

/* whether the provider `kind` is on the configured list */
int lv2_feature_on(enum lv2_feature_kind kind);

/* the process's one URID table, lock-free for a URI already mapped */
LV2_URID lv2_urid_lookup(const char *uri);
const char *lv2_urid_unlookup(LV2_URID id);

/* state:loadDefaultState over lilv-state, with the features `instantiate` was handed: 0, or -1 */
int lv2_plugin_default_state(const struct lv2_plugin *p, LV2_Handle handle, const LV2_Feature *const *features);

#endif
