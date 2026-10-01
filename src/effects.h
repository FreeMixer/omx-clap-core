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
************************************************************************************************************************
*/

#ifndef EFFECTS_H
#define EFFECTS_H


/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <stddef.h>

#include "host-errors.h"


/*
************************************************************************************************************************
*           CONFIGURATION DEFINES
************************************************************************************************************************
*/

#define MAX_PLUGIN_INSTANCES    9990
#define MAX_TOOL_INSTANCES      10
#define MAX_INSTANCES           (MAX_PLUGIN_INSTANCES + MAX_TOOL_INSTANCES)


/*
************************************************************************************************************************
*           FUNCTION PROTOTYPES
************************************************************************************************************************
*/

int effects_init(void);
int effects_finish(void);
int effects_add(const char *uri, int instance, const char *client_name);
int effects_pin_expect(int instance, const char *layout);
int effects_remove(int effect_id);
int effects_bypass(int effect_id, int value);
int effects_set_parameter(int effect_id, const char *control_symbol, float value);
int effects_get_parameter(int effect_id, const char *control_symbol, float *value);
int effects_preset_load(int effect_id, const char *location);
int effects_state_save(const char *dir);
int effects_state_load(const char *dir);
int effects_connect(const char *portA, const char *portB);
int effects_disconnect(const char *portA, const char *portB);
int effects_monitor_output(int effect_id, const char *symbol);
int effects_track_info(int effect_id, const char *name, const char *color, const char *kind);
int effects_remote_pages(int effect_id);
int effects_remote_page_get(int effect_id, int page, char *reply, size_t size);
int effects_param_info(int effect_id, const char *symbol);
float effects_jack_cpu_load(void);
void effects_idle(void);


/*
************************************************************************************************************************
*           END HEADER
************************************************************************************************************************
*/

#endif
