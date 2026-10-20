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
* The layout of a CLAP plugin as plugin-hostd's pin.h reads it: every parameter params.count() and get_info() give,
* hidden, read-only and bypass included, filled into pin.h's records. The bytes and the digest are pin.h's; nothing
* here serialises. Called on the main thread of an instance that is initialised and not yet activated.
*
* Beside it, the JACK name of each audio channel of that plugin: what omx-clap-host registers, and what omx-clap-scan
* and the audio_ports verb print, so a controller connects by the names the description gives.
*
************************************************************************************************************************
*/

#ifndef LAYOUT_PIN_H
#define LAYOUT_PIN_H


/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <clap/clap.h>
#include <plugin-hostd/pin.h>

#include "clap_host_limits.h"


/*
************************************************************************************************************************
*           INLINE FUNCTIONS
************************************************************************************************************************
*/

/* the auxiliary index that names a side's main port */
#define LAYOUT_PORT_MAIN        UINT32_MAX
/* a JACK port name of layout_port_name with its terminator: aux_out_ and two 32-bit numbers */
#define LAYOUT_PORT_NAME_SIZE   32

/* the JACK name of channel `k` (0-based) of a port: the main input's in_<k+1> and the main output's out_<k+1> (`aux`
 * LAYOUT_PORT_MAIN), auxiliary port `aux`'s (0-based, in the plugin's order of that side's non-main ports)
 * aux_in_<aux+1>_<k+1> and aux_out_<aux+1>_<k+1> */
static inline void layout_port_name(char buf[LAYOUT_PORT_NAME_SIZE], bool is_input, uint32_t aux, uint32_t k)
{
    if (aux == LAYOUT_PORT_MAIN)
        snprintf(buf, LAYOUT_PORT_NAME_SIZE, "%s_%u", is_input ? "in" : "out", k + 1);
    else
        snprintf(buf, LAYOUT_PORT_NAME_SIZE, "%s_%u_%u", is_input ? "aux_in" : "aux_out", aux + 1, k + 1);
}

typedef void (*layout_pin_sink_t)(void *ctx, const char *data, size_t size);

/* the layout of the plugin into sink as omx-layout/1 bytes, or into its digest with sink NULL; 0, or -1 when a
 * parameter cannot be read, memory runs out or the plugin reports more than CLAP_HOST_PARAM_COUNT_MAX parameters (a pin
 * names the whole layout, so a capped one would be a different plugin's). A plugin without the params extension has no
 * parameter. */
static inline int layout_pin_write(const clap_plugin_t *plugin, const clap_plugin_params_t *params,
                                   layout_pin_sink_t sink, void *ctx, char hex[PHD_SHA256_HEX_LEN + 1])
{
    const uint32_t count = params ? params->count(plugin) : 0;
    clap_param_info_t *infos = NULL;
    phd_layout_clap_param_t *records = NULL;
    uint32_t i;
    int ret = -1;

    if (count > CLAP_HOST_PARAM_COUNT_MAX)
        return -1;
    infos = calloc(count ? count : 1, sizeof(*infos));
    records = calloc(count ? count : 1, sizeof(*records));
    if (!infos || !records)
        goto out;
    for (i = 0; i < count; i++)
    {
        if (!params->get_info(plugin, i, &infos[i]))
            goto out;
        infos[i].name[sizeof(infos[i].name) - 1] = '\0';
        records[i].id = infos[i].id;
        records[i].name = infos[i].name;
        records[i].min = infos[i].min_value;
        records[i].max = infos[i].max_value;
        records[i].def = infos[i].default_value;
        records[i].flags = infos[i].flags;
    }
    ret = sink ? phd_layout_clap_write(records, count, sink, ctx) : phd_layout_clap_digest(records, count, hex);
out:
    free(records);
    free(infos);
    return ret;
}


/*
************************************************************************************************************************
*           END HEADER
************************************************************************************************************************
*/

#endif
