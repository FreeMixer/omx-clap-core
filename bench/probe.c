/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 */
/*
 * probe.c: the bench's source and recorder, one JACK client in the private graph.
 *
 *   probe <noise inputs> <impulse inputs>
 *
 * Two clients, so a cycle runs bench-src, then the plugins, then bench-rec, and an impulse through a path that adds no
 * latency arrives in the cycle it left. bench-src's outputs: src_l and src_r carry white noise at -20 dBFS (one fixed xorshift sequence), imp one unit impulse every
 * rate / 4 frames. Inputs: rec_<k> take what a plugin fed with the noise gives back, lat_<k> what a plugin (or a
 * direct link, the reference) fed with the impulse gives back.
 *
 * Measured on the audio thread, read by commands on stdin, one line per answer:
 *   stats   one line per input: first non-silent block (CLOCK_REALTIME ns), latency of the last impulses in frames
 *           (min and max over the window), silent blocks after the first sound; and one line for the client: cycles,
 *           the longest gap between two cycles (CLOCK_MONOTONIC us), the cycles that came more than 1.5 periods
 *           after the one before (late), xruns jack reported
 *   reset   clear the window: latencies, gaps, silent-block counts and xruns (first-sound times are kept)
 *   quit
 * It prints "ready <rate> <quantum>" once active.
 */
#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <jack/jack.h>

#define MAX_IN 32
#define THRESHOLD 0.001f /* -60 dBFS: the quietest output here is fault-14 at -40 dB under the -20 dBFS noise */

struct input
{
    jack_port_t *port;
    int impulse;                  /* fed by imp: latency is measured */
    _Atomic int64_t first_ns;     /* CLOCK_REALTIME of the first non-silent block, 0 before */
    _Atomic int64_t lat_min, lat_max; /* frames, over the window; -1: none seen */
    _Atomic uint64_t lat_count;
    _Atomic uint64_t silent_after; /* fully silent blocks once sound was seen */
    _Atomic int64_t silent_since_ns; /* CLOCK_REALTIME of the first silent block after sound in the window */
    int armed;                    /* an impulse went out and has not been seen yet */
};

static jack_client_t *client, *src;
static jack_port_t *src_l, *src_r, *imp;
static struct input inputs[MAX_IN];
static int n_inputs;
static uint64_t imp_period;
static _Atomic uint64_t last_imp = UINT64_MAX; /* the graph frame time of the last impulse */
static uint32_t rng = 0x9e3779b9u;
static _Atomic uint64_t cycles;
static _Atomic int64_t max_gap_ns;
static _Atomic uint64_t late;      /* cycles that came more than 1.5 periods after the one before: a missed cycle */
static int64_t period_ns;
static int64_t last_cycle_ns;
static _Atomic int reset_req;
static _Atomic uint64_t xruns;

