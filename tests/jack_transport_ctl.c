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


/* Move the JACK transport and say where it is, for the tests:
 *   jack_transport_ctl start | stop | locate <frame> | query
 * Each prints one line once the command is done: "<state> <frame> <rate>", the state 0 stopped, 1 rolling, 2
 * starting. start and stop wait until the transport says it rolls or stands; locate waits until it reads the frame. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <jack/jack.h>
#include <jack/transport.h>

static int state_of(jack_transport_state_t s)
{
    return s == JackTransportRolling ? 1 : (s == JackTransportStarting ? 2 : 0);
}

int main(int argc, char **argv)
{
    jack_client_t *client;
    jack_position_t pos;
    jack_transport_state_t s;
    int i;

    if (argc < 2)
    {
        fprintf(stderr, "usage: %s start | stop | locate <frame> | query\n", argv[0]);
        return 2;
    }
    client = jack_client_open("jack_transport_ctl", JackNoStartServer, NULL);
    if (!client || jack_activate(client) != 0)
    {
        fprintf(stderr, "can't get an active jack client\n");
        return 2;
    }
    usleep(200000);     // a few cycles first: a client reads the transport its cycles have seen
    if (!strcmp(argv[1], "start"))
        jack_transport_start(client);
    else if (!strcmp(argv[1], "stop"))
        jack_transport_stop(client);
    else if (!strcmp(argv[1], "locate") && argc > 2)
        jack_transport_locate(client, (jack_nframes_t)strtoul(argv[2], NULL, 10));
    else if (strcmp(argv[1], "query") != 0)
    {
        fprintf(stderr, "unknown command %s\n", argv[1]);
        return 2;
    }
    for (i = 0; i < 200; i++)
    {
        s = jack_transport_query(client, &pos);
        if ((!strcmp(argv[1], "start") && s == JackTransportRolling) || (!strcmp(argv[1], "stop") && s == JackTransportStopped) ||
            (!strcmp(argv[1], "locate") && pos.frame == (jack_nframes_t)strtoul(argv[2], NULL, 10)) || !strcmp(argv[1], "query"))
            break;
        usleep(10000);
    }
    s = jack_transport_query(client, &pos);
    printf("%d %u %u\n", state_of(s), pos.frame, jack_get_sample_rate(client));
    jack_client_close(client);
    return i == 200 ? 1 : 0;
}
