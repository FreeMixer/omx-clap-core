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
 *
 * Built with FAKE_GUI_WAYLAND (tests/fake_gui_wl.clap, #329) it offers a Wayland window too, floating only as CLAP
 * defines it, unless FAKE_GUI_NO_WAYLAND=1: create("wayland", true) opens a connection of its own to the compositor
 * WAYLAND_DISPLAY names, whose descriptor and timer the host serves as for X11; show makes an xdg-shell toplevel titled
 * as suggest_title says, hide takes it off. It logs as well "wayland configured" once after each show when the compositor
 * has configured the toplevel and its buffer is attached, and "wayland frame" when the compositor has drawn it.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <clap/clap.h>
#include <xcb/xcb.h>
#ifdef FAKE_GUI_WAYLAND
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#endif

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

#ifdef FAKE_GUI_WAYLAND
static struct wl_display *g_wl;     // the plugin's own connection to the compositor, while its Wayland window exists
static struct wl_registry *g_registry;
static struct wl_compositor *g_compositor;
static struct wl_shm *g_shm;
static struct xdg_wm_base *g_wm_base;
static struct wl_surface *g_surface;
static struct xdg_surface *g_xdg_surface;
static struct xdg_toplevel *g_toplevel;
static struct wl_buffer *g_buffer;
static int g_configured_said, g_frame_said;
#endif

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

/* the window's connection, X or Wayland, is open: created and not destroyed */
static int gui_open(void)
{
#ifdef FAKE_GUI_WAYLAND
    if (g_wl)
        return 1;
#endif
    return g_x != NULL;
}

static void plugin_destroy(const clap_plugin_t *plugin)
{
    if (gui_open())
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

#ifdef FAKE_GUI_WAYLAND
static int wayland_offered(void)
{
    return !(getenv("FAKE_GUI_NO_WAYLAND") && !strcmp(getenv("FAKE_GUI_NO_WAYLAND"), "1"));
}

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener g_wm_base_listener = { wm_base_ping };

static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    (void)data;
    (void)version;
    if (!strcmp(interface, wl_compositor_interface.name))
        g_compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
    else if (!strcmp(interface, wl_shm_interface.name))
        g_shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (!strcmp(interface, xdg_wm_base_interface.name))
    {
        g_wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(g_wm_base, &g_wm_base_listener, NULL);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener g_registry_listener = { registry_global, registry_global_remove };

/* a white buffer of the window's size, in shared memory */
static struct wl_buffer *wl_white_buffer(void)
{
    const size_t stride = (size_t)g_width * 4u, size = stride * g_height;
    struct wl_shm_pool *pool;
    struct wl_buffer *buffer;
    void *pixels;
    int fd = memfd_create("fake-gui", MFD_CLOEXEC);

    if (fd < 0)
        return NULL;
    if (ftruncate(fd, (off_t)size) != 0 || (pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED)
    {
        close(fd);
        return NULL;
    }
    memset(pixels, 0xff, size);
    munmap(pixels, size);
    pool = wl_shm_create_pool(g_shm, fd, (int32_t)size);
    buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)g_width, (int32_t)g_height, (int32_t)stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

static void frame_done(void *data, struct wl_callback *callback, uint32_t time)
{
    (void)data;
    (void)time;
    wl_callback_destroy(callback);
    if (g_shown && !g_frame_said)
        say("wayland frame");
    g_frame_said |= g_shown;
}

static const struct wl_callback_listener g_frame_listener = { frame_done };

/* the compositor configured the toplevel: acknowledged, and the first time after a show its buffer attached, with a
 * frame callback that says when the compositor drew it */
static void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(surface, serial);
    if (g_configured_said || !g_surface)
        return;
    if (!g_buffer)
        g_buffer = wl_white_buffer();
    wl_surface_attach(g_surface, g_buffer, 0, 0);
    wl_surface_damage(g_surface, 0, 0, (int32_t)g_width, (int32_t)g_height);
    wl_callback_add_listener(wl_surface_frame(g_surface), &g_frame_listener, NULL);
    wl_surface_commit(g_surface);
    g_configured_said = 1;
    say("wayland configured %s", g_buffer ? "with its buffer" : "without a buffer");
}

static const struct xdg_surface_listener g_xdg_surface_listener = { xdg_surface_configure };

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height, struct wl_array *states)
{
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
    (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)data;
    (void)toplevel;
}

static const struct xdg_toplevel_listener g_toplevel_listener = { toplevel_configure, toplevel_close, NULL, NULL };

/* the plugin's own connection to the compositor, the globals its window needs, and the descriptor and timer it lives
 * on */
static bool wl_create(void)
{
    if (!(g_wl = wl_display_connect(NULL)))
        return false;
    g_registry = wl_display_get_registry(g_wl);
    wl_registry_add_listener(g_registry, &g_registry_listener, NULL);
    if (wl_display_roundtrip(g_wl) < 0 || !g_compositor || !g_shm || !g_wm_base)
    {
        wl_display_disconnect(g_wl);
        g_wl = NULL;
        g_compositor = NULL;
        g_shm = NULL;
        g_wm_base = NULL;
        return false;
    }
    g_fd = wl_display_get_fd(g_wl);
    if (g_fds && !g_fds->register_fd(g_host, g_fd, CLAP_POSIX_FD_READ))
        g_fd = -1;
    if (g_timers && !g_timers->register_timer(g_host, FAKE_GUI_TIMER_MS, &g_timer))
        g_timer = CLAP_INVALID_ID;
    g_width = FAKE_GUI_WIDTH;
    g_height = FAKE_GUI_HEIGHT;
    g_shown = 0;
    return true;
}

/* the toplevel, titled; its buffer goes on at the compositor's first configure */
static void wl_map(void)
{
    g_surface = wl_compositor_create_surface(g_compositor);
    g_xdg_surface = xdg_wm_base_get_xdg_surface(g_wm_base, g_surface);
    xdg_surface_add_listener(g_xdg_surface, &g_xdg_surface_listener, NULL);
    g_toplevel = xdg_surface_get_toplevel(g_xdg_surface);
    xdg_toplevel_add_listener(g_toplevel, &g_toplevel_listener, NULL);
    xdg_toplevel_set_title(g_toplevel, g_title);
    xdg_toplevel_set_app_id(g_toplevel, "org.omx-clap-host.test.gui");
    g_configured_said = g_frame_said = 0;
    wl_surface_commit(g_surface);
    wl_display_flush(g_wl);
}

static void wl_unmap(void)
{
    if (g_toplevel)
        xdg_toplevel_destroy(g_toplevel);
    if (g_xdg_surface)
        xdg_surface_destroy(g_xdg_surface);
    if (g_surface)
        wl_surface_destroy(g_surface);
    g_toplevel = NULL;
    g_xdg_surface = NULL;
    g_surface = NULL;
    wl_display_flush(g_wl);
}

static void wl_destroy(void)
{
    wl_unmap();
    if (g_buffer)
        wl_buffer_destroy(g_buffer);
    xdg_wm_base_destroy(g_wm_base);
    wl_shm_destroy(g_shm);
    wl_compositor_destroy(g_compositor);
    wl_registry_destroy(g_registry);
    wl_display_flush(g_wl);
    wl_display_disconnect(g_wl);
    g_wl = NULL;
    g_registry = NULL;
    g_compositor = NULL;
    g_shm = NULL;
    g_wm_base = NULL;
    g_buffer = NULL;
}

/* what the compositor sent, read off the descriptor the host saw readable; never blocking when nothing came */
static void wl_read(void)
{
    struct pollfd p = { .fd = wl_display_get_fd(g_wl), .events = POLLIN };

    while (wl_display_prepare_read(g_wl) != 0)
        wl_display_dispatch_pending(g_wl);
    wl_display_flush(g_wl);
    if (poll(&p, 1, 0) == 1 && (p.revents & POLLIN))
        wl_display_read_events(g_wl);
    else
        wl_display_cancel_read(g_wl);
    wl_display_dispatch_pending(g_wl);
    wl_display_flush(g_wl);
}
#endif

static bool gui_is_api_supported(const clap_plugin_t *plugin, const char *api, bool is_floating)
{
    (void)plugin;
#ifdef FAKE_GUI_WAYLAND
    if (api && !strcmp(api, CLAP_WINDOW_API_WAYLAND))
        return is_floating && wayland_offered();
#endif
    return api && !strcmp(api, CLAP_WINDOW_API_X11) && is_floating == (g_floating != 0);
}

static bool gui_get_preferred_api(const clap_plugin_t *plugin, const char **api, bool *is_floating)
{
    (void)plugin;
#ifdef FAKE_GUI_WAYLAND
    if (wayland_offered())
    {
        *api = CLAP_WINDOW_API_WAYLAND;
        *is_floating = true;
        return true;
    }
#endif
    *api = CLAP_WINDOW_API_X11;
    *is_floating = g_floating != 0;
    return true;
}

static bool gui_create(const clap_plugin_t *plugin, const char *api, bool is_floating)
{
    (void)plugin;
    say("create %s %d", api ? api : "-", is_floating);
    if (!gui_is_api_supported(plugin, api, is_floating) || gui_open())
        return false;
#ifdef FAKE_GUI_WAYLAND
    if (!strcmp(api, CLAP_WINDOW_API_WAYLAND))
        return wl_create();
#endif
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
#ifdef FAKE_GUI_WAYLAND
    if (g_wl)
        wl_destroy();
#endif
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
#ifdef FAKE_GUI_WAYLAND
    if (g_wl)
    {
        if (!g_surface)
            wl_map();
        g_shown = 1;
        g_shown_timers = 0;
        g_fd_said = g_timer_said = 0;
        return true;
    }
#endif
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
#ifdef FAKE_GUI_WAYLAND
    if (g_wl)
    {
        wl_unmap();
        g_shown = 0;
        return true;
    }
#endif
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
    if (fd != g_fd || !gui_open())
        return;
#ifdef FAKE_GUI_WAYLAND
    if (g_wl)
        wl_read();
#endif
    while (g_x && (e = xcb_poll_for_event(g_x)))
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
