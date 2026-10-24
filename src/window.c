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
 * window.c: a plugin's own window over clap.gui and X11 (window.h). The APIs a show tries are a table, in its order:
 * X11 embedded, X11 floating. X11 is one xcb connection, opened by the first show and closed with the last X window;
 * one top-level window per plugin that embeds, titled with the plugin's name, which the plugin's window is set into
 * (set_parent); none for a plugin that only floats, which opens its own. The X events are read on the idle tick: a
 * resize of the host's window is the plugin's set_size, its close button (WM_DELETE_WINDOW) hides the plugin. The
 * plugin's timers and descriptors, which a plugin's window lives on, are the library's tick (clap.timer-support,
 * clap.posix-fd-support).
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <clap/ext/gui.h>
#ifdef OMX_CLAP_HOST_X11
#include <xcb/xcb.h>
#endif

#include "clap_host_limits.h"
#include "effects.h"
#include "window.h"

#ifdef OMX_CLAP_HOST_X11
/* the size of a window whose plugin names none */
#define WINDOW_DEFAULT_WIDTH    400u
#define WINDOW_DEFAULT_HEIGHT   300u

/* what a plugin asked of the host from any thread, for the tick to answer */
#define WINDOW_REQ_RESIZE       (1u << 0)
#define WINDOW_REQ_HINTS        (1u << 1)
#define WINDOW_REQ_SHOW         (1u << 2)
#define WINDOW_REQ_HIDE         (1u << 3)
#define WINDOW_REQ_CLOSED       (1u << 4)
#define WINDOW_REQ_DESTROYED    (1u << 5)

/* a window API a show may take: the display it needs, found and let go */
struct window_api
{
    const char *name;               ///< CLAP_WINDOW_API_*
    int floating;                   ///< the plugin opens its own window; the host makes none
    int (*connect)(void);           ///< 0 when the display this API shows on answers, -1 when there is none
    void (*release)(void);          ///< the display let go once no window of this API is left
    uint32_t *open;                 ///< the windows of this API made and not yet destroyed
};

struct window
{
    struct omx_clap_instance *_Atomic in;   ///< the plugin whose window this is, NULL for a free slot
    const clap_plugin_gui_t *gui;
    const struct window_api *api;   ///< the API the window was made with
    int floating;                   ///< the plugin opens its own window; the host made none
    int created;                    ///< gui->create answered true, and no destroy since
    int visible;
    int resizable;                  ///< what gui->can_resize last answered
    uint32_t xid;                   ///< the host's window, 0 for a floating plugin
    uint32_t width, height;         ///< the size the host's window has, as the host last set it or the plugin agreed to
    _Atomic uint32_t requests;      ///< WINDOW_REQ_*
    _Atomic uint64_t request_size;  ///< the last request_resize: width << 32 | height
};

/* a slot per plugin instance, never freed: a plugin's thread that calls the host reads a slot that stays */
static struct window g_windows[MAX_PLUGIN_INSTANCES];
static _Atomic uint32_t g_high;     // the slots ever claimed: the calls look no further

static struct window *window_of(const struct omx_clap_instance *in)
{
    const uint32_t high = atomic_load_explicit(&g_high, memory_order_acquire);

    if (!in)
        return NULL;
    for (uint32_t i = 0; i < high; i++)
        if (atomic_load_explicit(&g_windows[i].in, memory_order_acquire) == in)
            return &g_windows[i];
    return NULL;
}

/* the plugin's call to the host's clap.gui: host_data is the instance (omx_clap_host_offer_gui) */
static struct window *window_of_host(const clap_host_t *host)
{
    return host ? window_of((const struct omx_clap_instance *)host->host_data) : NULL;
}

static void request(struct window *w, uint32_t bits)
{
    atomic_fetch_or_explicit(&w->requests, bits, memory_order_release);
}

static void host_resize_hints_changed(const clap_host_t *host)
{
    struct window *w = window_of_host(host);

    if (w)
        request(w, WINDOW_REQ_HINTS);
}

static bool host_request_resize(const clap_host_t *host, uint32_t width, uint32_t height)
{
    struct window *w = window_of_host(host);

    if (!w || w->floating || width == 0 || height == 0)
        return false;
    atomic_store_explicit(&w->request_size, (uint64_t)width << 32 | height, memory_order_relaxed);
    request(w, WINDOW_REQ_RESIZE);
    return true;
}

