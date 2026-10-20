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

/* The feeder, recorder and clock of tests/lv2_parity.py: jack_identity's
 * method for many arms, two channels and MIDI, as fast as the graph runs.
 *
 *   jack_parity [-m] <frames> <outdir> <client>...
 *
 * parity-src:out_1 and :out_2 play the signal, :midi_out the note sequence (-m);
 * parity-rec records up to two audio outputs of every <client> (an effect_<N>
 * of mod-host or omx-clap-host), from the same cycles. The feeder links each
 * client itself: its audio inputs from out_1 and out_2 in the order the graph
 * lists them (a mono input takes out_1), its first MIDI input from midi_out,
 * its first two audio outputs to its recorder ports.
 *
 * parity-clock is the graph's only driver (the private PipeWire runs with
 * neither its dummy nor its freewheel driver): a PipeWire filter with
 * PW_FILTER_FLAG_DRIVER that runs one cycle per trigger; the next trigger waits
 * for parity-rec, the last node of every chain, to have run in the cycle. Its
 * port feeds parity-rec:clock, which makes it stream. Nothing runs before the first trigger, so every plugin of
 * every host gets its first run() in the same cycle, however far apart the
 * hosts added them. On stdin, "warm <blocks>" runs that many cycles of silence
 * and prints "warm blocks=<total>": the caller can then wait in wall time (the
 * adapter's latency hold, a worker's answer) with every instance on the same
 * cycle count. "go" plays <frames> frames back to back and records them; "go
 * paced" plays them one quantum of wall time apart, as a sound card would.
 * The rate and the quantum are the PipeWire's; the caller checks them on the
 * "ready" line.
 *
 * The signal, per channel c (0 or 1), at frame n of rate R, in the first 60 %
 * of the frames: noise (jack_identity's generator, its own seed per channel)
 * at 0.15, a sine of 441 * (c + 1.5) Hz at 0.2, and an impulse of 0.5 at the
 * start of every second. The last 40 % is silence, the tail. The MIDI: a note
 * every 250 ms from a fixed list, velocity 100, released 200 ms later, over the
 * same 60 %, and all notes off at its end.
 *
 * Writes <outdir>/source.<c>.f32 and <outdir>/<client>.<c>.f32, raw float32,
 * one channel each. Prints per client "linked <client> in=<k> out=<k> midi=<0|1>
 * midi_fed=<0|1> latency=<min>,<max>" (midi: it has a MIDI input; midi_fed:
 * the notes reach it; the capture latency of its first output), after each
 * warm "warm blocks=<n> latency=<client>:<min>,<max>;..." (the same latencies,
 * read again once the graph has run), and after the run "done frames=<n>
 * cycles=<n> quantum=<q> wall_ms=<ms>". */

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <semaphore.h>
#include <jack/jack.h>
#include <jack/midiport.h>
#include <pipewire/pipewire.h>
#include <pipewire/filter.h>

#define CLIENTS_MAX 8
#define CHANNELS 2
#define PORTS_MAX 64
#define QUANTUM_HINT 128

static jack_client_t *g_src, *g_rec;
static jack_port_t *g_out[CHANNELS], *g_midi, *g_clock_in;
static jack_port_t *g_in[CLIENTS_MAX][CHANNELS];
static int g_outs[CLIENTS_MAX];
static float *g_source[CHANNELS], *g_buf[CLIENTS_MAX][CHANNELS];
static int g_clients, g_send_midi;
static long g_frames, g_warm_frames;
static long g_played, g_recorded, g_warmed;
static long g_cycles;
static _Atomic jack_nframes_t g_quantum_seen;
static _Atomic int g_go, g_done, g_warm_done, g_odd_quantum;
static unsigned g_lcg[CHANNELS] = {12345u, 54321u};
static jack_nframes_t g_rate;
static _Atomic long g_seq, g_clock_seen, g_rec_seen;
/* posted by the recorder, the last node of every chain: the cycle the clock triggered is complete */
static sem_t g_cycle_done, g_clock_called;

