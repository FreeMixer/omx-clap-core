/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * inproc.c: the in-process path of the hosting bench. Each plugin runs inside a native PipeWire filter's process
 * callback, on the PipeWire data thread, through libomx-clap-core's stage (omx_clap_host_open / _open_entry, activate,
 * publish, omx_clap_host_run) with the core's default configuration (OMX_BENCH_GUARDS=off: omx-clap-host's): the calls and the configuration the engine's
 * in-process tier makes (pipewire-native mix_clap_host.h, mix_host_backend.c), with no socket, no JACK layer and no
 * other process. An LV2 plugin is opened through libomx-clap-lv2's entry, as the engine opens one.
 *
 * It is not the engine: one filter node per plugin (the engine runs a lane's chain in its own node), no mixer around
 * it. What it measures is the plugin's cost when its host is a PipeWire node in the same process.
 *
 * Commands on stdin, one per line, each answered "resp <code>" on stdout (mod-host's grammar where it overlaps):
 *   add clap <file.clap> <id> <n>       a filter "effect_<n>" with in_1 in_2 out_1 out_2, the plugin live in it
 *   add lv2 <bundle dir> <uri> <n>      the same for an LV2 plugin through the adapter
 *   param_set <n> <param id> <value>    a CLAP parameter write (an LV2 control's id is its index in the adapter)
 *   remove <n>
 *   stats <n>                           the filter's cycles, those that had no input or no output buffer, and the
 *                                       time omx_clap_host_run itself took per block (p50, p99, max, us): the
 *                                       plugin's cost to an engine that runs it inline, with no node of its own
 *   reset                               clear every instance's run-time histogram
 *   quit
 */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <spa/pod/builder.h>
#include <spa/param/latency-utils.h>

#include "clap_host.h"
#include "omx_clap_lv2.h"

#define MAX_FX 64
#define MAX_BLOCK 8192
#define HIST_NS 100u      /* the run-time histogram's bucket */
#define HIST_N 100000u    /* 100 ns buckets up to 10 ms; a longer run lands in the last */

struct port
{
    int unused;
};

struct fx
{
    struct pw_filter *filter;
    struct port *in[2], *out[2];
    struct omx_clap_instance *clap;
    const clap_plugin_entry_t *lv2_entry;
    _Atomic pthread_t rt;         /* the data thread, once the first cycle names it */
    _Atomic int have_rt;
    _Atomic int published;
    _Atomic unsigned long cycles, no_input, no_output;  /* what `stats` reports */
    _Atomic uint32_t hist[HIST_N];  /* omx_clap_host_run's own time per block, CLOCK_MONOTONIC */
    _Atomic uint64_t run_max_ns;
    _Atomic int reset;
    float l[MAX_BLOCK], r[MAX_BLOCK];
};

static struct pw_thread_loop *loop;
static struct fx *fxs[MAX_FX];
static double rate;
static int lv2_configured;

static void on_process(void *data, struct spa_io_position *position)
{
    struct fx *fx = data;
    const uint32_t n = position->clock.duration;
    struct timespec t0, t1;
    float *in_l = pw_filter_get_dsp_buffer(fx->in[0], n), *in_r = pw_filter_get_dsp_buffer(fx->in[1], n);
    float *out_l = pw_filter_get_dsp_buffer(fx->out[0], n), *out_r = pw_filter_get_dsp_buffer(fx->out[1], n);

    if (!atomic_load_explicit(&fx->have_rt, memory_order_acquire))
    {
        omx_denormals_off();
        atomic_store_explicit(&fx->rt, pthread_self(), memory_order_relaxed);
        atomic_store_explicit(&fx->have_rt, 1, memory_order_release);
    }
    atomic_fetch_add_explicit(&fx->cycles, 1, memory_order_relaxed);
    if (!in_l || !in_r)
        atomic_fetch_add_explicit(&fx->no_input, 1, memory_order_relaxed);
    /* an output nothing is linked to has no buffer: the plugin runs all the same */
    if (!out_l || !out_r)
        atomic_fetch_add_explicit(&fx->no_output, 1, memory_order_relaxed);
    if (n > MAX_BLOCK)
        return;
    if (in_l)
        memcpy(fx->l, in_l, n * sizeof(float));
    else
        memset(fx->l, 0, n * sizeof(float));
    if (in_r)
        memcpy(fx->r, in_r, n * sizeof(float));
    else
        memset(fx->r, 0, n * sizeof(float));
    if (atomic_exchange_explicit(&fx->reset, 0, memory_order_acquire))
    {
        for (uint32_t i = 0; i < HIST_N; i++)
            atomic_store_explicit(&fx->hist[i], 0, memory_order_relaxed);
        atomic_store_explicit(&fx->run_max_ns, 0, memory_order_relaxed);
    }
    /* before the control thread publishes the stage the run is the stage's own pass-through */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    omx_clap_host_run(fx->clap, fx->l, fx->r, n);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    {
        const uint64_t ns = (uint64_t)((t1.tv_sec - t0.tv_sec) * 1000000000 + (t1.tv_nsec - t0.tv_nsec));
        const uint64_t b = ns / HIST_NS;

        atomic_fetch_add_explicit(&fx->hist[b < HIST_N ? b : HIST_N - 1], 1, memory_order_relaxed);
        if (ns > atomic_load_explicit(&fx->run_max_ns, memory_order_relaxed))
            atomic_store_explicit(&fx->run_max_ns, ns, memory_order_relaxed);
    }
    if (out_l)
        memcpy(out_l, fx->l, n * sizeof(float));
    if (out_r)
        memcpy(out_r, fx->r, n * sizeof(float));
}

static const struct pw_filter_events filter_events = {
    PW_VERSION_FILTER_EVENTS,
    .process = on_process,
};

static struct port *add_port(struct pw_filter *f, enum spa_direction dir, const char *name)
{
    return pw_filter_add_port(f, dir, PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(struct port),
                              pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio", PW_KEY_PORT_NAME, name, NULL),
                              NULL, 0);
}

static int lv2_configure(char why[OMX_CLAP_LV2_WHY_MAX])
{
    /* the numbers the adapter's own tests run with */
    static const omx_clap_lv2_config_t config = {omx_clap_lv2_provided, 1000u, 4096u, 0u, 100u, NULL};

    if (lv2_configured)
        return 0;
    if (omx_clap_lv2_configure(&config, why) != 0)
        return -1;
    lv2_configured = 1;
    return 0;
}

static int fx_add(const char *kind, const char *path, const char *id, int n, char *why, size_t why_size)
{
    char reason[OMX_CLAP_WHY_MAX > OMX_CLAP_LV2_WHY_MAX ? OMX_CLAP_WHY_MAX : OMX_CLAP_LV2_WHY_MAX] = "";
    char name[32];
    struct fx *fx;
    int ret;

    if (n < 0 || n >= MAX_FX || fxs[n])
    {
        snprintf(why, why_size, "bad instance");
        return -1;
    }
    fx = calloc(1, sizeof(*fx));
    if (!fx)
        return -1;
    if (!strcmp(kind, "lv2"))
    {
        if (lv2_configure(reason) != 0 || !(fx->lv2_entry = omx_clap_lv2_entry(path, reason)))
        {
            snprintf(why, why_size, "lv2 entry: %s", reason);
            free(fx);
            return -1;
        }
        ret = omx_clap_host_open_entry(fx->lv2_entry, id, &fx->clap, reason);
    }
    else
        ret = omx_clap_host_open(path, id, &fx->clap, reason);
    if (ret != 0)
    {
        snprintf(why, why_size, "open: %s", reason);
        if (fx->lv2_entry)
            omx_clap_lv2_entry_release(fx->lv2_entry);
        free(fx);
        return -1;
    }
    if (omx_clap_host_activate(fx->clap, rate, MAX_BLOCK, reason) != 0)
    {
        snprintf(why, why_size, "activate: %s", reason);
        omx_clap_host_close(fx->clap);
        if (fx->lv2_entry)
            omx_clap_lv2_entry_release(fx->lv2_entry);
        free(fx);
        return -1;
    }

    snprintf(name, sizeof(name), "effect_%d", n);
    pw_thread_loop_lock(loop);
    fx->filter = pw_filter_new_simple(pw_thread_loop_get_loop(loop), name,
                                      pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Filter",
                                                        PW_KEY_MEDIA_ROLE, "DSP", PW_KEY_NODE_NAME, name, NULL),
                                      &filter_events, fx);
    fx->in[0] = add_port(fx->filter, SPA_DIRECTION_INPUT, "in_1");
    fx->in[1] = add_port(fx->filter, SPA_DIRECTION_INPUT, "in_2");
    fx->out[0] = add_port(fx->filter, SPA_DIRECTION_OUTPUT, "out_1");
    fx->out[1] = add_port(fx->filter, SPA_DIRECTION_OUTPUT, "out_2");
    ret = pw_filter_connect(fx->filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0);
    pw_thread_loop_unlock(loop);
    if (ret < 0)
    {
        snprintf(why, why_size, "pw_filter_connect: %s", strerror(-ret));
        return -1;
    }
    fxs[n] = fx;
    return 0;
}

static void fx_remove(int n)
{
    struct fx *fx = fxs[n];

    if (atomic_load(&fx->published))
        omx_clap_host_unpublish(fx->clap, 1000, 1000000);
    pw_thread_loop_lock(loop);
    pw_filter_destroy(fx->filter);
    pw_thread_loop_unlock(loop);
    omx_clap_host_stop(fx->clap);
    omx_clap_host_deactivate(fx->clap);
    omx_clap_host_close(fx->clap);
    if (fx->lv2_entry)
        omx_clap_lv2_entry_release(fx->lv2_entry);
    free(fx);
    fxs[n] = NULL;
}

/* the control thread: publish each stage once its filter's first cycle has named the data thread */
static void *publisher(void *arg)
{
    const struct timespec poll = {0, 1000000};

    (void)arg;
    for (;;)
    {
        for (int n = 0; n < MAX_FX; n++)
        {
            struct fx *fx = fxs[n];

            if (fx && !atomic_load(&fx->published) && atomic_load_explicit(&fx->have_rt, memory_order_acquire))
            {
                omx_clap_host_publish(fx->clap, atomic_load_explicit(&fx->rt, memory_order_relaxed));
                atomic_store(&fx->published, 1);
            }
        }
        nanosleep(&poll, NULL);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    char line[4096], kind[16], path[2048], id[512], why[256];
    pthread_t pub;
    int n;

    if (argc != 2 || (rate = atof(argv[1])) <= 0)
    {
        fprintf(stderr, "usage: inproc <rate>\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* OMX_BENCH_GUARDS=off: omx-clap-host's configuration (no clamp, no non-finite scan, no warm-up), to tell the
     * engine's per-block guards from the hosting path itself; unset, the core's defaults, which the engine runs */
    if (getenv("OMX_BENCH_GUARDS") && !strcmp(getenv("OMX_BENCH_GUARDS"), "off"))
    {
        struct omx_clap_host_config config;

        omx_clap_host_config_default(&config);
        config.clamp = 0;
        config.nonfinite = 0;
        config.warmup = 0;
        if (omx_clap_host_configure(&config) != 0)
            return 2;
    }
    pw_init(&argc, &argv);
    loop = pw_thread_loop_new("inproc", NULL);
    if (!loop || pw_thread_loop_start(loop) < 0)
        return 2;
    pthread_create(&pub, NULL, publisher, NULL);
    printf("ready\n");

    while (fgets(line, sizeof(line), stdin))
    {
        double value;
        unsigned pid;

        why[0] = 0;
        if (sscanf(line, "add %15s %2047s %511s %d", kind, path, id, &n) == 4)
        {
            if (fx_add(kind, path, id, n, why, sizeof(why)) == 0)
                printf("resp %d\n", n);
            else
                printf("resp -1 %s\n", why);
        }
        else if (sscanf(line, "param_set %d %u %lf", &n, &pid, &value) == 3)
        {
            if (n >= 0 && n < MAX_FX && fxs[n] && omx_clap_host_param_write(fxs[n]->clap, pid, value) == 0)
                printf("resp 0\n");
            else
                printf("resp -1\n");
        }
        else if (sscanf(line, "remove %d", &n) == 1)
        {
            if (n >= 0 && n < MAX_FX && fxs[n])
            {
                fx_remove(n);
                printf("resp 0\n");
            }
            else
                printf("resp -1\n");
        }
        else if (sscanf(line, "stats %d", &n) == 1)
        {
            if (n >= 0 && n < MAX_FX && fxs[n])
            {
                struct fx *fx = fxs[n];
                uint64_t total = 0, acc = 0;
                double p50 = -1, p99 = -1;

                for (uint32_t i = 0; i < HIST_N; i++)
                    total += atomic_load_explicit(&fx->hist[i], memory_order_relaxed);
                for (uint32_t i = 0; i < HIST_N && total; i++)
                {
                    acc += atomic_load_explicit(&fx->hist[i], memory_order_relaxed);
                    if (p50 < 0 && acc * 2 >= total)
                        p50 = (i + 0.5) * HIST_NS / 1000.0;
                    if (p99 < 0 && acc * 100 >= total * 99)
                        p99 = (i + 0.5) * HIST_NS / 1000.0;
                }
                printf("resp 0 cycles=%lu no_input=%lu no_output=%lu published=%d runs=%llu run_p50=%.2f run_p99=%.2f "
                       "run_max=%.2f\n", atomic_load(&fx->cycles), atomic_load(&fx->no_input), atomic_load(&fx->no_output),
                       atomic_load(&fx->published), (unsigned long long)total, p50, p99,
                       atomic_load(&fx->run_max_ns) / 1000.0);
            }
            else
                printf("resp -1\n");
        }
        else if (!strncmp(line, "reset", 5))
        {
            for (n = 0; n < MAX_FX; n++)
                if (fxs[n])
                    atomic_store_explicit(&fxs[n]->reset, 1, memory_order_release);
            printf("resp 0\n");
        }
        else if (!strncmp(line, "quit", 4))
        {
            printf("resp 0\n");
            break;
        }
        else
            printf("resp -1 unknown\n");
    }
    _exit(0);
}
