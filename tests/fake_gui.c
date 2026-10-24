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
 * A plugin built for the tests of the host's window (#180): org.omx-clap-host.test.gui, a stereo plugin that halves its
 * input, with a clap.gui of the X11 API. Its window lives as a real plugin's does: create opens a connection of its own
 * to the display DISPLAY names, and registers that connection's descriptor (clap.posix-fd-support) and a timer of
 * FAKE_GUI_TIMER_MS (clap.timer-support) with the host, which must serve both on its tick. It
 *  - embeds (set_parent: a child window of the host's, filled, named "fake gui view") and resizes (can_resize, any size),
 *    or with FAKE_GUI_FLOATING=1 only floats: a top-level window of its own, titled as suggest_title says;
 *  - with FAKE_GUI_RESIZE=<w>x<h> asks the host for that size (request_resize) once, on its fifth timer while shown;
 *  - appends a line per call to the file FAKE_GUI_LOG, in the order the calls come, so a test reads their order off the
 *    line numbers: "host gui offered" or "host gui none" at init, "create <api>
 *    <floating>", "scale", "set_parent", "show", "hide", "set_size <w> <h>", "title <title>", "destroy", "request_resize
 *    <w> <h> <answer>", and once after each show "timer on the tick" and "fd on the tick" when the host calls them.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <clap/clap.h>
#include <xcb/xcb.h>

#define ID_GUI              "org.omx-clap-host.test.gui"
#define FAKE_GUI_TIMER_MS   20u
#define FAKE_GUI_WIDTH      300u
#define FAKE_GUI_HEIGHT     200u

static const clap_host_t *g_host;
static const clap_plugin_t *g_plugin;
static const clap_host_gui_t *g_host_gui;
static const clap_host_timer_support_t *g_timers;
static const clap_host_posix_fd_support_t *g_fds;

static xcb_connection_t *g_x;
static xcb_screen_t *g_screen;
static int g_floating;
static uint32_t g_window;           // the plugin's own window: the child of the host's, or a top-level floating one
static uint32_t g_width = FAKE_GUI_WIDTH, g_height = FAKE_GUI_HEIGHT;
static char g_title[256] = "fake gui";
static clap_id g_timer = CLAP_INVALID_ID;
static int g_fd = -1;
static int g_shown;
static uint32_t g_shown_timers;     // timers since the last show
static int g_fd_said, g_timer_said;
static int g_resize_asked;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    const char *path = getenv("FAKE_GUI_LOG");
    FILE *f;
    va_list ap;

    if (!path || !(f = fopen(path, "a")))
        return;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static const char *const g_features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL };
static const clap_plugin_descriptor_t g_desc = { CLAP_VERSION_INIT, ID_GUI, "fake gui", "omx-clap-host", "", "", "", "0", "", g_features };

static bool plugin_init(const clap_plugin_t *plugin)
{
    (void)plugin;
    g_host_gui = g_host->get_extension(g_host, CLAP_EXT_GUI);
    g_timers = g_host->get_extension(g_host, CLAP_EXT_TIMER_SUPPORT);
    g_fds = g_host->get_extension(g_host, CLAP_EXT_POSIX_FD_SUPPORT);
    g_floating = getenv("FAKE_GUI_FLOATING") && !strcmp(getenv("FAKE_GUI_FLOATING"), "1");
    say("host gui %s", g_host_gui ? "offered" : "none");
    return true;
}

static void gui_destroy(const clap_plugin_t *plugin);

static void plugin_destroy(const clap_plugin_t *plugin)
{
    if (g_x)
        gui_destroy(plugin);
    g_host = NULL;
    g_plugin = NULL;
}

static bool plugin_activate(const clap_plugin_t *plugin, double rate, uint32_t min_frames, uint32_t max_frames)
{
    (void)plugin;
    (void)rate;
    (void)min_frames;
    (void)max_frames;
    return true;
}

static void plugin_deactivate(const clap_plugin_t *plugin) { (void)plugin; }
static bool plugin_start_processing(const clap_plugin_t *plugin) { (void)plugin; return true; }
static void plugin_stop_processing(const clap_plugin_t *plugin) { (void)plugin; }
static void plugin_reset(const clap_plugin_t *plugin) { (void)plugin; }

static clap_process_status plugin_process(const clap_plugin_t *plugin, const clap_process_t *process)
{
    (void)plugin;
    for (uint32_t c = 0; c < 2; c++)
        for (uint32_t i = 0; i < process->frames_count; i++)
            process->audio_outputs[0].data32[c][i] = 0.5f * process->audio_inputs[0].data32[c][i];
    return CLAP_PROCESS_CONTINUE;
}

static uint32_t ports_count(const clap_plugin_t *plugin, bool is_input) { (void)plugin; (void)is_input; return 1; }