static const int g_notes[] = {60, 64, 67, 72, 48, 55, 62, 69};

static float noise(int c)
{
    g_lcg[c] = g_lcg[c] * 1664525u + 1013904223u;
    return ((float)(g_lcg[c] >> 8) / 16777216.0f * 2.0f - 1.0f) * 0.15f;
}

static float signal_at(int c, long n)
{
    double t;
    float s;

    if (n >= g_frames * 6 / 10)
        return 0.0f;
    t = (double)n / (double)g_rate;
    s = noise(c) + 0.2f * (float)sin(2.0 * M_PI * 441.0 * (c + 1.5) * t);
    if (n % (long)g_rate == 0)
        s += 0.5f;
    return s;
}

/* the MIDI of frames [from, from + n) into the port buffer, at their offsets */
static void midi_events(void *buffer, long from, jack_nframes_t n)
{
    long step = (long)g_rate / 4, hold = (long)g_rate / 5, end = g_frames * 6 / 10;
    jack_nframes_t i;

    jack_midi_clear_buffer(buffer);
    if (!g_send_midi)
        return;
    for (i = 0; i < n; i++)
    {
        long f = from + i;
        unsigned char ev[3];

        if (f < 0 || f > end)
            continue;
        if (f < end && f % step == hold % step && f >= hold)
        {
            ev[0] = 0x80;
            ev[1] = (unsigned char)g_notes[((f - hold) / step) % 8];
            ev[2] = 0;
            jack_midi_event_write(buffer, i, ev, 3);
        }
        if (f < end && f % step == 0)
        {
            ev[0] = 0x90;
            ev[1] = (unsigned char)g_notes[(f / step) % 8];
            ev[2] = 100;
            jack_midi_event_write(buffer, i, ev, 3);
        }
        if (f == end)
        {
            ev[0] = 0xB0;
            ev[1] = 123; /* all notes off */
            ev[2] = 0;
            jack_midi_event_write(buffer, i, ev, 3);
        }
    }
}

static int src_process(jack_nframes_t nframes, void *arg)
{
    float *out[CHANNELS];
    void *midi = jack_port_get_buffer(g_midi, nframes);
    jack_nframes_t i;
    int c;

    (void)arg;
    if (atomic_load(&g_quantum_seen) == 0)
        atomic_store(&g_quantum_seen, nframes);
    else if (atomic_load(&g_quantum_seen) != nframes)
        atomic_store(&g_odd_quantum, 1);
    for (c = 0; c < CHANNELS; c++)
        out[c] = jack_port_get_buffer(g_out[c], nframes);
    if (!atomic_load(&g_go) || g_played >= g_frames)
    {
        for (c = 0; c < CHANNELS; c++)
            memset(out[c], 0, nframes * sizeof(float));
        midi_events(midi, -1 - (long)nframes, nframes);
        return 0;
    }
    midi_events(midi, g_played, nframes);
    for (i = 0; i < nframes; i++)
    {
        for (c = 0; c < CHANNELS; c++)
        {
            float s = g_played < g_frames ? signal_at(c, g_played) : 0.0f;
            out[c][i] = s;
            if (g_played < g_frames)
                g_source[c][g_played] = s;
        }
        if (g_played < g_frames)
            g_played++;
    }
    return 0;
}

