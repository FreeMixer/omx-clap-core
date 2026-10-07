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

/* The adapter as a program outside this tree sees it: compiled against the installed omx_clap_lv2.h, linked by
 * `pkg-config --libs omx-clap-lv2` alone. It configures with the adapter's own list, takes the entry of the fixture
 * bundle (argument 1), creates the fixture and runs one block of it at -26 dB. */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <clap/clap.h>
#include <omx_clap_lv2.h>

static const void *ext(const clap_host_t *h, const char *id)
{
    (void)h;
    (void)id;
    return NULL;
}

static void nop(const clap_host_t *h)
{
    (void)h;
}

static uint32_t in_size(const clap_input_events_t *l)
{
    (void)l;
    return 0;
}

static const clap_event_header_t *in_get(const clap_input_events_t *l, uint32_t i)
{
    (void)l;
    (void)i;
    return NULL;
}

static bool out_push(const clap_output_events_t *l, const clap_event_header_t *e)
{
    (void)l;
    (void)e;
    return true;
}

int main(int argc, char **argv)
{
    const omx_clap_lv2_config_t config = { omx_clap_lv2_provided, 1000u, 4096u, 0u, 250u, NULL };
    clap_host_t host = { CLAP_VERSION_INIT, NULL, "lv2_link_test", "", "", "0", ext, nop, nop, nop };
    const clap_input_events_t in_events = { NULL, in_size, in_get };
    const clap_output_events_t out_events = { NULL, out_push };
    char why[OMX_CLAP_LV2_WHY_MAX];
    const clap_plugin_entry_t *entry;
    const clap_plugin_factory_t *f;
    const clap_plugin_t *p;
    float x[64], y[64], *ins[1] = { x }, *outs[1] = { y };
    clap_audio_buffer_t ib = { ins, NULL, 1, 0, 0 }, ob = { outs, NULL, 1, 0, 0 };
    clap_process_t proc;
    double ex = 0, ey = 0, db;
    int i;

    if (argc < 2 || omx_clap_lv2_configure(&config, why) != 0 || !(entry = omx_clap_lv2_entry(argv[1], why)))
    {
        printf("FAIL the adapter does not take %s (%s)\n", argc > 1 ? argv[1] : "", why);
        return 1;
    }
    f = entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    p = f ? f->create_plugin(f, &host, "urn:openmixer:test:host-fixture") : NULL;
    if (!p || !p->init(p) || !p->activate(p, 48000.0, 1, 64) || !p->start_processing(p))
    {
        printf("FAIL the fixture does not rack\n");
        return 1;
    }
    for (i = 0; i < 64; i++)
        x[i] = 0.5f * sinf(0.0613f * (float)i);
    memset(&proc, 0, sizeof(proc));
    proc.steady_time = -1;
    proc.frames_count = 64;
    proc.audio_inputs = &ib;
    proc.audio_outputs = &ob;
    proc.audio_inputs_count = 1;
    proc.audio_outputs_count = 1;
    proc.in_events = &in_events;
    proc.out_events = &out_events;
    p->process(p, &proc);
    for (i = 0; i < 64; i++)
    {
        ex += (double)x[i] * x[i];
        ey += (double)y[i] * y[i];
    }
    db = 10.0 * log10(ey / ex);
    p->deactivate(p);
    p->destroy(p);
    omx_clap_lv2_entry_release(entry);
    if (fabs(db + 26.0) >= 0.01)
    {
        printf("FAIL the fixture reads %.3f dB, not -26.00\n", db);
        return 1;
    }
    printf("lv2 link test ok: the fixture reads %.3f dB\n", db);
    return 0;
}