static bool ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_audio_port_info_t *info)
{
    (void)plugin;
    if (index != 0)
        return false;
    memset(info, 0, sizeof(*info));
    info->id = 0;
    snprintf(info->name, sizeof(info->name), "%s", is_input ? "in" : "out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

/* clap.gui */

static bool gui_is_api_supported(const clap_plugin_t *plugin, const char *api, bool is_floating)
{
    (void)plugin;
    return api && !strcmp(api, CLAP_WINDOW_API_X11) && is_floating == (g_floating != 0);
}

static bool gui_get_preferred_api(const clap_plugin_t *plugin, const char **api, bool *is_floating)
{
    (void)plugin;
    *api = CLAP_WINDOW_API_X11;
    *is_floating = g_floating != 0;
    return true;
}

static bool gui_create(const clap_plugin_t *plugin, const char *api, bool is_floating)
{
    (void)plugin;
    say("create %s %d", api ? api : "-", is_floating);
    if (!gui_is_api_supported(plugin, api, is_floating) || g_x)
        return false;
    g_x = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(g_x))
    {
        xcb_disconnect(g_x);
        g_x = NULL;
        return false;
    }
    g_screen = xcb_setup_roots_iterator(xcb_get_setup(g_x)).data;
    g_fd = xcb_get_file_descriptor(g_x);
    if (g_fds && !g_fds->register_fd(g_host, g_fd, CLAP_POSIX_FD_READ))
        g_fd = -1;
    if (g_timers && !g_timers->register_timer(g_host, FAKE_GUI_TIMER_MS, &g_timer))
        g_timer = CLAP_INVALID_ID;
    g_width = FAKE_GUI_WIDTH;
    g_height = FAKE_GUI_HEIGHT;
    g_shown = 0;
    return true;
}

static void gui_destroy(const clap_plugin_t *plugin)
{
    (void)plugin;
    say("destroy");
    if (g_fd >= 0 && g_fds)
        g_fds->unregister_fd(g_host, g_fd);
    if (g_timer != CLAP_INVALID_ID && g_timers)
        g_timers->unregister_timer(g_host, g_timer);
    g_fd = -1;
    g_timer = CLAP_INVALID_ID;
    if (g_x)
    {
        if (g_window)
            xcb_destroy_window(g_x, g_window);
        xcb_flush(g_x);
        xcb_disconnect(g_x);
    }
    g_x = NULL;
    g_window = 0;
    g_shown = 0;
}

static bool gui_set_scale(const clap_plugin_t *plugin, double scale)
{
    (void)plugin;
    say("scale %.1f", scale);
    return true;
}

static bool gui_get_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height)
{
    (void)plugin;
    *width = g_width;
    *height = g_height;
    return true;
}

static bool gui_can_resize(const clap_plugin_t *plugin) { (void)plugin; return true; }

static bool gui_get_resize_hints(const clap_plugin_t *plugin, clap_gui_resize_hints_t *hints)
{
    (void)plugin;
    memset(hints, 0, sizeof(*hints));
    hints->can_resize_horizontally = true;
    hints->can_resize_vertically = true;
    return true;
}

static bool gui_adjust_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height)
{
    (void)plugin;
    (void)width;
    (void)height;
    return true;
}

static void window_size(uint32_t width, uint32_t height)
{
    const uint32_t size[2] = { width, height };

    g_width = width;
    g_height = height;
    if (g_window)
        xcb_configure_window(g_x, g_window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, size);
    xcb_flush(g_x);
}

static bool gui_set_size(const clap_plugin_t *plugin, uint32_t width, uint32_t height)
{
    (void)plugin;
    say("set_size %u %u", width, height);
    if (!g_x)
        return false;
    window_size(width, height);
    return true;
}