static int rec_process(jack_nframes_t nframes, void *arg)
{
    jack_nframes_t i;
    int k, c;

    (void)arg;
    atomic_store(&g_rec_seen, atomic_fetch_add(&g_seq, 1));
    if (!atomic_load(&g_go))
    {
        if (g_warmed < g_warm_frames)
        {
            g_warmed += nframes;
            if (g_warmed >= g_warm_frames)
                atomic_store(&g_warm_done, 1);
        }
        sem_post(&g_cycle_done);
        return 0;
    }
    if (g_recorded >= g_frames)
    {
        sem_post(&g_cycle_done);
        return 0;
    }
    g_cycles++;
    for (k = 0; k < g_clients; k++)
        for (c = 0; c < g_outs[k]; c++)
        {
            const float *in = jack_port_get_buffer(g_in[k][c], nframes);
            for (i = 0; i < nframes && g_recorded + (long)i < g_frames; i++)
                g_buf[k][c][g_recorded + i] = in[i];
        }
    g_recorded += nframes;
    if (g_recorded >= g_frames)
        atomic_store(&g_done, 1);
    sem_post(&g_cycle_done);
    return 0;
}

static int write_file(const char *dir, const char *name, int c, const float *buffer, long n)
{
    char path[4096];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s.%d.f32", dir, name, c);
    f = fopen(path, "wb");
    if (!f)
        return -1;
    if (fwrite(buffer, sizeof(float), (size_t)n, f) != (size_t)n)
    {
        fclose(f);
        return -1;
    }
    return fclose(f);
}

/* the ports of <client> of one type and direction, in the order the graph lists them */
static int client_ports(const char *client, const char *type, unsigned long flags, const char **list, int max)
{
    char pattern[512];
    const char **ports;
    int n = 0;

    snprintf(pattern, sizeof(pattern), "^%s:", client);
    ports = jack_get_ports(g_src, pattern, type, flags);
    if (!ports)
        return 0;
    while (ports[n] && n < max)
    {
        list[n] = strdup(ports[n]);
        n++;
    }
    jack_free(ports);
    return n;
}

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}



/* ---- the clock: the only driver of the graph, one cycle per trigger ---- */

static struct pw_thread_loop *g_loop;
static struct pw_filter *g_clock;
static void *g_clock_port;
static _Atomic int g_clock_state = PW_FILTER_STATE_UNCONNECTED;

/* the driver's own process: called by its trigger, as the cycle starts, and again when the graph has
 * completed the cycle */
static void clock_process(void *data, struct spa_io_position *position)
{
    float *out = pw_filter_get_dsp_buffer(g_clock_port, position->clock.duration);

    (void)data;
    if (out)
        memset(out, 0, position->clock.duration * sizeof(float));
    atomic_store(&g_clock_seen, atomic_fetch_add(&g_seq, 1));
    sem_post(&g_clock_called);
}

static void clock_state(void *data, enum pw_filter_state old, enum pw_filter_state state, const char *error)
{
    (void)data;
    if (getenv("PARITY_DEBUG"))
        fprintf(stderr, "clock: %s -> %s%s%s\n", pw_filter_state_as_string(old), pw_filter_state_as_string(state),
                error ? " " : "", error ? error : "");
    atomic_store(&g_clock_state, (int)state);
}

static const struct pw_filter_events clock_events = {
    PW_VERSION_FILTER_EVENTS,
    .state_changed = clock_state,
    .process = clock_process,
};

/* the clock at least in <state> (paused: connected; streaming: its port linked) */
static int clock_wait(int state)
{
    double until = now_ms() + 10000;

    while (atomic_load(&g_clock_state) < state && now_ms() < until)
    {
        if (atomic_load(&g_clock_state) == PW_FILTER_STATE_ERROR)
            return -1;
        usleep(1000);
    }
    return atomic_load(&g_clock_state) >= state ? 0 : -1;
}

