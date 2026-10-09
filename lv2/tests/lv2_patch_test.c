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
 * lv2/core's patch ports against omx-patch-fixture.lv2: the numeric patch:writable properties as patch parameters,
 * the patch:Set forged before a block's run, and the patch:Set and patch:Put read back from the output after it.
 *
 *   lv2_patch_test <build/lv2 directory>
 */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "lv2_core.h"
#include "lv2_test_util.h"

#define BLOCK 64u
#define FX_URI "urn:openmixer:test:patch-fixture"
#define FX_PREFIX FX_URI "#"

static char g_dir[PATH_MAX], g_build[PATH_MAX];

/* the parameter of a symbol, or its index, in the plugin's order */
static const struct lv2_patch *patch_of(const struct lv2_plugin *p, const char *symbol)
{
    uint32_t j;

    for (j = 0; j < p->n_patches; j++)
        if (strcmp(p->patches[j].symbol, symbol) == 0)
            return &p->patches[j];
    return NULL;
}

static int index_of(const struct lv2_plugin *p, const char *symbol)
{
    uint32_t j;

    for (j = 0; j < p->n_patches; j++)
        if (strcmp(p->patches[j].symbol, symbol) == 0)
            return (int)j;
    return -1;
}

/* the plugin read and its binary loaded, as the CLAP shim's init does before an instance is made */
static struct lv2_plugin *open_fx(const char *uri, char why[LV2_CORE_WHY_MAX])
{
    struct lv2_bundle *b = lv2_bundle_ref(g_dir, why);
    struct lv2_plugin *p = b ? lv2_plugin_open(b, uri, why) : NULL;

    lv2_bundle_unref(b);
    if (p && lv2_plugin_load(p, why) != 0)
    {
        lv2_plugin_close(p);
        return NULL;
    }
    return p;
}

/* one block: the input copied in, the caller's writes already made, the output copied out */
static void run_block(struct lv2_instance *in, const float *x, float *y)
{
    memcpy(lv2_instance_audio_in(in, 0), x, BLOCK * sizeof(float));
    lv2_instance_run(in, BLOCK, NULL, NULL);
    memcpy(y, lv2_instance_audio_out(in, 0), BLOCK * sizeof(float));
}

/* whether the last run echoed `symbol`, and its value when it did */
static int echoed(const struct lv2_plugin *p, struct lv2_instance *in, const char *symbol, float *value)
{
    const int j = index_of(p, symbol);

    return j >= 0 && lv2_instance_patch_changed(in, (uint32_t)j, value);
}

static float ramp_in[BLOCK], ramp_out[BLOCK], scratch[BLOCK];

/* ---- structure ---- */

static void t_structure(void)
{
    char why[LV2_CORE_WHY_MAX] = "";
    struct lv2_plugin *p = open_fx(FX_PREFIX "midi", why);
    uint32_t j, k;
    const struct lv2_patch *g;

    CHECK(!p && strcmp(why, "hosting.features.midi-in-fed-empty") == 0,
          "patch: a patch port that also takes MIDI is refused with the MIDI code (%s)", why);
    p = open_fx(FX_URI, why);
    CHECK(p != NULL, "patch: the fixture opens and loads (%s)", why);
    if (!p)
        return;
    CHECK(p->patch_in == 2 && p->patch_out == 3, "patch: the patch input is port 2 and the patch output port 3 (%d, %d)", p->patch_in, p->patch_out);
    CHECK(p->n_patches == 4, "patch: four numeric patch:writable properties; the string one is no parameter (%u)", p->n_patches);
    for (j = 0; j < p->n_patches; j++)
        for (k = 0; k < j; k++)
            CHECK(p->patches[j].id != p->patches[k].id, "patch: the ids are distinct (%u, %u)", p->patches[j].id, p->patches[k].id);
    for (j = 0; j < p->n_patches; j++)
        CHECK(p->patches[j].id >= p->n_ports, "patch: %s id %u is above the %u port indices", p->patches[j].symbol, p->patches[j].id, p->n_ports);
    CHECK(patch_of(p, "name") == NULL, "patch: the string property has no parameter");
    g = patch_of(p, "gain");
    CHECK(g && g->type == LV2_PATCH_FLOAT && g->min == 0.0f && g->max == 2.0f && g->def == 1.0f && g->props == 0 && strcmp(g->name, "Gain") == 0,
          "patch: gain is a float over 0..2, default 1, named by its label");
    g = patch_of(p, "mode");
    CHECK(g && g->type == LV2_PATCH_INT && g->min == 0.0f && g->max == 3.0f && g->def == 0.0f && g->props == LV2_PROP_INTEGER,
          "patch: mode is an integer over 0..3, default 0, stepped");
    g = patch_of(p, "flag");
    CHECK(g && g->type == LV2_PATCH_BOOL && g->min == 0.0f && g->max == 1.0f && g->props == LV2_PROP_TOGGLED,
          "patch: flag is a boolean, toggled over 0..1");
    g = patch_of(p, "level");
    CHECK(g && g->type == LV2_PATCH_FLOAT && g->min == -1e9f && g->max == 1e9f && g->def == g->min && strcmp(g->name, "level") == 0,
          "patch: level has no lv2:symbol, so its symbol is its URI's fragment; no bounds give -1e9..1e9, default the minimum");
    lv2_plugin_close(p);
}

