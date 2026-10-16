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


/* Play one note into a plugin's midi_in over jack and read what comes out of its midi_out, each message at its frame:
 *   jack_notes_out_probe <midi_in port> <midi_out port>
 * Two clients, so the graph orders them around the plugin and all three run in one cycle: the sender, linked to
 * midi_in, and the reader, linked from midi_out. The sender writes a note on (0x90, key 60, velocity 100) at frame 10
 * of a cycle, then, half a second later, its note off (0x80, key 60, velocity 0) at frame 20. Every message the reader
 * gets is printed with its frame counted from the frame of the note on, the frame of the cycle plus its offset in it:
 *   rate <sample rate>
 *   off <frame of the note off>
 *   <frame> <byte> <byte> <byte>
 *   ... */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <jack/jack.h>
#include <jack/midiport.h>

#define KEY             60
#define SENT_MAX        64
#define ON_OFFSET       10u
#define OFF_OFFSET      20u

static jack_client_t *g_sender, *g_reader;
static jack_port_t *g_out, *g_in;

/* the control thread names one message, the sender's next cycle writes it and says at which frame */
static _Atomic int g_send_status;
static _Atomic int64_t g_sent_frame = -1;

/* what the reader got, written by its cycle only */
static uint8_t g_got[SENT_MAX][3];
static int64_t g_got_frame[SENT_MAX];
static _Atomic int g_got_count;

static int send_process(jack_nframes_t nframes, void *arg)
{
    void *midi = jack_port_get_buffer(g_out, nframes);
    const int status = atomic_exchange(&g_send_status, 0);

    (void)arg;
    jack_midi_clear_buffer(midi);
    if (status)
    {
        const jack_nframes_t at = status == 0x90 ? ON_OFFSET : OFF_OFFSET;
        const unsigned char data[3] = { (unsigned char)status, KEY, status == 0x90 ? 100 : 0 };

        if (at < nframes && jack_midi_event_write(midi, at, data, 3) == 0)
            atomic_store(&g_sent_frame, (int64_t)jack_last_frame_time(g_sender) + at);
    }
    return 0;
}

static int read_process(jack_nframes_t nframes, void *arg)
{
    void *midi = jack_port_get_buffer(g_in, nframes);
    const jack_nframes_t count = jack_midi_get_event_count(midi);
    const int64_t cycle = (int64_t)jack_last_frame_time(g_reader);
    jack_midi_event_t event;
    jack_nframes_t e;

    (void)arg;
    for (e = 0; e < count; e++)
    {
        const int n = atomic_load(&g_got_count);

        if (n >= SENT_MAX || jack_midi_event_get(&event, midi, e) != 0)
            continue;
        memset(g_got[n], 0, 3);
        memcpy(g_got[n], event.buffer, event.size < 3 ? event.size : 3);
        g_got_frame[n] = cycle + event.time;
        atomic_store(&g_got_count, n + 1);
    }
    return 0;
}

static int64_t send(int status)
{
    int i;

    atomic_store(&g_sent_frame, -1);
    atomic_store(&g_send_status, status);
    for (i = 0; i < 100 && atomic_load(&g_sent_frame) < 0; i++)
        usleep(10000);
    usleep(500000);
    return atomic_load(&g_sent_frame);
}

int main(int argc, char **argv)
{
    int64_t on, off;
    int i;

    if (argc != 3)
    {
        fprintf(stderr, "usage: %s <midi_in port> <midi_out port>\n", argv[0]);
        return 2;
    }
    g_sender = jack_client_open("notes_out_sender", JackNoStartServer, NULL);
    g_reader = jack_client_open("notes_out_reader", JackNoStartServer, NULL);
    if (!g_sender || !g_reader)
    {
        fprintf(stderr, "can't get jack clients\n");
        return 2;
    }
    g_out = jack_port_register(g_sender, "out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
    g_in = jack_port_register(g_reader, "in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
    jack_set_process_callback(g_sender, send_process, NULL);
    jack_set_process_callback(g_reader, read_process, NULL);
    if (jack_activate(g_sender) != 0 || jack_activate(g_reader) != 0)
    {
        fprintf(stderr, "can't activate\n");
        return 2;
    }
    if (jack_connect(g_sender, "notes_out_sender:out", argv[1]) != 0 || jack_connect(g_reader, argv[2], "notes_out_reader:in") != 0)
    {
        fprintf(stderr, "can't connect %s and %s\n", argv[1], argv[2]);
        return 1;
    }
    usleep(500000);
    on = send(0x90);
    off = send(0x80);
    if (on < 0 || off < 0)
    {
        fprintf(stderr, "a note was not sent\n");
        return 1;
    }
    printf("rate %u\n", (unsigned)jack_get_sample_rate(g_sender));
    printf("off %lld\n", (long long)(off - on));
    for (i = 0; i < atomic_load(&g_got_count); i++)
        printf("%lld %02x %02x %02x\n", (long long)(g_got_frame[i] - on), g_got[i][0], g_got[i][1], g_got[i][2]);
    jack_client_close(g_reader);
    jack_client_close(g_sender);
    return 0;
}