static int clock_start(void)
{
    pw_init(NULL, NULL);
    sem_init(&g_cycle_done, 0, 0);
    sem_init(&g_clock_called, 0, 0);
    g_loop = pw_thread_loop_new("parity-clock", NULL);
    pw_thread_loop_lock(g_loop);
    g_clock = pw_filter_new_simple(pw_thread_loop_get_loop(g_loop), "parity-clock",
                                   pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Source",
                                                     PW_KEY_MEDIA_ROLE, "DSP", PW_KEY_NODE_NAME, "parity-clock",
                                                     PW_KEY_PRIORITY_DRIVER, "1000000", PW_KEY_NODE_ALWAYS_PROCESS, "true",
                                                     NULL),
                                   &clock_events, NULL);
    g_clock_port = pw_filter_add_port(g_clock, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
                                      pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                                                        PW_KEY_PORT_NAME, "out", NULL),
                                      NULL, 0);
    if (pw_filter_connect(g_clock, PW_FILTER_FLAG_DRIVER | PW_FILTER_FLAG_RT_PROCESS, NULL, 0) < 0)
    {
        pw_thread_loop_unlock(g_loop);
        return -1;
    }
    pw_thread_loop_unlock(g_loop);
    pw_thread_loop_start(g_loop);
    return clock_wait(PW_FILTER_STATE_PAUSED);
}

/* one graph cycle, waited for; paced: not before <next_ms> */
/* <client>:<min>,<max> of each client's first output, its capture latency as the graph has it now */
static void print_latencies(char **clients)
{
    int k;

    for (k = 0; k < g_clients; k++)
    {
        const char *outs[PORTS_MAX];
        jack_latency_range_t range = {0, 0};

        if (client_ports(clients[k], JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, outs, PORTS_MAX) > 0)
            jack_port_get_latency_range(jack_port_by_name(g_src, outs[0]), JackCaptureLatency, &range);
        printf("%s%s:%u,%u", k ? ";" : "", clients[k], range.min, range.max);
    }
}

static int sem_wait_s(sem_t *sem, int seconds)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += seconds;
    while (sem_timedwait(sem, &ts) != 0)
        if (errno != EINTR)
            return -1;
    return 0;
}

/* one graph cycle: triggered, then waited for until the clock is called back at its end and the recorder
 * has run in it */
static int clock_cycle(void)
{
    while (sem_trywait(&g_cycle_done) == 0)
        ;
    while (sem_trywait(&g_clock_called) == 0)
        ;
    pw_filter_trigger_process(g_clock);
    if (sem_wait_s(&g_clock_called, 10) != 0 || sem_wait_s(&g_clock_called, 10) != 0)
        return -1;
    return sem_wait_s(&g_cycle_done, 1);
}

/* cycles until *flag is set or <max> cycles ran; paced: one per quantum of wall time */
static int clock_run(_Atomic int *flag, long max, int paced, jack_nframes_t quantum)
{
    double period = 1000.0 * quantum / (double)g_rate, next = now_ms();
    long i;

    for (i = 0; i < max && !atomic_load(flag); i++)
    {
        if (paced)
        {
            double wait = next - now_ms();
            if (wait > 0)
                usleep((useconds_t)(wait * 1000));
            next += period;
        }
        if (clock_cycle() != 0)
            return -1;
    }
    return atomic_load(flag) ? 0 : -1;
}

