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

#include <stdarg.h>

#include <lv2/urid/urid.h>
#include <lv2/log/log.h>

#include "lv2_core.h"

/* the providers, in the order a feature array lists them: omx_lv2_feature_kind's order */
enum lv2_feature_kind
{
    F_MAP = OMX_LV2_F_MAP,
    F_UNMAP = OMX_LV2_F_UNMAP,
    F_OPTIONS = OMX_LV2_F_OPTIONS,
    F_BOUNDED = OMX_LV2_F_BOUNDED,
    F_WORKER = OMX_LV2_F_WORKER,
    F_DEFAULT_STATE = OMX_LV2_F_DEFAULT_STATE,
    F_LOG = OMX_LV2_F_LOG,
    LV2_FEATURE_KINDS = OMX_LV2_FEATURE_KINDS
};

/* the features an array may hold, the buf-size declarations beyond bounded included */
#define LV2_FEATURE_SLOTS               OMX_LV2_FEATURE_SLOTS

/* the URI of a provider */
extern const char *const lv2_feature_uri[LV2_FEATURE_KINDS];

/* write `code` into `why`, NULL being no-op */
void lv2_why_set(char why[LV2_CORE_WHY_MAX], const char *code);

/* whether the provider `kind` is on the configured list */
int lv2_feature_on(enum lv2_feature_kind kind);

/* the kinds on the configured list, as OMX_LV2_FEATURE_BIT bits */
uint32_t lv2_features_configured(void);

/* whether `o` offers `uri` among the kinds `allowed` (OMX_LV2_FEATURE_BIT bits) */
int lv2_feature_offered_within(const struct omx_lv2_feature_options *o, uint32_t allowed, const char *uri);

/* `features` and the NULL-terminated `ptrs` filled with what `o` offers among the kinds `allowed`, in the kinds' order
 * and the buf-size declarations after them, kind k carrying data[k]: the count */
uint32_t lv2_features_fill(LV2_Feature features[LV2_FEATURE_SLOTS], const LV2_Feature *ptrs[LV2_FEATURE_SLOTS + 1],
                           const struct omx_lv2_feature_options *o, uint32_t allowed, void *const data[LV2_FEATURE_KINDS]);

/* the block lengths the options report for an activation of `min_frames` to `max_frames`: 0, or -1 when the minimum is
 * above the maximum or a length does not fit an Int */
int lv2_block_lengths(const struct omx_lv2_feature_options *o, uint32_t min_frames, uint32_t max_frames, int32_t *min,
                      int32_t *max, int32_t *nominal);

/* options:options over `rate` and the three lengths, which the options point to, ending in the empty option */
void lv2_options_fill(LV2_Options_Option opts[5], float *rate, int32_t *min, int32_t *max, int32_t *nominal);

/* one log:log line, as `o` says: what printf answers (0 when silent) */
int lv2_log_emit(const struct omx_lv2_feature_options *o, LV2_URID type, const char *fmt, va_list ap);

/* the process's one URID table, lock-free for a URI already mapped */
LV2_URID lv2_urid_lookup(const char *uri);
const char *lv2_urid_unlookup(LV2_URID id);

/* state:loadDefaultState over lilv-state, with the features `instantiate` was handed: 0, or -1 */
int lv2_plugin_default_state(const struct lv2_plugin *p, LV2_Handle handle, const LV2_Feature *const *features);

/* the state of a live handle as LV2 Turtle (malloc'd), read from its state:interface and its input controls: NULL on failure */
char *lv2_plugin_state_text(const struct lv2_plugin *p, LV2_Handle handle, const float *controls,
                            const LV2_Feature *const *features, LV2_URID_Map *map, LV2_URID_Unmap *unmap);

/* the state text restored into a live handle and its input controls: 0, or -1 when the text does not parse */
int lv2_plugin_state_restore(const struct lv2_plugin *p, LV2_Handle handle, float *controls, const char *text,
                             const LV2_Feature *const *features, LV2_URID_Map *map);

/* the state of a preset (malloc'd LV2 Turtle) for this plugin, NULL when the bundle holds no such preset applying to it */
char *lv2_plugin_preset_text(const struct lv2_plugin *p, const char *uri, LV2_URID_Map *map, LV2_URID_Unmap *unmap);

/* whether the state text parses: 0, or -1 */
int lv2_plugin_state_check(const char *text, LV2_URID_Map *map);

#endif
