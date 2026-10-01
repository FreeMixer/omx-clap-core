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

/* A constant input for the meters, until it is terminated: 0.5 on the left and 0.25 on the right, -6.0206 and
 * -12.0412 dBFS, into every pair of ports named, left then right:
 *   jack_meter_source <left port> <right port> [<left port> <right port> ...]
 * It prints "playing" once every connection is made. */

#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <jack/jack.h>

#define LEFT    0.5f
#define RIGHT   0.25f

static jack_port_t *g_ports[2];
static volatile sig_atomic_t g_running = 1;

static int process(jack_nframes_t nframes, void *arg)
{
    jack_nframes_t i;
    int c;

    (void)arg;
    for (c = 0; c < 2; c++)
    {
        float *out = jack_port_get_buffer(g_ports[c], nframes);

        for (i = 0; i < nframes; i++)
            out[i] = c == 0 ? LEFT : RIGHT;
    }
    return 0;
}

static void stop(int sig)
{
    g_running = 0;
    (void)sig;
}

int main(int argc, char **argv)
{
    jack_client_t *client;
    int i;

    if (argc < 3 || (argc - 1) % 2)
    {
        fprintf(stderr, "usage: %s <left port> <right port> [<left port> <right port> ...]\n", argv[0]);
        return 2;
    }
    client = jack_client_open("jack_meter_source", JackNoStartServer, NULL);
    if (!client)
    {
        fprintf(stderr, "can't get jack client\n");
        return 2;
    }
    g_ports[0] = jack_port_register(client, "out_l", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    g_ports[1] = jack_port_register(client, "out_r", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    if (!g_ports[0] || !g_ports[1] || jack_set_process_callback(client, process, NULL) != 0 || jack_activate(client) != 0)
    {
        fprintf(stderr, "can't start the source\n");
        jack_client_close(client);
        return 2;
    }
    for (i = 1; i < argc; i++)
    {
        if (jack_connect(client, jack_port_name(g_ports[(i - 1) % 2]), argv[i]) != 0)
        {
            fprintf(stderr, "can't connect to %s\n", argv[i]);
            jack_client_close(client);
            return 1;
        }
    }
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    printf("playing\n");
    fflush(stdout);
    while (g_running)
        usleep(10000);
    jack_client_close(client);
    return 0;
}