static bool host_request_show(const clap_host_t *host)
{
    struct window *w = window_of_host(host);

    if (!w)
        return false;
    request(w, WINDOW_REQ_SHOW);
    return true;
}

static bool host_request_hide(const clap_host_t *host)
{
    struct window *w = window_of_host(host);

    if (!w)
        return false;
    request(w, WINDOW_REQ_HIDE);
    return true;
}

static void host_closed(const clap_host_t *host, bool was_destroyed)
{
    struct window *w = window_of_host(host);

    if (w)
        request(w, WINDOW_REQ_CLOSED | (was_destroyed ? WINDOW_REQ_DESTROYED : 0u));
}

static const clap_host_gui_t g_host_gui = {
    host_resize_hints_changed, host_request_resize, host_request_show, host_request_hide, host_closed
};

static xcb_connection_t *g_x;
static xcb_screen_t *g_screen;
static xcb_atom_t g_wm_protocols, g_wm_delete, g_net_wm_name, g_utf8_string;
static uint32_t g_open;             // the X windows made and not yet destroyed: the connection closes at 0
static int g_x_lost_said;

static xcb_atom_t atom(const char *name)
{
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(g_x, xcb_intern_atom(g_x, 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;

    free(r);
    return a;
}

/* the display DISPLAY names, once: -1 when it is unset or the server refuses the connection */
static int x_connect(void)
{
    const char *display = getenv("DISPLAY");
    xcb_screen_iterator_t it;
    int screen = 0;

    if (g_x)
        return 0;
    if (!display || !display[0])
        return -1;
    g_x = xcb_connect(NULL, &screen);
    if (xcb_connection_has_error(g_x))
    {
        xcb_disconnect(g_x);
        g_x = NULL;
        return -1;
    }
    for (it = xcb_setup_roots_iterator(xcb_get_setup(g_x)); it.rem && screen > 0; screen--)
        xcb_screen_next(&it);
    g_screen = it.data;
    g_wm_protocols = atom("WM_PROTOCOLS");
    g_wm_delete = atom("WM_DELETE_WINDOW");
    g_net_wm_name = atom("_NET_WM_NAME");
    g_utf8_string = atom("UTF8_STRING");
    g_x_lost_said = 0;
    return 0;
}

/* the connection closed once no window is left */
static void x_release(void)
{
    if (g_x && g_open == 0)
    {
        xcb_disconnect(g_x);
        g_x = NULL;
        g_screen = NULL;
    }
}

/* the APIs a show tries, first to last: the first the plugin offers whose display answers is taken */
static const struct window_api g_apis[] = {
    { CLAP_WINDOW_API_X11, 0, x_connect, x_release, &g_open },
    { CLAP_WINDOW_API_X11, 1, x_connect, x_release, &g_open },
};

/* WM_NORMAL_HINTS: a window the plugin cannot resize has its size as both its least and its most */
static void x_size_hints(const struct window *w)
{
    uint32_t hints[18] = { 0 };

    if (!w->resizable)
    {
        hints[0] = 16u | 32u;       // PMinSize | PMaxSize
        hints[5] = hints[7] = w->width;
        hints[6] = hints[8] = w->height;
    }
    xcb_change_property(g_x, XCB_PROP_MODE_REPLACE, w->xid, XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 32, 18, hints);
}

static void x_resize(const struct window *w)
{
    const uint32_t size[2] = { w->width, w->height };

    xcb_configure_window(g_x, w->xid, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, size);
}

/* the host's window of the plugin's size, titled, its close button asking (WM_DELETE_WINDOW), not yet mapped */
static uint32_t x_window_make(const struct window *w, const char *title)
{
    static const char wm_class[] = "omx-clap-host\0omx-clap-host";
    const uint32_t values[2] = { g_screen->black_pixel, XCB_EVENT_MASK_STRUCTURE_NOTIFY };
    const uint32_t xid = xcb_generate_id(g_x);

    xcb_create_window(g_x, XCB_COPY_FROM_PARENT, xid, g_screen->root, 0, 0, (uint16_t)w->width, (uint16_t)w->height, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, g_screen->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
    xcb_change_property(g_x, XCB_PROP_MODE_REPLACE, xid, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, (uint32_t)strlen(title), title);
    xcb_change_property(g_x, XCB_PROP_MODE_REPLACE, xid, g_net_wm_name, g_utf8_string, 8, (uint32_t)strlen(title), title);
    xcb_change_property(g_x, XCB_PROP_MODE_REPLACE, xid, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, sizeof(wm_class), wm_class);
    xcb_change_property(g_x, XCB_PROP_MODE_REPLACE, xid, g_wm_protocols, XCB_ATOM_ATOM, 32, 1, &g_wm_delete);
    return xid;
}

/* the plugin's window and the host's gone; the slot free for the next show */
static void window_drop(struct window *w, int destroy_gui)
{
    struct omx_clap_instance *in = atomic_load_explicit(&w->in, memory_order_relaxed);

    if (w->created && destroy_gui)
        w->gui->destroy(in->plugin);
    if (w->xid)
        xcb_destroy_window(g_x, w->xid);
    if (g_x)
        xcb_flush(g_x);
    atomic_store_explicit(&w->in, NULL, memory_order_release);
    w->created = 0;
    w->visible = 0;
    w->xid = 0;
    (*w->api->open)--;
    w->api->release();
}

/* the plugin's clap.gui call that answered false, on stderr: the reply carries only hosting.window.refused */
static void refused_say(const struct omx_clap_instance *in, const char *call)
{
    fprintf(stderr, "%s: clap.gui %s answered false; no window\n", in->desc->id, call);
}

static int window_unhide(struct window *w, const char **code)
{
    struct omx_clap_instance *in = atomic_load_explicit(&w->in, memory_order_relaxed);

    if (w->visible)
        return 0;
    if (w->xid)
        xcb_map_window(g_x, w->xid);
    if (!w->gui->show(in->plugin))
    {
        refused_say(in, "show");
        if (w->xid)
            xcb_unmap_window(g_x, w->xid);
        if (g_x)
            xcb_flush(g_x);
        *code = CLAP_HOST_CODE_WINDOW_REFUSED;
        return -1;
    }
    w->visible = 1;
    if (g_x)
        xcb_flush(g_x);
    return 0;
}

static void window_conceal(struct window *w)
{
    struct omx_clap_instance *in = atomic_load_explicit(&w->in, memory_order_relaxed);

    if (!w->visible)
        return;
    w->gui->hide(in->plugin);
    if (w->xid)
        xcb_unmap_window(g_x, w->xid);
    if (g_x)
        xcb_flush(g_x);
    w->visible = 0;
}

/* a free slot claimed for `in`; NULL when every slot is a window already */
static struct window *window_claim(struct omx_clap_instance *in, const clap_plugin_gui_t *gui, const struct window_api *api)
{
    uint32_t high = atomic_load_explicit(&g_high, memory_order_relaxed), i;
    struct window *w = NULL;

    for (i = 0; i < high && !w; i++)
        if (!atomic_load_explicit(&g_windows[i].in, memory_order_relaxed))
            w = &g_windows[i];
    if (!w && high < MAX_PLUGIN_INSTANCES)
        w = &g_windows[high];
    if (!w)
        return NULL;
    w->gui = gui;
    w->api = api;
    w->floating = api->floating;
    w->created = w->visible = w->resizable = 0;
    w->xid = 0;
    w->width = WINDOW_DEFAULT_WIDTH;
    w->height = WINDOW_DEFAULT_HEIGHT;
    atomic_store_explicit(&w->requests, 0u, memory_order_relaxed);
    atomic_store_explicit(&w->in, in, memory_order_release);
    if (w == &g_windows[high])
        atomic_store_explicit(&g_high, high + 1, memory_order_release);
    return w;
}

/* the first API of g_apis the plugin offers and whose display answers; NULL with *code unsupported when it offers
 * none, no-display when it offers one but no display of any it offers answers */
static const struct window_api *window_api_pick(const clap_plugin_t *plugin, const clap_plugin_gui_t *gui, const char **code)
{
    int offered = 0;

    for (size_t i = 0; i < sizeof(g_apis) / sizeof(g_apis[0]); i++)
    {
        const struct window_api *api = &g_apis[i];

        if (!gui->is_api_supported(plugin, api->name, api->floating) || (!api->floating && !gui->set_parent))
            continue;
        offered = 1;
        if (api->connect() == 0)
            return api;
    }
    *code = offered ? CLAP_HOST_CODE_WINDOW_NO_DISPLAY : CLAP_HOST_CODE_WINDOW_UNSUPPORTED;
    return NULL;
}

/* a plugin's own window, as clap.gui orders it: create, the scale, the size, the parent (or the title of a floating
 * one), show */
static int window_make(struct omx_clap_instance *in, const char *title, const char **code)
{
    const clap_plugin_t *plugin = in->plugin;
    const clap_plugin_gui_t *gui = plugin->get_extension(plugin, CLAP_EXT_GUI);
    const struct window_api *api;
    struct window *w;

    if (!gui || !gui->is_api_supported || !gui->create || !gui->destroy || !gui->show || !gui->hide)
    {
        *code = CLAP_HOST_CODE_WINDOW_UNSUPPORTED;
        return -1;
    }
    if (!(api = window_api_pick(plugin, gui, code)))
        return -1;
    w = window_claim(in, gui, api);
    if (!w || !gui->create(plugin, api->name, api->floating))
    {
        if (w)
        {
            atomic_store_explicit(&w->in, NULL, memory_order_release);
            refused_say(in, "create");
        }
        api->release();
        *code = CLAP_HOST_CODE_WINDOW_REFUSED;
        return -1;
    }
    w->created = 1;
    (*api->open)++;
    if (api->floating)
    {
        if (gui->suggest_title)
            gui->suggest_title(plugin, title);
    }
    else
    {
        clap_window_t parent;
        uint32_t width = 0, height = 0;

        if (gui->set_scale)
            gui->set_scale(plugin, 1.0);
        w->resizable = gui->can_resize && gui->can_resize(plugin);
        if (gui->get_size && gui->get_size(plugin, &width, &height) && width > 0 && height > 0)
        {
            w->width = width;
            w->height = height;
        }
        w->xid = x_window_make(w, title);
        x_size_hints(w);
        // the window exists on the server before the plugin names it as its parent over a connection of its own
        free(xcb_get_input_focus_reply(g_x, xcb_get_input_focus(g_x), NULL));
        parent.api = CLAP_WINDOW_API_X11;
        parent.x11 = w->xid;
        if (!gui->set_parent(plugin, &parent))
        {
            refused_say(in, "set_parent");
            window_drop(w, 1);
            *code = CLAP_HOST_CODE_WINDOW_REFUSED;
            return -1;
        }
    }
    if (window_unhide(w, code) != 0)
    {
        window_drop(w, 1);
        return -1;
    }
    return 0;
}

/* the host's window resized by its user (or its window manager): the plugin is asked to take the size, as near as it
 * can, or the window goes back to the plugin's own size */
static void window_configured(struct window *w, uint32_t width, uint32_t height)
{
    struct omx_clap_instance *in = atomic_load_explicit(&w->in, memory_order_relaxed);
    uint32_t cw = width, ch = height;

    if (!w->created || (width == w->width && height == w->height))
        return;
    if (w->resizable && w->gui->set_size)
    {
        if (w->gui->adjust_size)
            w->gui->adjust_size(in->plugin, &cw, &ch);
        if (cw > 0 && ch > 0 && w->gui->set_size(in->plugin, cw, ch))
        {
            w->width = cw;
            w->height = ch;
        }
    }
    if (w->width != width || w->height != height)
        x_resize(w);
}

static struct window *window_of_xid(uint32_t xid)
{
    const uint32_t high = atomic_load_explicit(&g_high, memory_order_relaxed);

    for (uint32_t i = 0; i < high; i++)
        if (atomic_load_explicit(&g_windows[i].in, memory_order_relaxed) && g_windows[i].xid == xid)
            return &g_windows[i];
    return NULL;
}

static void x_events(void)
{
    xcb_generic_event_t *e;

    while ((e = xcb_poll_for_event(g_x)))
    {
        const uint8_t type = e->response_type & 0x7f;

        if (type == XCB_CONFIGURE_NOTIFY)
        {
            const xcb_configure_notify_event_t *c = (const xcb_configure_notify_event_t *)e;
            struct window *w = window_of_xid(c->window);

            if (w && c->window == c->event)
                window_configured(w, c->width, c->height);
        }
        else if (type == XCB_CLIENT_MESSAGE)
        {
            const xcb_client_message_event_t *m = (const xcb_client_message_event_t *)e;
            struct window *w = window_of_xid(m->window);

            if (w && m->type == g_wm_protocols && m->data.data32[0] == g_wm_delete)
                window_conceal(w);
        }
        free(e);
    }
    if (xcb_connection_has_error(g_x) && !g_x_lost_said)
    {
        fprintf(stderr, "window: the X connection is lost; the plugins run on\n");
        g_x_lost_said = 1;
    }
}

/* what the plugins asked since the last tick */
static void window_requests(struct window *w)
{
    const uint32_t req = atomic_exchange_explicit(&w->requests, 0u, memory_order_acquire);
    struct omx_clap_instance *in = atomic_load_explicit(&w->in, memory_order_relaxed);
    const char *code;

    if (!req || !w->created)
        return;
    if (req & (WINDOW_REQ_CLOSED | WINDOW_REQ_DESTROYED))
    {
        // a floating window its user closed: hidden, or gone with the plugin's window when was_destroyed
        if (req & WINDOW_REQ_DESTROYED)
        {
            window_drop(w, 0);
            return;
        }
        if (w->xid)
            xcb_unmap_window(g_x, w->xid);
        w->visible = 0;
    }
    if (req & WINDOW_REQ_HINTS)
    {
        w->resizable = w->gui->can_resize && w->gui->can_resize(in->plugin);
        if (w->xid)
            x_size_hints(w);
    }
    if ((req & WINDOW_REQ_RESIZE) && w->xid)
    {
        const uint64_t size = atomic_load_explicit(&w->request_size, memory_order_relaxed);

        w->width = (uint32_t)(size >> 32);
        w->height = (uint32_t)size;
        if (!w->resizable)
            x_size_hints(w);
        x_resize(w);
    }
    if (req & WINDOW_REQ_SHOW)
        (void)window_unhide(w, &code);
    if (req & WINDOW_REQ_HIDE)
        window_conceal(w);
}
#endif

void window_init(void)
{
#ifdef OMX_CLAP_HOST_X11
    omx_clap_host_offer_gui(&g_host_gui);
#endif
}

int window_show(struct omx_clap_instance *in, const char *title, const char **code)
{
#ifdef OMX_CLAP_HOST_X11
    struct window *w = window_of(in);

    if (w && w->created)
        return window_unhide(w, code);
    return window_make(in, title, code);
#else
    (void)in;
    (void)title;
    *code = CLAP_HOST_CODE_WINDOW_NOT_BUILT;
    return -1;
#endif
}

int window_hide(struct omx_clap_instance *in)
{
#ifdef OMX_CLAP_HOST_X11
    struct window *w = window_of(in);

    if (w && w->created)
        window_conceal(w);
#else
    (void)in;
#endif
    return 0;
}

void window_close(struct omx_clap_instance *in)
{
#ifdef OMX_CLAP_HOST_X11
    struct window *w = window_of(in);

    if (w && w->created)
        window_drop(w, 1);
#else
    (void)in;
#endif
}

void window_idle(void)
{
#ifdef OMX_CLAP_HOST_X11
    const uint32_t high = atomic_load_explicit(&g_high, memory_order_relaxed);

    if (!g_x)
        return;
    x_events();
    for (uint32_t i = 0; i < high && g_x; i++)
        if (atomic_load_explicit(&g_windows[i].in, memory_order_relaxed))
            window_requests(&g_windows[i]);
    if (g_x)
        xcb_flush(g_x);
#endif
}

void window_finish(void)
{
#ifdef OMX_CLAP_HOST_X11
    const uint32_t high = atomic_load_explicit(&g_high, memory_order_relaxed);

    for (uint32_t i = 0; i < high; i++)
        if (atomic_load_explicit(&g_windows[i].in, memory_order_relaxed) && g_windows[i].created)
            window_drop(&g_windows[i], 1);
#endif
}