/* ---- the block: the defaults are sent once, the echo is read back, the audio follows the gain ---- */

static void t_block(void)
{
    char why[LV2_CORE_WHY_MAX] = "";
    struct lv2_plugin *p = open_fx(FX_URI, why);
    struct lv2_instance *in = p ? lv2_instance_new(p) : NULL;
    float v = 0.0f;
    uint32_t i;
    int gain_j = p ? index_of(p, "gain") : -1;

    CHECK(in != NULL && lv2_instance_activate(in, 48000.0, 1, BLOCK, why) == 0, "patch: an instance activates (%s)", why);
    if (!in)
        return;
    for (i = 0; i < BLOCK; i++)
        ramp_in[i] = 0.25f * sinf(0.3f * (float)i);
    run_block(in, ramp_in, ramp_out);
    CHECK(bits_equal(ramp_out, ramp_in, BLOCK), "patch: the first block carries the defaults, so the gain is 1 and the audio passes");
    CHECK(echoed(p, in, "gain", &v) && v == 1.0f, "patch: the plugin received gain 1 and echoed it as patch:Set (%g)", v);
    CHECK(echoed(p, in, "mode", &v) && v == 0.0f, "patch: the plugin received mode 0 and echoed it as patch:Put (%g)", v);
    CHECK(echoed(p, in, "flag", &v) && v == 0.0f, "patch: the plugin received flag 0 and echoed it as a boolean (%g)", v);
    CHECK(echoed(p, in, "level", &v) && v == -1e9f, "patch: level, at its default, was received and echoed (%g)", v);
    run_block(in, ramp_in, ramp_out);
    CHECK(!echoed(p, in, "gain", &v) && !echoed(p, in, "mode", &v) && !echoed(p, in, "level", &v),
          "patch: a block without writes carries no patch:Set, so nothing is echoed");

    lv2_instance_patch_write(in, (uint32_t)gain_j, 0.5f);
    CHECK(lv2_instance_patch_get(in, (uint32_t)gain_j) == 0.5f, "patch: a write is the value the host reads back at once");
    run_block(in, ramp_in, ramp_out);
    for (i = 0; i < BLOCK; i++)
        scratch[i] = ramp_in[i] * 0.5f;
    CHECK(bits_equal(ramp_out, scratch, BLOCK), "patch: gain 0.5 written before the block scales the audio exactly");
    CHECK(echoed(p, in, "gain", &v) && v == 0.5f, "patch: the echo of gain 0.5 reads back from the output (%g)", v);

    lv2_instance_patch_write(in, (uint32_t)index_of(p, "mode"), 2.0f);
    lv2_instance_patch_write(in, (uint32_t)index_of(p, "flag"), 1.0f);
    lv2_instance_patch_write(in, (uint32_t)index_of(p, "level"), -3.25f);
    run_block(in, ramp_in, ramp_out);
    CHECK(echoed(p, in, "mode", &v) && v == 2.0f, "patch: an integer written is received as an integer (%g)", v);
    CHECK(echoed(p, in, "flag", &v) && v == 1.0f, "patch: a boolean written is received as a boolean (%g)", v);
    CHECK(echoed(p, in, "level", &v) && v == -3.25f, "patch: a float written is received exactly (%g)", v);
    CHECK(bits_equal(ramp_out, scratch, BLOCK), "patch: the mode and the flag do not change the audio");
    CHECK(lv2_instance_patch_get(in, (uint32_t)index_of(p, "mode")) == 2.0f, "patch: the value read after the echo is the one written");

    /* a new handle (a rate change) receives the last values again, as the first block did */
    CHECK(lv2_instance_activate(in, 96000.0, 1, BLOCK, why) == 0, "patch: the instance re-activates at a new rate (%s)", why);
    run_block(in, ramp_in, ramp_out);
    CHECK(echoed(p, in, "gain", &v) && v == 0.5f, "patch: a new handle is sent the last gain again (%g)", v);
    CHECK(echoed(p, in, "level", &v) && v == -3.25f, "patch: and the last level");
    CHECK(bits_equal(ramp_out, scratch, BLOCK), "patch: the new handle runs at the gain it was sent");

    CHECK(lv2_instance_patch_changed(in, 99u, &v) == 0 && lv2_instance_patch_get(in, 99u) == 0.0f,
          "patch: an index past the parameters is no parameter");
    {
        struct lv2_counters c;

        lv2_instance_counters(in, &c);
        CHECK(c.patch_dropped == 0, "patch: a block that fits drops nothing (%u)", c.patch_dropped);
    }
    lv2_instance_free(in);
    lv2_plugin_close(p);
}

