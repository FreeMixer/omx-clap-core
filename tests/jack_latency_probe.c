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

/* Print the latency range another client published on one of its ports,
 * as a graph reads it: jack_latency_probe <port> capture|playback */

#include <stdio.h>
#include <string.h>
#include <jack/jack.h>

int main(int argc, char **argv)
{
    jack_client_t *client;
    jack_port_t *port;
    jack_latency_range_t range;
    jack_latency_callback_mode_t mode;

    if (argc != 3 || (strcmp(argv[2], "capture") && strcmp(argv[2], "playback")))
    {
        fprintf(stderr, "usage: %s <port> capture|playback\n", argv[0]);
        return 2;
    }
    mode = strcmp(argv[2], "capture") == 0 ? JackCaptureLatency : JackPlaybackLatency;

    client = jack_client_open("jack_latency_probe", JackNoStartServer, NULL);
    if (!client)
    {
        fprintf(stderr, "can't get jack client\n");
        return 2;
    }
    port = jack_port_by_name(client, argv[1]);
    if (!port)
    {
        fprintf(stderr, "no port %s\n", argv[1]);
        jack_client_close(client);
        return 1;
    }
    jack_port_get_latency_range(port, mode, &range);
    printf("%u %u\n", range.min, range.max);
    jack_client_close(client);
    return 0;
}
