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
 * window.h: a plugin's own window, over CLAP's clap.gui with the Wayland or the X11 API, in the standalone omx-clap-host
 * only (#180, #329). A show tries Wayland floating first, when WAYLAND_DISPLAY names a compositor that answers: the plugin
 * opens its own toplevel. Then X11: the host makes one top-level X window per plugin and embeds the plugin's window in
 * it, or lets the plugin open a floating one when that is all it offers, or when it offers X11 embedded but has no
 * set_parent to be embedded with. Everything here runs on the control thread: the commands and the idle tick; the
 * plugin's clap.gui calls to the host, which may come from any thread, only leave a request the tick answers.
 * Wayland needs no library of the host's and is always built; built without xcb (no OMX_CLAP_HOST_X11, or X11=0), a show
 * tries Wayland alone, so a plugin with only an X11 window is CLAP_HOST_CODE_WINDOW_UNSUPPORTED there.
 */

#ifndef WINDOW_H
#define WINDOW_H

#include "clap_host.h"

/* offer the host's clap.gui to every plugin opened from now on: before the first add (omx_clap_host_offer_gui) */
void window_init(void);
/* show the plugin's window, made on the first show; `title` names it. 0, or -1 with *code the hosting code that says why
 * (CLAP_HOST_CODE_WINDOW_*); the plugin runs on either way */
int window_show(struct omx_clap_instance *in, const char *title, const char **code);
/* take the plugin's window off the display; it is kept for the next show. 0, also when it has none */
int window_hide(struct omx_clap_instance *in);
/* the plugin's window destroyed, before the plugin is: the remove. Nothing when it has none */
void window_close(struct omx_clap_instance *in);
/* the idle tick: the X events, and the plugin's requests (resize, show, hide, closed) */
void window_idle(void);
/* every window closed and the X connection with them */
void window_finish(void);

#endif
