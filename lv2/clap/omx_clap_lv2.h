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
* libomx-clap-lv2: one LV2 plugin presented as one CLAP plugin, so a CLAP host runs it the way it runs any CLAP. The
* whole public surface. A host configures the adapter once, takes the entry of a bundle, and from there on uses the
* entry as it would a .clap file's: init, get_factory, create_plugin. The adapter is linked by the hosts that trust it;
* it is never installed as a .clap file a third-party host could load.
*
* Stage 1: 1x1 or 2x2 audio, the control ports as clap.params, the latency port as clap.latency, the plugin's own
* bypass held, the default state, the configured features with the LV2 worker. Everything else is refused by
* create_plugin with the verdict's code, which the adapter writes to the host's clap.log.
*
************************************************************************************************************************
*/

#ifndef OMX_CLAP_LV2_H
#define OMX_CLAP_LV2_H

#include <clap/entry.h>

#include "omx_lv2_features.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OMX_CLAP_LV2_WHY_MAX 96 /* a refusal code, the verdict's spelling */

/* What the host hands the adapter once, before any entry: the LV2 features it may provide (a
 * NULL-terminated URI list; a URI the adapter has no provider for refuses the configure, and the
 * adapter provides nothing outside the list), and the declared numbers it obeys. */
typedef struct omx_clap_lv2_config {
  const char *const *features;  /* the engine: OMX_INPROCESS_FEATURES; the worker: omx_clap_lv2_provided */
  uint32_t worker_poll_us;      /* the engine: HOSTED_STAGE_LIMITS.workerPollUs */
  uint32_t worker_ring_bytes;   /* the engine: HOSTED_STAGE_LIMITS.workerRingBytes; a power of two */
  uint32_t atom_buffer_bytes;   /* the smallest atom port buffer; a port's rsz:minimumSize raises it */
  uint32_t latency_hold_ms;     /* §6.7 */
  const char *lilv_soname;      /* NULL: liblilv-0.so.0 */
} omx_clap_lv2_config_t;

/* [main-thread] 0, or -1 with `why` naming the refusal. Called once per process; a second call
 * with another list is refused. */
int omx_clap_lv2_configure(const omx_clap_lv2_config_t *cfg, char why[OMX_CLAP_LV2_WHY_MAX]);

/* [main-thread] The feature options of the plugins created from now on (omx_lv2_features.h), NULL being the defaults,
 * which are what the adapter did before it had options: 0, or -1 with `why` when they do not check, the options then
 * unchanged. A feature the configured list does not name is never offered, whatever the options say. The sink and the
 * capture buffer they name must outlive every plugin created under them. */
int omx_clap_lv2_feature_options(const struct omx_lv2_feature_options *o, char why[OMX_CLAP_LV2_WHY_MAX]);

/* Every feature URI the adapter can provide, NULL-terminated: what the worker hands back. */
extern const char *const omx_clap_lv2_provided[];

/* [main-thread] The entry for ONE bundle (an absolute directory), reference-counted: the bundle
 * is loaded on the first call and unloaded when the last reference is released. NULL with `why`
 * set when lilv cannot be loaded or the bundle cannot be read. The entry's factory lists the
 * bundle's plugins, each with descriptor id = its LV2 URI. */
const clap_plugin_entry_t *omx_clap_lv2_entry(const char *bundle_path, char why[OMX_CLAP_LV2_WHY_MAX]);
void omx_clap_lv2_entry_release(const clap_plugin_entry_t *entry);

/* [main-thread] FOR TESTS ONLY: block the caller until the plugin's LV2 worker has serviced every pending request, so
 * a test that compares two hosts pins the worker's schedule. The real worker thread runs; only the schedule is pinned. */
void omx_clap_lv2_worker_quiesce(const clap_plugin_t *plugin);

/* [main-thread] The code of the last refusal on the calling thread: what create_plugin, init or activate of a plugin
 * this adapter makes wrote to the host's clap.log before it failed (a hosting.* code), "" when the last of those calls
 * did not fail. A host that sees create_plugin return NULL, or init or activate return false, reads the reason here
 * rather than from its log. */
const char *omx_clap_lv2_last_refusal(void);

/* [main-thread] The lv2:symbol of the control port that is the parameter `id` (its port index) of a plugin this adapter
 * created; NULL for an id that is no control port's (a patch parameter, the latency port, an audio port) or for a
 * plugin the adapter did not create. */
const char *omx_clap_lv2_param_symbol(const clap_plugin_t *plugin, clap_id id);

#ifdef __cplusplus
}
#endif

#endif