/* ---- the atom buffer of one event: the rest of a block's events are dropped and counted ---- */

static void t_drops(void)
{
    static const struct lv2_core_config small = { lv2_core_provided, 1000u, 4096u, 0u, NULL };
    char why[LV2_CORE_WHY_MAX] = "";
    struct lv2_plugin *p;
    struct lv2_instance *in;
    struct lv2_counters c;
    uint32_t j;
    int echoes = 0;
    float v;

    CHECK(lv2_core_configure(&small, why) == 0, "patch drop: configured with the smallest atom buffer (%s)", why);
    p = open_fx(FX_URI, why);
    in = p ? lv2_instance_new(p) : NULL;
    CHECK(in != NULL && lv2_instance_activate(in, 48000.0, 1, BLOCK, why) == 0, "patch drop: an instance activates (%s)", why);
    if (!in)
        return;
    run_block(in, ramp_in, ramp_out);
    lv2_instance_counters(in, &c);
    CHECK(c.patch_dropped == p->n_patches - 1u, "patch drop: the first block fits one event and drops the other %u, counted (%u)", p->n_patches - 1u, c.patch_dropped);
    for (j = 0; j < p->n_patches; j++)
        echoes += echoed(p, in, p->patches[j].symbol, &v);
    CHECK(echoes == 1, "patch drop: exactly one event reached the plugin (%d)", echoes);
    lv2_instance_free(in);
    lv2_plugin_close(p);
}

int main(int argc, char **argv)
{
    static const struct lv2_core_config config = { lv2_core_provided, 1000u, 4096u, 4096u, NULL };
    char dir[PATH_MAX + 32], why[LV2_CORE_WHY_MAX];

    if (argc < 2 || abs_dir(argv[1], g_build) != 0)
    {
        printf("FAIL usage: lv2_patch_test <build/lv2>\n");
        return 1;
    }
    snprintf(dir, sizeof(dir), "%somx-patch-fixture.lv2", g_build);
    if (abs_dir(dir, g_dir) != 0)
        return 1;
    setenv("LV2_PATH", g_build, 1);

    in_child("drops", t_drops);     /* first: it configures the smallest atom buffer, which this process must not */
    CHECK(lv2_core_configure(&config, why) == 0, "configure every provider (%s)", why);
    t_structure();
    t_block();
    printf("%s\n", g_failures == 0 ? "lv2 patch test ok" : "lv2 patch test FAILED");
    return g_failures == 0 ? 0 : 1;
}
