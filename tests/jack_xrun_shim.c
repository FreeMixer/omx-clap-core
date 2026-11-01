// SPDX-License-Identifier: GPL-3.0-or-later
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

/* A test double for what JACK does and a private PipeWire cannot be made to do (#371): preloaded under the host, it
 * calls the xrun callbacks the host registered, on a thread of its own as JACK does, once for every byte written to the
 * FIFO named by JACK_XRUN_SHIM_FIFO: every callback of every client that is active then, as JACK tells every active
 * client of every xrun. PipeWire's libjack calls that callback only when the driver reports an xrun, and the dummy
 * driver of a PipeWire with no sound card never does. Everything else goes to the real libjack, and the shim writes to
 * JACK_XRUN_SHIM_LOG a line for each xrun callback registered and each client activated, deactivated and closed, in the
 * order they happen, so a test can hold that the callback is registered before its client is active and that the host
 * always has an active client:
 *   set_xrun <client>
 *   activate <client>
 *   deactivate <client>
 *   close <client>
 *   fire <callbacks called> */

#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <jack/jack.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void say(const char *what, jack_client_t *client, int n)
{
    const char *path = getenv("JACK_XRUN_SHIM_LOG");
    FILE *f = path ? fopen(path, "a") : NULL;

    if (!f)
        return;
    if (client)
        fprintf(f, "%s %p\n", what, (void *)client);
    else
        fprintf(f, "%s %d\n", what, n);
    fclose(f);
}

#define CALLBACKS_MAX 256
static struct callback
{
    jack_client_t *client;
    JackXRunCallback function;
    void *arg;
    int active;
} g_callbacks[CALLBACKS_MAX];
static int g_ncallbacks, g_thread_started;

static struct callback *find(jack_client_t *client)
{
    int k;

    for (k = 0; k < g_ncallbacks; k++)
        if (g_callbacks[k].client == client)
            return &g_callbacks[k];
    return NULL;
}

static void *fire_thread(void *data)
{
    const char *path = getenv("JACK_XRUN_SHIM_FIFO");
    int fd = path ? open(path, O_RDWR) : -1, k, n;
    char byte;

    (void)data;
    while (fd >= 0 && read(fd, &byte, 1) == 1)
    {
        pthread_mutex_lock(&g_lock);
        for (k = n = 0; k < g_ncallbacks; k++)
            if (g_callbacks[k].active && g_callbacks[k].function)
            {
                g_callbacks[k].function(g_callbacks[k].arg);
                n++;
            }
        say("fire", NULL, n);
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

__attribute__((visibility("default"))) int jack_set_xrun_callback(jack_client_t *client, JackXRunCallback function, void *arg)
{
    int (*real)(jack_client_t *, JackXRunCallback, void *) = dlsym(RTLD_NEXT, "jack_set_xrun_callback");
    struct callback *c;
    pthread_t thread;

    pthread_mutex_lock(&g_lock);
    say("set_xrun", client, 0);
    if (!(c = find(client)) && g_ncallbacks < CALLBACKS_MAX)
    {
        c = &g_callbacks[g_ncallbacks++];
        c->client = client;
        c->active = 0;
    }
    if (c)
    {
        c->function = function;
        c->arg = arg;
    }
    if (!g_thread_started && pthread_create(&thread, NULL, fire_thread, NULL) == 0)
    {
        pthread_detach(thread);
        g_thread_started = 1;
    }
    pthread_mutex_unlock(&g_lock);
    return real(client, function, arg);
}

__attribute__((visibility("default"))) int jack_activate(jack_client_t *client)
{
    int (*real)(jack_client_t *) = dlsym(RTLD_NEXT, "jack_activate");
    int ret;
    struct callback *c;

    pthread_mutex_lock(&g_lock);
    say("activate", client, 0);
    pthread_mutex_unlock(&g_lock);
    ret = real(client);
    pthread_mutex_lock(&g_lock);
    if (ret == 0 && (c = find(client)))
        c->active = 1;
    pthread_mutex_unlock(&g_lock);
    return ret;
}

__attribute__((visibility("default"))) int jack_deactivate(jack_client_t *client)
{
    int (*real)(jack_client_t *) = dlsym(RTLD_NEXT, "jack_deactivate");
    struct callback *c;

    pthread_mutex_lock(&g_lock);
    say("deactivate", client, 0);
    if ((c = find(client)))
        c->active = 0;
    pthread_mutex_unlock(&g_lock);
    return real(client);
}

__attribute__((visibility("default"))) int jack_client_close(jack_client_t *client)
{
    int (*real)(jack_client_t *) = dlsym(RTLD_NEXT, "jack_client_close");
    struct callback *c;

    pthread_mutex_lock(&g_lock);
    say("close", client, 0);
    if ((c = find(client)))
    {
        *c = g_callbacks[--g_ncallbacks];
    }
    pthread_mutex_unlock(&g_lock);
    return real(client);
}
