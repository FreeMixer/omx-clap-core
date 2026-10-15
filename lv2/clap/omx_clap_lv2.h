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
  const char *state_root;       /* an absolute directory, made by the configure: state:makePath makes each plugin
                                 * instance's directory in it. NULL: state:makePath is not offered */
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

/* [main-thread] The directory a state save of `plugin` (one this adapter created) copies the files its state names to,
 * <state_root>/lv2-<pid>-<n>.saved: its length, and as much of it as `size` holds, NUL-terminated, in `buf`; 0 and ""
 * without a state root. The adapter never removes it: the saved state names the copies by their absolute path. */
uint32_t omx_clap_lv2_saved_dir(const clap_plugin_t *plugin, char *buf, uint32_t size);

/* [main-thread] Remove a save's copy directory and everything in it, when the host deletes the states that name it:
 * 0, or -1 with `why` hosting.state.not-a-copy for a path that is not a lv2-<pid>-<n>.saved directory directly under the
 * configured state root (a symbolic link included), or with no state root. A link inside is removed, never followed. */
int omx_clap_lv2_saved_drop(const char *saved_dir, char why[OMX_CLAP_LV2_WHY_MAX]);

/* A string or path property of the plugin: an LV2 patch:writable property whose rdfs:range is atom:String or atom:Path.
 * CLAP has no text parameter, so the adapter carries these through the plugin's own patch port, with the functions
 * below. */
typedef struct omx_clap_lv2_text_info {
  const char *uri;              /* the property's URI, as omx_clap_lv2_patch_text_set names it */
  const char *name;             /* rdfs:label, else the URI */
  int is_path;                  /* 1: atom:Path, 0: atom:String */
  uint32_t max_bytes;           /* the longest value, without its NUL: 4095, or less when the patch port's buffer is
                                 * smaller (the configured atom_buffer_bytes or the port's rsz:minimumSize, less 80) */
} omx_clap_lv2_text_info_t;

/* [main-thread] The text properties of a plugin this adapter created, in the order of their URIs: their count, and the one
 * at `index` (false past the count, or for a plugin the adapter did not create). */
uint32_t omx_clap_lv2_patch_text_count(const clap_plugin_t *plugin);
bool omx_clap_lv2_patch_text_info(const clap_plugin_t *plugin, uint32_t index, omx_clap_lv2_text_info_t *info);

/* [main-thread] Write the text property `uri`: a patch:Set at frame 0 of the next block that runs, and again to the LV2
 * instance a later activate makes. 0, or -1 with `why`: hosting.patch.not-writable (the plugin lists no such property,
 * or the adapter did not create it), hosting.patch.too-large (longer than its max_bytes), hosting.patch.busy (while
 * active, the previous write still waits for its block). */
int omx_clap_lv2_patch_text_set(const clap_plugin_t *plugin, const char *uri, const char *value, char why[OMX_CLAP_LV2_WHY_MAX]);

/* [main-thread] The text property `uri` as the plugin last reported it (a patch:Set or patch:Put on its patch output),
 * else as the host last wrote it, else empty: its length without the NUL, and as much of it as `size` holds,
 * NUL-terminated, in `buf`. 0 and an empty `buf` for a property the plugin does not list. */
uint32_t omx_clap_lv2_patch_text_get(const clap_plugin_t *plugin, const char *uri, char *buf, uint32_t size);

/* [main-thread] FOR TESTS ONLY: block the caller until the plugin's LV2 worker has serviced every pending request, so
 * a test that compares two hosts pins the worker's schedule. The real worker thread runs; only the schedule is pinned. */
void omx_clap_lv2_worker_quiesce(const clap_plugin_t *plugin);

#ifdef __cplusplus
}
#endif

#endif