static int64_t now_ns(clockid_t c)
{
    struct timespec ts;

    clock_gettime(c, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static float noise(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    /* uniform in [-1, 1) scaled to a -20 dBFS peak */
    return ((float)rng / 2147483648.0f - 1.0f) * 0.1f;
}

static void window_reset(void)
{
    for (int i = 0; i < n_inputs; i++)
    {
        atomic_store(&inputs[i].lat_min, -1);
        atomic_store(&inputs[i].lat_max, -1);
        atomic_store(&inputs[i].lat_count, 0);
        atomic_store(&inputs[i].silent_after, 0);
        atomic_store(&inputs[i].silent_since_ns, 0);
    }
    atomic_store(&max_gap_ns, 0);
    atomic_store(&late, 0);
    atomic_store(&xruns, 0);
}

/* the source: its own client, so the graph runs it before the plugins and the recorder after them in the same cycle */
static int on_source(jack_nframes_t n, void *arg)
{
    (void)arg;
    const uint64_t t0 = jack_last_frame_time(src);
    float *l = jack_port_get_buffer(src_l, n), *r = jack_port_get_buffer(src_r, n), *p = jack_port_get_buffer(imp, n);

    memset(p, 0, n * sizeof(float));
    for (jack_nframes_t i = 0; i < n; i++)
    {
        l[i] = noise();
        r[i] = noise();
        if ((t0 + i) % imp_period == 0)
        {
            p[i] = 1.0f;
            atomic_store_explicit(&last_imp, t0 + i, memory_order_release);
        }
    }
    return 0;
}

static int on_process(jack_nframes_t n, void *arg)
{
    (void)arg;
    const int64_t mono = now_ns(CLOCK_MONOTONIC);
    const uint64_t t0 = jack_last_frame_time(client);
    const uint64_t imp_at = atomic_load_explicit(&last_imp, memory_order_acquire);
    static uint64_t seen_imp = UINT64_MAX;
    int64_t real = 0;

    if (atomic_exchange(&reset_req, 0))
    {
        window_reset();
        last_cycle_ns = 0;
    }
    if (last_cycle_ns && mono - last_cycle_ns > atomic_load_explicit(&max_gap_ns, memory_order_relaxed))
        atomic_store_explicit(&max_gap_ns, mono - last_cycle_ns, memory_order_relaxed);
    if (last_cycle_ns && 2 * (mono - last_cycle_ns) > 3 * period_ns)
        atomic_fetch_add_explicit(&late, 1, memory_order_relaxed);
    last_cycle_ns = mono;
    if (imp_at != seen_imp)
    {
        seen_imp = imp_at;
        for (int k = 0; k < n_inputs; k++)
            inputs[k].armed = 1;
    }

    for (int k = 0; k < n_inputs; k++)
    {
        struct input *in = &inputs[k];
        const float *x = jack_port_get_buffer(in->port, n);
        int sound = 0;

        for (jack_nframes_t i = 0; i < n; i++)
        {
            if (fabsf(x[i]) > THRESHOLD)
            {
                sound = 1;
                if (in->impulse && in->armed && imp_at != UINT64_MAX && t0 + i >= imp_at)
                {
                    const int64_t lat = (int64_t)(t0 + i - imp_at);
                    int64_t mn = atomic_load_explicit(&in->lat_min, memory_order_relaxed);
                    int64_t mx = atomic_load_explicit(&in->lat_max, memory_order_relaxed);

                    if (mn < 0 || lat < mn)
                        atomic_store_explicit(&in->lat_min, lat, memory_order_relaxed);
                    if (lat > mx)
                        atomic_store_explicit(&in->lat_max, lat, memory_order_relaxed);
                    atomic_fetch_add_explicit(&in->lat_count, 1, memory_order_relaxed);
                    in->armed = 0;
                }
                if (!in->impulse)
                    break;
            }
        }
        /* an impulse input is silent between impulses: only a noise input's silence means its source died */
        if (!real)
            real = now_ns(CLOCK_REALTIME);
        if (sound && !atomic_load_explicit(&in->first_ns, memory_order_relaxed))
            atomic_store_explicit(&in->first_ns, real, memory_order_relaxed);
        if (!sound && !in->impulse && atomic_load_explicit(&in->first_ns, memory_order_relaxed))
        {
            if (!atomic_fetch_add_explicit(&in->silent_after, 1, memory_order_relaxed))
                atomic_store_explicit(&in->silent_since_ns, real, memory_order_relaxed);
        }
    }
    atomic_fetch_add_explicit(&cycles, 1, memory_order_relaxed);
    return 0;
}

static int on_xrun(void *arg)
{
    (void)arg;
    atomic_fetch_add(&xruns, 1);
    return 0;
}

int main(int argc, char **argv)
{
    char name[32], line[64];
    int n_noise, n_imp;

    if (argc != 3 || (n_noise = atoi(argv[1])) < 0 || (n_imp = atoi(argv[2])) < 0 || n_noise + n_imp > MAX_IN)
    {
        fprintf(stderr, "usage: probe <noise inputs> <impulse inputs> (at most %d in all)\n", MAX_IN);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    client = jack_client_open("bench-rec", JackNoStartServer, NULL);
    src = jack_client_open("bench-src", JackNoStartServer, NULL);
    if (!client || !src)
    {
        fprintf(stderr, "no jack server\n");
        return 2;
    }
    src_l = jack_port_register(src, "src_l", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    src_r = jack_port_register(src, "src_r", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    imp = jack_port_register(src, "imp", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    for (int k = 0; k < n_noise + n_imp; k++)
    {
        inputs[k].impulse = k >= n_noise;
        snprintf(name, sizeof(name), inputs[k].impulse ? "lat_%d" : "rec_%d", inputs[k].impulse ? k - n_noise : k);
        inputs[k].port = jack_port_register(client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    }
    n_inputs = n_noise + n_imp;
    imp_period = jack_get_sample_rate(client) / 4;
    period_ns = (int64_t)jack_get_buffer_size(client) * 1000000000 / jack_get_sample_rate(client);
    window_reset();
    jack_set_process_callback(client, on_process, NULL);
    jack_set_xrun_callback(client, on_xrun, NULL);
    jack_set_process_callback(src, on_source, NULL);
    if (jack_activate(src) || jack_activate(client))
        return 2;
    printf("ready %u %u\n", jack_get_sample_rate(client), jack_get_buffer_size(client));

    while (fgets(line, sizeof(line), stdin))
    {
        if (!strncmp(line, "stats", 5))
        {
            for (int k = 0; k < n_inputs; k++)
            {
                struct input *in = &inputs[k];
                printf("input %s first_ns=%" PRId64 " lat_min=%" PRId64 " lat_max=%" PRId64 " lat_n=%" PRIu64
                       " silent_after=%" PRIu64 " silent_since_ns=%" PRId64 "\n",
                       jack_port_short_name(in->port), atomic_load(&in->first_ns), atomic_load(&in->lat_min),
                       atomic_load(&in->lat_max), atomic_load(&in->lat_count), atomic_load(&in->silent_after),
                       atomic_load(&in->silent_since_ns));
            }
            printf("client cycles=%" PRIu64 " max_gap_us=%.1f late=%" PRIu64 " xruns=%" PRIu64 " quantum=%u\n",
                   atomic_load(&cycles), atomic_load(&max_gap_ns) / 1000.0, atomic_load(&late), atomic_load(&xruns),
                   jack_get_buffer_size(client));
            printf("end\n");
        }
        else if (!strncmp(line, "reset", 5))
        {
            atomic_store(&reset_req, 1);
            printf("ok\n");
        }
        else if (!strncmp(line, "quit", 4))
            break;
    }
    jack_client_close(client);
    jack_client_close(src);
    return 0;
}
