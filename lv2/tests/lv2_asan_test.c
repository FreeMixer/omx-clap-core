/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * This file is part of omx-clap-host.
 *
 * lv2_asan_test: the MIDI output path of lv2/core built with AddressSanitizer. The echo fixture declares an output sequence
 * whose size is far past the buffer it was given, with zeros inside the buffer. The adapter must read only its own buffer:
 * the run passes nothing, and any read past the buffer is an AddressSanitizer report, which ends the process.
 *
 *   lv2_asan_test <build dir holding omx-midi-echo.lv2 built with AddressSanitizer>
 */

#include <dlfcn.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lv2_core.h"

#define ECHO_URI "urn:openmixer:test:midi-echo"
#define BLOCK 128u
#define EXTRA_OVERSIZE 5

static void count_out(void *ctx, int64_t frame, const uint8_t *data, uint32_t size)
{
    (void)frame;
    (void)data;
    (void)size;
    (*(uint32_t *)ctx)++;
}

int main(int argc, char **argv)
{
    static const struct lv2_core_config config = { lv2_core_provided, 1000u, 4096u, 0u, NULL };
    char dir[PATH_MAX], path[PATH_MAX + 64], why[LV2_CORE_WHY_MAX] = "";
    void *so;
    void (*extra)(int);
    struct lv2_bundle *b;
    struct lv2_plugin *p = NULL;
    struct lv2_instance *in = NULL;
    uint32_t out = 0;
    int failures = 0;

    if (argc < 2)
    {
        printf("FAIL usage: lv2_asan_test <build dir>\n");
        return 1;
    }
    if (lv2_core_configure(&config, why) != 0)
    {
        printf("FAIL configure (%s)\n", why);
        return 1;
    }
    if (!realpath(argv[1], dir))
    {
        printf("FAIL no build dir %s\n", argv[1]);
        return 1;
    }
    setenv("LV2_PATH", dir, 1);     // what the host finds: the bundles are there
    snprintf(path, sizeof(path), "%s/omx-midi-echo.lv2", dir);
    b = lv2_bundle_ref(path, why);
    p = b ? lv2_plugin_open(b, ECHO_URI, why) : NULL;
    lv2_bundle_unref(b);
    if (!p || lv2_plugin_load(p, why) != 0 || !(in = lv2_instance_new(p)))
    {
        printf("FAIL the echo does not open (%s)\n", why);
        return 1;
    }
    if (lv2_instance_activate(in, 48000.0, 1, BLOCK, why) != 0)
    {
        printf("FAIL the echo does not activate (%s)\n", why);
        lv2_plugin_close(p);
        return 1;
    }

    // the fixture's own symbols: the same object lilv loaded, found again by its path
    snprintf(path, sizeof(path), "%s/omx-midi-echo.lv2/omx-midi-echo.so", dir);
    so = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    *(void **)&extra = so ? dlsym(so, "lv2_midi_echo_extra") : NULL;
    if (!extra)
    {
        printf("FAIL the fixture's lv2_midi_echo_extra is not found (%s)\n", path);
        return 1;
    }

    extra(EXTRA_OVERSIZE);
    lv2_instance_run(in, BLOCK, NULL, NULL);
    lv2_instance_midi_out_each(in, count_out, &out);
    if (out == 0)
        printf("ok   an output sequence declared past its buffer passes nothing\n");
    else
    {
        printf("FAIL an output sequence declared past its buffer passed %u event(s)\n", out);
        failures++;
    }
    extra(0);

    lv2_instance_deactivate(in);
    lv2_instance_free(in);
    lv2_plugin_close(p);
    printf("%s\n", failures == 0 ? "lv2 asan test ok" : "lv2 asan test FAILED");
    return failures == 0 ? 0 : 1;
}