int main(int argc, char **argv)
{
    const char *outdir;
    char line[64];
    double t0, wall;
    long warm_total = 0;
    static const char *ins_of[CLIENTS_MAX][PORTS_MAX], *outs_of[CLIENTS_MAX][PORTS_MAX], *midis_of[CLIENTS_MAX][PORTS_MAX];
    int n_ins[CLIENTS_MAX], n_outs[CLIENTS_MAX], n_midis[CLIENTS_MAX], midi_all;
    int paced = 0;
    int k, c, opt;

    while ((opt = getopt(argc, argv, "m")) != -1)
    {
        if (opt == 'm')
            g_send_midi = 1;
        else
            return 2;
    }
    if (argc - optind < 2 || argc - optind > 2 + CLIENTS_MAX)
    {
        fprintf(stderr, "usage: %s [-m] <frames> <outdir> <client>...\n", argv[0]);
        return 2;
    }
    g_frames = atol(argv[optind]);
    outdir = argv[optind + 1];
    g_clients = argc - optind - 2;
    for (c = 0; c < CHANNELS; c++)
        g_source[c] = calloc((size_t)g_frames, sizeof(float));

    if (clock_start() != 0)
    {
        fprintf(stderr, "can't start the clock\n");
        return 2;
    }
    g_src = jack_client_open("parity-src", JackNoStartServer, NULL);
    g_rec = jack_client_open("parity-rec", JackNoStartServer, NULL);
    if (!g_src || !g_rec)
    {
        fprintf(stderr, "can't get jack client\n");
        return 2;
    }
    g_rate = jack_get_sample_rate(g_src);
    g_out[0] = jack_port_register(g_src, "out_1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    g_out[1] = jack_port_register(g_src, "out_2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    g_midi = jack_port_register(g_src, "midi_out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
    /* an unlinked driver never streams: the clock's port feeds parity-rec:clock */
    g_clock_in = jack_port_register(g_rec, "clock", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    jack_set_process_callback(g_src, src_process, NULL);
    jack_set_process_callback(g_rec, rec_process, NULL);
    if (jack_activate(g_src) != 0 || jack_activate(g_rec) != 0)
    {
        fprintf(stderr, "can't activate\n");
        return 2;
    }
    {
        const char *clock_out[PORTS_MAX];

        if (getenv("PARITY_DEBUG"))
        {
            const char **all = jack_get_ports(g_src, NULL, NULL, 0);
            for (int i = 0; all && all[i]; i++)
                fprintf(stderr, "port %s\n", all[i]);
            if (all)
                jack_free(all);
        }
        if (client_ports("parity-clock", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, clock_out, PORTS_MAX) < 1 ||
            jack_connect(g_rec, clock_out[0], jack_port_name(g_clock_in)) != 0 || clock_wait(PW_FILTER_STATE_STREAMING) != 0)
        {
            fprintf(stderr, "can't start the clock: its port is not linked\n");
            return 2;
        }
    }
    printf("ready rate=%u quantum=%u\n", g_rate, jack_get_buffer_size(g_src));
    /* every client's ports, read until the graph shows them: a client's ports are registered after the host has
     * answered its add, and a list read too early is short (a client with no output is not one of ours yet) */
    for (k = 0; k < g_clients; k++)
    {
        const char *client = argv[optind + 2 + k];
        double until = now_ms() + 5000;
        int last = -1;

        for (;;)
        {
            int sig;

            n_ins[k] = client_ports(client, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, ins_of[k], PORTS_MAX);
            n_outs[k] = client_ports(client, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, outs_of[k], PORTS_MAX);
            n_midis[k] = client_ports(client, JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, midis_of[k], PORTS_MAX);
            sig = n_ins[k] * 10000 + n_outs[k] * 100 + n_midis[k];
            if ((n_outs[k] > 0 && sig == last) || now_ms() > until)
                break;
            last = sig;
            usleep(30000);
        }
    }
    /* MIDI goes to every client or to none: a MIDI input one host exposes and the other does not would feed one
     * side only (mod-host exposes an atom input that supports MIDI as a MIDI port, even beside a patch port) */
    midi_all = g_send_midi;
    for (k = 0; k < g_clients; k++)
        if (n_midis[k] == 0)
            midi_all = 0;
    for (k = 0; k < g_clients; k++)
    {
        const char *client = argv[optind + 2 + k];
        const char **ins = ins_of[k], **outs = outs_of[k], **midis = midis_of[k];
        int n_in = n_ins[k], n_out = n_outs[k], n_midi = n_midis[k];
        jack_latency_range_t range = {0, 0};
        int failed = 0, i;

        /* the recorder's ports, registered once the graph is known: a port list read before the activation can
         * miss a client's ports */
        g_outs[k] = n_out < CHANNELS ? n_out : CHANNELS;
        for (c = 0; c < g_outs[k]; c++)
        {
            char name[64];

            snprintf(name, sizeof(name), "%s_%d", client, c + 1);
            g_buf[k][c] = calloc((size_t)g_frames, sizeof(float));
            g_in[k][c] = jack_port_register(g_rec, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
            failed |= g_in[k][c] == NULL;
        }

        for (i = 0; i < n_in && i < CHANNELS; i++)
            failed |= jack_connect(g_src, jack_port_name(g_out[i]), ins[i]) != 0;
        if (midi_all)
            failed |= jack_connect(g_src, jack_port_name(g_midi), midis[0]) != 0;
        for (c = 0; c < g_outs[k]; c++)
            failed |= jack_connect(g_rec, outs[c], jack_port_name(g_in[k][c])) != 0;
        if (n_out > 0)
            jack_port_get_latency_range(jack_port_by_name(g_src, outs[0]), JackCaptureLatency, &range);
        printf("linked %s in=%d out=%d midi=%d midi_fed=%d latency=%u,%u%s\n", client, n_in, n_out, n_midi > 0,
               midi_all, range.min, range.max, failed ? " FAILED" : "");
        if (failed)
            return 1;
    }
    fflush(stdout);

    /* "warm <blocks>" runs that many cycles of silence; "go" (or "go paced") plays */
    while (fgets(line, sizeof(line), stdin))
    {
        long blocks;

        if (strncmp(line, "go", 2) == 0)
        {
            paced = strstr(line, "paced") != NULL;
            break;
        }
        if (sscanf(line, "warm %ld", &blocks) != 1 || blocks <= 0)
            continue;
        atomic_store(&g_warm_done, 0);
        g_warmed = 0;
        g_warm_frames = blocks * (long)jack_get_buffer_size(g_src);
        if (clock_run(&g_warm_done, blocks + 2, 0, jack_get_buffer_size(g_src)) != 0)
        {
            fprintf(stderr, "warm: the graph did not complete a cycle (clock process %ld, recorder %ld, seq %ld, driving %d)\n",
                    atomic_load(&g_clock_seen), atomic_load(&g_rec_seen), atomic_load(&g_seq), pw_filter_is_driving(g_clock));
            return 1;
        }
        warm_total += blocks;
        printf("warm blocks=%ld latency=", warm_total);
        print_latencies(argv + optind + 2);
        printf("\n");
        fflush(stdout);
    }
    atomic_store(&g_go, 1);
    t0 = now_ms();
    clock_run(&g_done, g_frames / (long)jack_get_buffer_size(g_src) + 4, paced, jack_get_buffer_size(g_src));
    wall = now_ms() - t0;
    jack_deactivate(g_rec);
    jack_deactivate(g_src);

    if (!atomic_load(&g_done))
    {
        fprintf(stderr, "recorded %ld of %ld frames: no cycles\n", g_recorded, g_frames);
        return 1;
    }
    for (c = 0; c < CHANNELS; c++)
        if (write_file(outdir, "source", c, g_source[c], g_frames) != 0)
            return 1;
    for (k = 0; k < g_clients; k++)
        for (c = 0; c < g_outs[k]; c++)
            if (write_file(outdir, argv[optind + 2 + k], c, g_buf[k][c], g_frames) != 0)
            {
                fprintf(stderr, "can't write the recordings\n");
                return 1;
            }
    if (getenv("PARITY_DEBUG"))
        fprintf(stderr, "order: last clock process %ld, last recorder cycle %ld\n", atomic_load(&g_clock_seen),
                atomic_load(&g_rec_seen));
    printf("done frames=%ld cycles=%ld quantum=%u%s wall_ms=%.1f\n", g_frames, g_cycles,
           atomic_load(&g_quantum_seen), atomic_load(&g_odd_quantum) ? "(varied)" : "", wall);
    jack_client_close(g_rec);
    jack_client_close(g_src);
    return 0;
}
