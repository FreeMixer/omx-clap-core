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

/* The core as a program outside this tree sees it: compiled against the installed headers, linked to libomx-clap-core.so
 * alone and configured as nothing but the defaults. It shows what the isolated host's configuration turns off:
 * the clamp, the scan for non-finite output with its strike, the warm-up and the restart after it, and the refusal of a
 * note input and of an instrument. Argument 1 is tests/fake.clap, argument 2 tests/fake_synth.clap. */

#include <pthread.h>
#include <stdlib.h>

#include <omx-clap-host/clap_host.h>

#include "test_util.h"

static int open_ok(const char *path, const char *id, struct omx_clap_instance **instance, char why[OMX_CLAP_WHY_MAX])
{
    return omx_clap_host_open(path, id, instance, why) == 0 && *instance != NULL;
}

static void check_refused(const char *path, const char *id, const char *code)
{
    struct omx_clap_instance *instance = NULL;
    char why[OMX_CLAP_WHY_MAX];
    int ret = omx_clap_host_open(path, id, &instance, why);

    CHECK(ret == -1 && instance == NULL && strcmp(why, code) == 0, "%s is refused with %s (%s)", id, code, why);
    if (instance)
        omx_clap_host_close(instance);
}

int main(int argc, char **argv)
{
    struct omx_clap_instance *instance = NULL;
    struct omx_clap_host_config config;
    float in_l[BLOCK], in_r[BLOCK], out_l[BLOCK], out_r[BLOCK];
    const float *inputs[2] = { in_l, in_r };
    float *outputs[2] = { out_l, out_r };
    char why[OMX_CLAP_WHY_MAX];
    double value;
    uint32_t b;

    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <fake.clap> <fake_synth.clap>\n", argv[0]);
        return 2;
    }

    CHECK(omx_clap_core_version() == 300, "the library says 0.3.0 (%u)", omx_clap_core_version());
    omx_clap_host_config_default(&config);
    CHECK(config.clamp && config.nonfinite && config.warmup && !config.note_inputs && !config.preset_load,
          "the defaults: clamp, scan and warm-up on, note inputs refused, no preset-load");
    CHECK(config.abi == OMX_CLAP_CORE_ABI && config.size == sizeof(config), "and they say which ABI and how large they are (%u, %u)", config.abi, config.size);
    config.abi++;
    CHECK(omx_clap_host_configure(&config) == -1, "a configuration made for another ABI is refused");
    config.abi--;
    config.size = 4;
    CHECK(omx_clap_host_configure(&config) == -1, "and one too short to name a host");

    // the defaults refuse what the isolated host admits
    check_refused(argv[1], FAKE_NOTES, CLAP_HOST_CODE_NOTE_INPUT);
    check_refused(argv[2], SYNTH, CLAP_HOST_CODE_NOT_AUDIO_EFFECT);
    check_refused(argv[1], FAKE_WIDE, CLAP_HOST_CODE_WIDER_THAN_STRIP);
    check_refused(argv[1], FAKE_SIDECHAIN, CLAP_HOST_CODE_EXTRA_INPUTS);
    CHECK(omx_clap_host_binaries_open() == 0, "nothing stays loaded after the refusals (%u open)", omx_clap_host_binaries_open());
    omx_clap_host_config_default(&config);
    CHECK(omx_clap_host_configure(&config) == -1, "and the process cannot be configured once a binary was opened");

    CHECK(open_ok(argv[1], FAKE_PASSTHROUGH, &instance, why), "open %s", FAKE_PASSTHROUGH);
    if (!instance)
        return report("core link test ok");
    CHECK(strcmp(instance->host.name, "omx-clap-core") == 0, "the plugin is told it is in %s", instance->host.name);
    CHECK(omx_clap_host_activate(instance, SAMPLE_RATE, BLOCK, why) == 0, "activate");
    CHECK(omx_clap_host_param_read(instance, FAKE_PARAM_ACTIVATIONS, &value) == 0 && value == 2.0, "the warm-up restarted the plugin: activated %g times", value);
    CHECK(omx_clap_host_param_read(instance, FAKE_PARAM_PROCESS_CALLS, &value) == 0 && value == CLAP_HOST_WARMUP_BLOCKS, "the warm-up ran %g blocks", value);
    CHECK(atomic_load(&instance->stage.h.runs) == 0 && atomic_load(&instance->stage.steady_time) == 0, "and left the live counters and the clock alone");
    CHECK(omx_clap_host_param_read(instance, FAKE_PARAM_HOST_PRESET, &value) == 0 && value == 0.0, "the preset-load host extension is not offered");
    omx_clap_host_publish(instance, pthread_self());

    // +40 dBFS is clamped to +24 dBFS and counted
    fill_tone(in_l, BLOCK, 0, 100.0f);
    memcpy(in_r, in_l, sizeof(in_l));
    for (b = 0; b < 3; b++)
        omx_clap_run_io(&instance->stage, inputs, outputs, BLOCK);
    CHECK(max_abs(out_l, BLOCK) <= omx_hosted_db_to_lin(CLAP_HOST_CLAMP_DBFS) * 1.0000001f && max_abs(out_l, BLOCK) > 15.0f && atomic_load(&instance->stage.h.clamped_samples) > 0,
          "the clamp: +40 dBFS leaves at %g (%u samples moved)", (double)max_abs(out_l, BLOCK), atomic_load(&instance->stage.h.clamped_samples));

    // a non-finite block is discarded (the lane passes dry) and, at the strike count, the stage is out for good
    fill_tone(in_l, BLOCK, 0, 0.5f);
    memcpy(in_r, in_l, sizeof(in_l));
    omx_clap_run_io(&instance->stage, inputs, outputs, BLOCK);
    in_l[3] = NAN;
    for (b = 0; b < CLAP_HOST_NON_FINITE_STRIKES; b++)
    {
        omx_clap_run_io(&instance->stage, inputs, outputs, BLOCK);
        CHECK(all_finite(out_l, BLOCK) == 0 || isnan(in_l[3]), "block %u of the non-finite input reaches the lane as it came (dry)", b);
    }
    CHECK(atomic_load(&instance->stage.h.nonfinite_blocks) == CLAP_HOST_NON_FINITE_STRIKES && atomic_load(&instance->stage.h.fault) == OMX_HOSTED_FAULT_NONFINITE,
          "the scan: %u strikes and the stage is faulted for good", atomic_load(&instance->stage.h.nonfinite_blocks));
    CHECK(atomic_load(&instance->thread_violations) == 0, "no thread violation");

    omx_clap_host_close(instance);
    CHECK(omx_clap_host_binaries_open() == 0, "closed");
    return report("core link test ok");
}
