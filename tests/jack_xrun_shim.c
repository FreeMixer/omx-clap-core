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
 * calls the xrun callback the host registered, on a thread of its own as JACK does, once for every byte written to the
 * FIFO named by JACK_XRUN_SHIM_FIFO. PipeWire's libjack calls that callback only when the driver reports an xrun, and
 * the dummy driver of a PipeWire with no sound card never does. Everything else goes to the real libjack, and the shim
 * writes to JACK_XRUN_SHIM_LOG a line for each xrun callback registered and each client activated, in the order they
 * happen, so a test can hold that the callback is registered before its client is active.
 *   set_xrun <client>
 *   activate <client>
 *   fire */

#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <jack/jack.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void say(const char *what, jack_client_t *client)
{
    const char *path = getenv("JACK_XRUN_SHIM_LOG");
    FILE *f = path ? fopen(path, "a") : NULL;

    if (!f)
        return;
    pthread_mutex_lock(&g_lock);
    if (client)
        fprintf(f, "%s %p\n", what, (void *)client);
    else
        fprintf(f, "%s\n", what);
    fclose(f);
    pthread_mutex_unlock(&g_lock);
}

struct callback
{
    JackXRunCallback function;
    void *arg;
};

static void *fire_thread(void *data)
{
    struct callback *callback = data;
    const char *path = getenv("JACK_XRUN_SHIM_FIFO");
    int fd = path ? open(path, O_RDWR) : -1;
    char byte;

    while (fd >= 0 && read(fd, &byte, 1) == 1)
    {
        say("fire", NULL);
        callback->function(callback->arg);
    }
    return NULL;
}

__attribute__((visibility("default"))) int jack_set_xrun_callback(jack_client_t *client, JackXRunCallback function, void *arg)
{
    int (*real)(jack_client_t *, JackXRunCallback, void *) = dlsym(RTLD_NEXT, "jack_set_xrun_callback");
    struct callback *callback = malloc(sizeof(*callback));
    pthread_t thread;

    say("set_xrun", client);
    callback->function = function;
    callback->arg = arg;
    if (function && pthread_create(&thread, NULL, fire_thread, callback) == 0)
        pthread_detach(thread);
    return real(client, function, arg);
}

__attribute__((visibility("default"))) int jack_activate(jack_client_t *client)
{
    int (*real)(jack_client_t *) = dlsym(RTLD_NEXT, "jack_activate");

    say("activate", client);
    return real(client);
}
