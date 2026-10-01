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


/* Play a note into an instrument over jack and print how loud it came out:
 *   jack_synth_probe <midi_in port> <audio out port>...
 * A client with one midi output and one audio input per audio port given,
 * linked to the ports named. It measures the linked ports' signal over half
 * a second at a time: before any note, with a note on at velocity 100, with
 * one on at velocity 50, and after the note off. One line, the RMS of each
 * window over all the ports, then the peak of the first note's:
 *   <before> <on_100> <on_50> <after> <peak_100> */

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <jack/jack.h>
#include <jack/midiport.h>

#define MAX_AUDIO_PORTS 4
#define KEY             69

static jack_client_t *g_client;
static jack_port_t *g_midi_out;
static jack_port_t *g_audio_in[MAX_AUDIO_PORTS];
static int g_audio_count;

/* the control thread names one message, the next cycle sends it */
static _Atomic int g_send_status;
static _Atomic int g_send_velocity;

static _Atomic double g_sum;
static _Atomic double g_peak;
static _Atomic uint64_t g_count;

static int process(jack_nframes_t nframes, void *arg)
{
    void *midi = jack_port_get_buffer(g_midi_out, nframes);
    const int status = atomic_exchange(&g_send_status, 0);
    double sum = 0.0, peak = 0.0;
    int p;
    jack_nframes_t i;

    (void)arg;
    jack_midi_clear_buffer(midi);
    if (status)
    {
        unsigned char *data = jack_midi_event_reserve(midi, nframes / 2, 3);
        if (data)
        {
            data[0] = (unsigned char)status;
            data[1] = KEY;
            data[2] = (unsigned char)atomic_load(&g_send_velocity);
        }
    }
    for (p = 0; p < g_audio_count; p++)
    {
        const float *in = jack_port_get_buffer(g_audio_in[p], nframes);
        for (i = 0; i < nframes; i++)
        {
            sum += (double)in[i] * (double)in[i];
            if (fabs((double)in[i]) > peak)
                peak = fabs((double)in[i]);
        }
    }
    atomic_store(&g_sum, atomic_load(&g_sum) + sum);
    if (peak > atomic_load(&g_peak))
        atomic_store(&g_peak, peak);
    atomic_fetch_add(&g_count, (uint64_t)nframes * (uint64_t)g_audio_count);
    return 0;
}

static void window(double *rms, double *peak)
{
    atomic_store(&g_sum, 0.0);
    atomic_store(&g_peak, 0.0);
    atomic_store(&g_count, 0);
    usleep(500000);
    *rms = sqrt(atomic_load(&g_sum) / (double)(atomic_load(&g_count) ? atomic_load(&g_count) : 1));
    *peak = atomic_load(&g_peak);
}

static void send(int status, int velocity)
{
    atomic_store(&g_send_velocity, velocity);
    atomic_store(&g_send_status, status);
    usleep(200000);
}

int main(int argc, char **argv)
{
    double before, on100, on50, after, peak, ignored;
    char name[64];
    int i;

    if (argc < 3 || argc - 2 > MAX_AUDIO_PORTS)
    {
        fprintf(stderr, "usage: %s <midi_in port> <audio out port>...\n", argv[0]);
        return 2;
    }
    g_client = jack_client_open("jack_synth_probe", JackNoStartServer, NULL);
    if (!g_client)
    {
        fprintf(stderr, "can't get jack client\n");
        return 2;
    }
    g_midi_out = jack_port_register(g_client, "midi_out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
    for (i = 0; i < argc - 2; i++)
    {
        snprintf(name, sizeof(name), "in_%i", i + 1);
        g_audio_in[i] = jack_port_register(g_client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    }
    g_audio_count = argc - 2;
    jack_set_process_callback(g_client, process, NULL);
    if (jack_activate(g_client) != 0)
    {
        fprintf(stderr, "can't activate\n");
        return 2;
    }
    if (jack_connect(g_client, "jack_synth_probe:midi_out", argv[1]) != 0)
    {
        fprintf(stderr, "can't connect to %s\n", argv[1]);
        return 1;
    }
    for (i = 0; i < g_audio_count; i++)
    {
        snprintf(name, sizeof(name), "jack_synth_probe:in_%i", i + 1);
        if (jack_connect(g_client, argv[i + 2], name) != 0)
        {
            fprintf(stderr, "can't connect %s\n", argv[i + 2]);
            return 1;
        }
    }

    usleep(500000);
    window(&before, &ignored);
    send(0x90, 100);
    window(&on100, &peak);
    send(0x90, 50);
    window(&on50, &ignored);
    send(0x80, 64);
    window(&after, &ignored);
    printf("%.6f %.6f %.6f %.6f %.6f\n", before, on100, on50, after, peak);
    jack_client_close(g_client);
    return 0;
}