/* the plugin's window: a child of `parent`, or with 0 a top-level window of its own; filled with the screen's white */
static void window_make(uint32_t parent, const char *name)
{
    const uint32_t values[2] = { g_screen->white_pixel, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY };

    g_window = xcb_generate_id(g_x);
    xcb_create_window(g_x, XCB_COPY_FROM_PARENT, g_window, parent ? parent : g_screen->root, 0, 0, (uint16_t)g_width,
                      (uint16_t)g_height, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, g_screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
    xcb_change_property(g_x, XCB_PROP_MODE_REPLACE, g_window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, (uint32_t)strlen(name), name);
    xcb_flush(g_x);
}

static bool gui_set_parent(const clap_plugin_t *plugin, const clap_window_t *window)
{
    (void)plugin;
    say("set_parent %s", window && window->api ? window->api : "-");
    if (!g_x || g_floating || !window || strcmp(window->api, CLAP_WINDOW_API_X11) != 0 || !window->x11)
        return false;
    window_make((uint32_t)window->x11, "fake gui view");
    xcb_map_window(g_x, g_window);
    xcb_flush(g_x);
    return true;
}

static bool gui_set_transient(const clap_plugin_t *plugin, const clap_window_t *window)
{
    (void)plugin;
    (void)window;
    return g_floating != 0;
}

static void gui_suggest_title(const clap_plugin_t *plugin, const char *title)
{
    (void)plugin;
    say("title %s", title);
    snprintf(g_title, sizeof(g_title), "%s", title);
}

static bool gui_show(const clap_plugin_t *plugin)
{
    (void)plugin;
    say("show");
    if (!g_x)
        return false;
    if (g_floating && !g_window)
        window_make(0, g_title);
    xcb_map_window(g_x, g_window);
    xcb_flush(g_x);
    g_shown = 1;
    g_shown_timers = 0;
    g_fd_said = g_timer_said = 0;
    return true;
}

static bool gui_hide(const clap_plugin_t *plugin)
{
    (void)plugin;
    say("hide");
    if (!g_x)
        return false;
    if (g_floating)
        xcb_unmap_window(g_x, g_window);
    xcb_flush(g_x);
    g_shown = 0;
    return true;
}

static const clap_plugin_gui_t g_gui = {
    gui_is_api_supported, gui_get_preferred_api, gui_create, gui_destroy, gui_set_scale, gui_get_size, gui_can_resize,
    gui_get_resize_hints, gui_adjust_size, gui_set_size, gui_set_parent, gui_set_transient, gui_suggest_title, gui_show,
    gui_hide
};

/* the timer and the descriptor the window lives on, called by the host's tick */

static void on_timer(const clap_plugin_t *plugin, clap_id id)
{
    const char *resize = getenv("FAKE_GUI_RESIZE");
    unsigned width, height;

    (void)plugin;
    if (id != g_timer || !g_shown)
        return;
    if (!g_timer_said)
        say("timer on the tick");
    g_timer_said = 1;
    if (++g_shown_timers == 5 && resize && !g_resize_asked && !g_floating && sscanf(resize, "%ux%u", &width, &height) == 2)
    {
        const bool answer = g_host_gui && g_host_gui->request_resize(g_host, width, height);

        g_resize_asked = 1;
        say("request_resize %u %u %d", width, height, answer);
        if (answer)
            window_size(width, height);
    }
}

static void on_fd(const clap_plugin_t *plugin, int fd, clap_posix_fd_flags_t flags)
{
    xcb_generic_event_t *e;

    (void)plugin;
    if (fd != g_fd || !g_x)
        return;
    while ((e = xcb_poll_for_event(g_x)))
        free(e);
    if (g_shown && (flags & CLAP_POSIX_FD_READ) && !g_fd_said)
        say("fd on the tick");
    g_fd_said |= g_shown;
}

static const clap_plugin_audio_ports_t g_ports = { ports_count, ports_get };
static const clap_plugin_timer_support_t g_timer_ext = { on_timer };
static const clap_plugin_posix_fd_support_t g_fd_ext = { on_fd };

static const void *plugin_get_extension(const clap_plugin_t *plugin, const char *id)
{
    (void)plugin;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &g_ports;
    if (!strcmp(id, CLAP_EXT_GUI))
        return &g_gui;
    if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT))
        return &g_timer_ext;
    if (!strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT))
        return &g_fd_ext;
    return NULL;
}

static void plugin_on_main_thread(const clap_plugin_t *plugin) { (void)plugin; }

static clap_plugin_t g_the_plugin = { &g_desc, NULL, plugin_init, plugin_destroy, plugin_activate, plugin_deactivate,
                                      plugin_start_processing, plugin_stop_processing, plugin_reset, plugin_process,
                                      plugin_get_extension, plugin_on_main_thread };

static uint32_t factory_count(const clap_plugin_factory_t *factory) { (void)factory; return 1; }

static const clap_plugin_descriptor_t *factory_descriptor(const clap_plugin_factory_t *factory, uint32_t index)
{
    (void)factory;
    return index == 0 ? &g_desc : NULL;
}

/* one instance at a time */
static const clap_plugin_t *factory_create(const clap_plugin_factory_t *factory, const clap_host_t *host, const char *id)
{
    (void)factory;
    if (!id || strcmp(id, ID_GUI) != 0 || g_plugin)
        return NULL;
    g_host = host;
    g_plugin = &g_the_plugin;
    g_resize_asked = 0;
    return g_plugin;
}

static const clap_plugin_factory_t g_factory = { factory_count, factory_descriptor, factory_create };

static bool entry_init(const char *path) { (void)path; return true; }
static void entry_deinit(void) {}
static const void *entry_get_factory(const char *id) { return !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &g_factory : NULL; }

CLAP_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory };
