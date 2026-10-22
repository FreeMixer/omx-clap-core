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

/* One signal into up to six effects, every output recorded from the same cycles:
 *   jack_identity <frames> <preroll_blocks> <source.f32> <a.f32> [<b.f32> ... [<f.f32>]]
 * identity-src:out plays deterministic noise from the cycle after "go" on
 * stdin; identity-rec:a, :b ... :f (one per file given) record <frames>
 * frames starting <preroll_blocks> cycles later, and the source frames of that
 * same window go to <source.f32>. Raw float32, one channel each. Prints the
 * rate, the quantum and the peak of each recording, then exits. */

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <jack/jack.h>

#define RECORDERS_MAX 6

static jack_port_t *g_out, *g_in[RECORDERS_MAX];
static float *g_source, *g_rec[RECORDERS_MAX];
static int g_recorders;
static long g_frames, g_preroll_frames;
static long g_played, g_recorded;
static _Atomic int g_go, g_done;
static unsigned g_lcg = 12345u;

static float next_sample(void)
{
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return ((float)(g_lcg >> 8) / 16777216.0f * 2.0f - 1.0f) * 0.25f;
}

static int src_process(jack_nframes_t nframes, void *arg)
{
    float *out = jack_port_get_buffer(g_out, nframes);
    jack_nframes_t i;

    (void)arg;
    for (i = 0; i < nframes; i++)
    {
        float s = 0.0f;
        if (atomic_load(&g_go) && g_played < g_preroll_frames + g_frames)
        {
            s = next_sample();
            if (g_played >= g_preroll_frames)
                g_source[g_played - g_preroll_frames] = s;
            g_played++;
        }
        out[i] = s;
    }
    return 0;
}

static int rec_process(jack_nframes_t nframes, void *arg)
{
    const float *in[RECORDERS_MAX];
    jack_nframes_t i;
    int r;

    (void)arg;
    if (!atomic_load(&g_go))
        return 0;
    for (r = 0; r < g_recorders; r++)
        in[r] = jack_port_get_buffer(g_in[r], nframes);
    for (i = 0; i < nframes && g_recorded < g_preroll_frames + g_frames; i++, g_recorded++)
    {
        if (g_recorded >= g_preroll_frames)
            for (r = 0; r < g_recorders; r++)
                g_rec[r][g_recorded - g_preroll_frames] = in[r][i];
    }
    if (g_recorded >= g_preroll_frames + g_frames)
        atomic_store(&g_done, 1);
    return 0;
}

static float peak(const float *buffer, long n)
{
    float p = 0.0f;
    long i;

    for (i = 0; i < n; i++)
        if (fabsf(buffer[i]) > p)
            p = fabsf(buffer[i]);
    return p;
}

static int write_file(const char *path, const float *buffer, long n)
{
    FILE *f = fopen(path, "wb");

    if (!f)
        return -1;
    if (fwrite(buffer, sizeof(float), (size_t)n, f) != (size_t)n)
    {
        fclose(f);
        return -1;
    }
    return fclose(f);
}

int main(int argc, char **argv)
{
    jack_client_t *src, *rec;
    char line[64];
    unsigned waited = 0;
    int r;

    if (argc < 5 || argc > 4 + RECORDERS_MAX)
    {
        fprintf(stderr, "usage: %s <frames> <preroll_blocks> <source.f32> <a.f32> [<b.f32> ... [<f.f32>]]\n", argv[0]);
        return 2;
    }
    g_recorders = argc - 4;
    g_frames = atol(argv[1]);
    g_source = calloc((size_t)g_frames, sizeof(float));
    for (r = 0; r < g_recorders; r++)
        g_rec[r] = calloc((size_t)g_frames, sizeof(float));

    src = jack_client_open("identity-src", JackNoStartServer, NULL);
    rec = jack_client_open("identity-rec", JackNoStartServer, NULL);
    if (!src || !rec)
    {
        fprintf(stderr, "can't get jack client\n");
        return 2;
    }
    g_preroll_frames = atol(argv[2]) * (long)jack_get_buffer_size(src);
    g_out = jack_port_register(src, "out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    for (r = 0; r < g_recorders; r++)
    {
        const char name[2] = {(char)('a' + r), '\0'};

        g_in[r] = jack_port_register(rec, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    }
    jack_set_process_callback(src, src_process, NULL);
    jack_set_process_callback(rec, rec_process, NULL);
    if (jack_activate(src) != 0 || jack_activate(rec) != 0)
    {
        fprintf(stderr, "can't activate\n");
        return 2;
    }
    printf("ready rate=%u quantum=%u\n", jack_get_sample_rate(src), jack_get_buffer_size(src));
    fflush(stdout);

    while (fgets(line, sizeof(line), stdin))
        if (strncmp(line, "go", 2) == 0)
            break;
    atomic_store(&g_go, 1);

    while (!atomic_load(&g_done) && waited < 30000)
    {
        usleep(1000);
        waited++;
    }
    jack_deactivate(rec);
    jack_deactivate(src);
    jack_client_close(rec);
    jack_client_close(src);

    if (!atomic_load(&g_done))
    {
        fprintf(stderr, "recorded %ld of %ld frames: no cycles\n", g_recorded, g_preroll_frames + g_frames);
        return 1;
    }
    if (write_file(argv[3], g_source, g_frames) != 0)
    {
        fprintf(stderr, "can't write the recordings\n");
        return 1;
    }
    for (r = 0; r < g_recorders; r++)
        if (write_file(argv[4 + r], g_rec[r], g_frames) != 0)
        {
            fprintf(stderr, "can't write the recordings\n");
            return 1;
        }
    printf("done frames=%ld peak_source=%g", g_frames, (double)peak(g_source, g_frames));
    for (r = 0; r < g_recorders; r++)
        printf(" peak_%c=%g", 'a' + r, (double)peak(g_rec[r], g_frames));
    printf("\n");
    return 0;
}
