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
* The LV2 feature provider of libomx-clap-lv2, with the options a host applies to it: the one URID table of the process,
* the log, the options:options a plugin reads its rate and block lengths from, the buf-size declarations, and the array
* `instantiate` takes. The console's LV2 host builds every instance's features through it; a host of its own (a test
* host, the qualifier's measurement) builds an array with omx_lv2_features_build and instantiates the plugin itself.
*
* The options are one struct, omx_lv2_feature_options. omx_lv2_feature_options_init fills it with what the console does
* when nobody says otherwise: every feature offered, a plugin requiring one the host does not offer refused, the block
* lengths of the activation declared bounded, and a log that formats and writes nothing.
*
* No CLAP here: a program that hosts LV2 alone includes this header and links the archive.
*
************************************************************************************************************************
*/

#ifndef OMX_LV2_FEATURES_H
#define OMX_LV2_FEATURES_H

#include <stddef.h>
#include <stdint.h>

#include <lv2/core/lv2.h>
#include <lv2/log/log.h>
#include <lv2/options/options.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The features a host may offer, in the order an array lists them. OMX_LV2_F_BOUNDED is the buf-size item: withheld,
 * no buf-size declaration is made at all; offered, the block options say which ones are. */
enum omx_lv2_feature_kind
{
    OMX_LV2_F_MAP,              /* urid:map */
    OMX_LV2_F_UNMAP,            /* urid:unmap */
    OMX_LV2_F_OPTIONS,          /* options:options */
    OMX_LV2_F_BOUNDED,          /* bufsz:boundedBlockLength, and the other buf-size declarations */
    OMX_LV2_F_WORKER,           /* worker:schedule */
    OMX_LV2_F_DEFAULT_STATE,    /* state:loadDefaultState */
    OMX_LV2_F_LOG,              /* log:log */
    OMX_LV2_F_RTMEMPOOL,        /* rtmempool#Pool, lv2dynparam's realtime-safe memory pool, and under its deprecated URI
                                 * (lv2dynparam/rtmempool/v1, the struct whose create orders its arguments differently),
                                 * a slot of its own after the declarations: the same pool */
    OMX_LV2_FEATURE_KINDS
};
#define OMX_LV2_FEATURE_BIT(k)          (1u << (k))
#define OMX_LV2_FEATURES_ALL            (OMX_LV2_FEATURE_BIT(OMX_LV2_FEATURE_KINDS) - 1u)

/* The buf-size declarations, made when OMX_LV2_F_BOUNDED is offered: each one a feature of its own in the array. A host
 * that declares FIXED runs every block at the declared maximum; one that declares POW2 runs blocks of a power of two. */
#define OMX_LV2_BLOCK_BOUNDED           (1u << 0)   /* bufsz:boundedBlockLength */
#define OMX_LV2_BLOCK_FIXED             (1u << 1)   /* bufsz:fixedBlockLength */
#define OMX_LV2_BLOCK_POW2              (1u << 2)   /* bufsz:powerOf2BlockLength */
#define OMX_LV2_BLOCK_ALL               (OMX_LV2_BLOCK_BOUNDED | OMX_LV2_BLOCK_FIXED | OMX_LV2_BLOCK_POW2)

/* The features an array may hold: every kind, the two buf-size declarations beyond bounded, and the pool's deprecated
 * URI. */
#define OMX_LV2_FEATURE_SLOTS           (OMX_LV2_FEATURE_KINDS + 3)

/* What log:log does with a line. */
enum omx_lv2_log_mode
{
    OMX_LV2_LOG_SILENT = 0,     /* formats nothing and writes nothing: a line from run() never reaches a system call */
    OMX_LV2_LOG_ROUTE = 1,      /* the formatted line to the host's sink */
    OMX_LV2_LOG_CAPTURE = 2,    /* the formatted line appended to the host's buffer */
};

/* The host's log sink: `type` is the line's log:Error, log:Warning, log:Note or log:Trace URI (NULL when the plugin
 * passed a URID the table does not know), `line` the formatted text as the plugin wrote it. */
typedef void (*omx_lv2_log_sink_fn)(void *ctx, const char *type, const char *line);

/* The options of the provider. Defaults: omx_lv2_feature_options_init. */
struct omx_lv2_feature_options
{
    uint32_t features;              /* OMX_LV2_FEATURE_BIT of each kind offered: OMX_LV2_FEATURES_ALL */
    int strict;                     /* 1: a plugin requiring a feature not offered is refused; 0: it is loaded
                                     * without it, to degrade or refuse in its own instantiate: 1 */
    struct
    {
        uint32_t declares;          /* OMX_LV2_BLOCK_* made when OMX_LV2_F_BOUNDED is offered: OMX_LV2_BLOCK_BOUNDED.
                                     * OMX_LV2_BLOCK_FIXED is a promise the host keeps (every run() the maximum's
                                     * length): the minimum and nominal reported are then the maximum, whatever `min`
                                     * and `nominal` name */
        uint32_t min;               /* bufsz:minBlockLength; 0: the activation's: 0 (overruled by OMX_LV2_BLOCK_FIXED) */
        uint32_t max;               /* bufsz:maxBlockLength; 0: the activation's: 0 */
        uint32_t nominal;           /* bufsz:nominalBlockLength; 0: the maximum reported: 0 (overruled by FIXED) */
    } block;
    struct
    {
        enum omx_lv2_log_mode mode; /* OMX_LV2_LOG_SILENT */
        omx_lv2_log_sink_fn sink;   /* ROUTE: called with each line: NULL */
        void *ctx;                  /* ROUTE: the sink's first argument: NULL */
        char *capture;              /* CAPTURE: a NUL-terminated buffer the lines are appended to, each ending in a
                                     * newline, the last one cut short when it is full; one writer at a time: NULL */
        size_t capture_size;        /* CAPTURE: the buffer's bytes, with its NUL: 0 */
    } log;
};

/* What omx_lv2_features_build fills and the array points into: build it where it stays for the instance's life. */
struct omx_lv2_feature_block
{
    struct omx_lv2_feature_options options;
    LV2_URID_Map map;
    LV2_URID_Unmap unmap;
    LV2_Log_Log log;
    float rate;
    int32_t min_block, max_block, nominal_block;
    LV2_Options_Option opts[5];
    LV2_Feature features[OMX_LV2_FEATURE_SLOTS];
    const LV2_Feature *ptrs[OMX_LV2_FEATURE_SLOTS + 1];
};

/* The URI of each kind. */
extern const char *const omx_lv2_feature_uris[OMX_LV2_FEATURE_KINDS];

/* `o` filled with the console's behaviour, the defaults each field names. */
void omx_lv2_feature_options_init(struct omx_lv2_feature_options *o);

/* Whether `o` says something the provider can do: 0, or -1 for a bit outside the kinds or the declarations, a log mode
 * that is none of the three, ROUTE without a sink, CAPTURE without a buffer, or a block minimum above its maximum. */
int omx_lv2_feature_options_check(const struct omx_lv2_feature_options *o);

/* The kind serving `uri`, or -1 when the provider has none. */
int omx_lv2_feature_kind(const char *uri);

/* The kinds of a NULL-terminated URI list (NULL: the empty list) in `*mask`: 0, or -1, `*mask` untouched, when the list
 * names a feature no kind serves. */
int omx_lv2_feature_mask(const char *const *uris, uint32_t *mask);

/* Whether `o` (NULL: the defaults) offers `uri`: a kind it offers, or a buf-size declaration it makes. */
int omx_lv2_feature_offered(const struct omx_lv2_feature_options *o, const char *uri);

/* Whether a plugin requiring the NULL-terminated `required` (NULL: nothing) is admitted under `o` (NULL: the defaults).
 * `*missing` (when `missing` is not NULL) is the first required URI not offered, NULL when every one is. 0: admitted,
 * every one offered or the options not strict; -1: refused, strict and one missing. */
int omx_lv2_features_admit(const struct omx_lv2_feature_options *o, const char *const *required, const char **missing);

/* The process's one URI table: a URID is the same in every instance and never changes meaning. map: 0 for NULL or when
 * the table is full; lock-free for a URI already mapped. unmap: NULL for 0 or an id never handed out. */
LV2_URID omx_lv2_urid_map(const char *uri);
const char *omx_lv2_urid_unmap(LV2_URID id);

/* Fill `b` with the features `o` (NULL: the defaults) offers, in the kinds' order and the declarations after them,
 * NULL-terminated, and return the array `instantiate` takes. The options carry param:sampleRate `rate` and the block
 * lengths: `min_frames` and `max_frames` unless the options name others, the nominal the maximum unless they name one.
 * `schedule` serves worker:schedule (the instance's own). NULL, nothing built, when the options do not check, when the
 * worker is offered and `schedule` is NULL, or when the block lengths reported have a minimum above their maximum. */
const LV2_Feature *const *omx_lv2_features_build(struct omx_lv2_feature_block *b, const struct omx_lv2_feature_options *o,
                                                 double rate, uint32_t min_frames, uint32_t max_frames,
                                                 LV2_Worker_Schedule *schedule);

/* Open the LV2 binary at `path` and walk its lv2_descriptor to `uri`. The descriptor, `*so` holding the handle to
 * dlclose after the last instance; NULL, `*so` NULL and nothing left loaded, with `*why` (when not NULL) the step that
 * refused: "dlopen", "lv2_descriptor" or "uri". */
const LV2_Descriptor *omx_lv2_open(void **so, const char *path, const char *uri, const char **why);

#ifdef __cplusplus
}
#endif

#endif
