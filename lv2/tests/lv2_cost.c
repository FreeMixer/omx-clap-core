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

/*
 * lv2_cost.c — what the adapter costs per process() call: the stereo pad of the fakes bundle at 96 kHz and 192 kHz,
 * quantum 128, timed through the CLAP face (the copy in, the event walk, the LV2 run, the copy out) and through
 * lv2/core's run alone, the difference being the shim's own cost. It prints figures and judges nothing: the ceiling is
 * the console's to hold.
 *
 *   lv2_cost <omx-lv2-fakes.lv2 directory> [calls]
 *
 * Each figure is the median of 31 rounds, each round `calls` / 31 calls timed with CLOCK_MONOTONIC.
 */
#include <stdint.h>
#include <time.h>

#include <clap/clap.h>

#include "lv2_core.h"
#include "omx_clap_lv2.h"
#include "fixtures/lv2_fakes.h"
#include "lv2_test_util.h"

#define QUANTUM 128u
#define ROUNDS 31

static bool out_push(const clap_output_events_t *list, const clap_event_header_t *event)
{
    (void)list;
    (void)event;
    return true;
}

static uint32_t in_size(const clap_input_events_t *list)
{
    (void)list;
    return 0;
}

static const clap_event_header_t *in_get(const clap_input_events_t *list, uint32_t index)
{
    (void)list;
    (void)index;
    return NULL;
}

static const void *host_ext(const clap_host_t *host, const char *id)
{
    (void)host;
    (void)id;
    return NULL;
}

static void host_nop(const clap_host_t *host)
{
    (void)host;
}

static double now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static int cmp_double(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;

    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    static const double rates[] = { 96000.0, 192000.0 };
    omx_clap_lv2_config_t config = { omx_clap_lv2_provided, 1000u, 4096u, 0u, 250u, NULL };
    const uint32_t calls = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : 310000u;
    char dir[PATH_MAX], why[OMX_CLAP_LV2_WHY_MAX];
    static float l[QUANTUM], r[QUANTUM], ol[QUANTUM], or_[QUANTUM];
    clap_host_t host = { CLAP_VERSION_INIT, NULL, "lv2_cost", "", "", "0", host_ext, host_nop, host_nop, host_nop };
    const clap_input_events_t in_events = { NULL, in_size, in_get };
    const clap_output_events_t out_events = { NULL, out_push };
    size_t k;

    if (argc < 2 || abs_dir(argv[1], dir) != 0 || omx_clap_lv2_configure(&config, why) != 0)
    {
        printf("usage: lv2_cost <omx-lv2-fakes.lv2> [calls]\n");
        return 1;
    }
    tone(l, QUANTUM, 0.5f, 0);
    tone(r, QUANTUM, 0.25f, 7);
    for (k = 0; k < sizeof(rates) / sizeof(rates[0]); k++)
    {
        const clap_plugin_entry_t *entry = omx_clap_lv2_entry(dir, why);
        const clap_plugin_factory_t *f = entry ? entry->get_factory(CLAP_PLUGIN_FACTORY_ID) : NULL;
        const clap_plugin_t *p = f ? f->create_plugin(f, &host, FAKE_PAD2) : NULL;
        float *ins[2] = { l, r }, *outs[2] = { ol, or_ };
        clap_audio_buffer_t ib = { ins, NULL, 2, 0, 0 }, ob = { outs, NULL, 2, 0, 0 };
        clap_process_t proc;
        double face[ROUNDS], core[ROUNDS];
        const uint32_t per = calls / ROUNDS ? calls / ROUNDS : 1u;
        struct lv2_bundle *b;
        struct lv2_plugin *lp;
        struct lv2_instance *li;
        int round;
        uint32_t i;

        if (!p || !p->init(p) || !p->activate(p, rates[k], 1, QUANTUM) || !p->start_processing(p))
        {
            printf("FAIL the stereo pad does not rack at %.0f\n", rates[k]);
            return 1;
        }
        memset(&proc, 0, sizeof(proc));
        proc.steady_time = -1;
        proc.frames_count = QUANTUM;
        proc.audio_inputs = &ib;
        proc.audio_outputs = &ob;
        proc.audio_inputs_count = 1;
        proc.audio_outputs_count = 1;
        proc.in_events = &in_events;
        proc.out_events = &out_events;
        b = lv2_bundle_ref(dir, why);
        lp = b ? lv2_plugin_open(b, FAKE_PAD2, why) : NULL;
        li = lp && lv2_plugin_load(lp, why) == 0 ? lv2_instance_new(lp) : NULL;
        if (!li || lv2_instance_activate(li, rates[k], 1, QUANTUM, why) != 0)
        {
            printf("FAIL the bare instance does not activate at %.0f\n", rates[k]);
            return 1;
        }
        for (round = 0; round < ROUNDS; round++)
        {
            double t0 = now_ns();

            for (i = 0; i < per; i++)
                p->process(p, &proc);
            face[round] = (now_ns() - t0) / per;
            t0 = now_ns();
            for (i = 0; i < per; i++)
                lv2_instance_run(li, QUANTUM, NULL, NULL);
            core[round] = (now_ns() - t0) / per;
        }
        qsort(face, ROUNDS, sizeof(double), cmp_double);
        qsort(core, ROUNDS, sizeof(double), cmp_double);
        printf("lv2 adapter cost @ %.0f Hz, quantum %u, 2x2: process() %.1f ns, the LV2 run alone %.1f ns, the shim %.1f ns per call (median of %d x %u)\n",
               rates[k], QUANTUM, face[ROUNDS / 2], core[ROUNDS / 2], face[ROUNDS / 2] - core[ROUNDS / 2], ROUNDS, per);
        lv2_instance_free(li);
        lv2_plugin_close(lp);
        lv2_bundle_unref(b);
        p->deactivate(p);
        p->destroy(p);
        omx_clap_lv2_entry_release(entry);
    }
    return 0;
}
