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

/** @file
 * @brief Reading a plugin's org.openmixer.param-contract/1: the rules a host checks, the canonical fingerprint. */

/*
************************************************************************************************************************
*
* Internal to the library, never installed: clap_host.c reads a plugin's contract with it at the open, and
* tests/param_contract_test.c checks it on its own. Everything runs on the main thread, at the open; nothing here is
* reached from the audio thread.
*
* A plugin whose contract breaks one rule is uncontracted as a whole, never partly trusted, and the reason names the
* rule (omx_clap_ext.h states the extension, the canonical serialisation and the rules):
*   R1  every id clap.params lists has exactly one entry, the plugin's bypass included; no two entries share a symbol
*   R2  a symbol matches the grammar; the unit and the scale are known codes; a log scale has positive bounds; a stepped
*       scale has CLAP_PARAM_IS_STEPPED and step 1; a step is 0 or positive and finite; the bounds are finite
*   R4  the fingerprint the host computes from get_info and get is the one the plugin states
*   R5  the migration steps: by from_version ascending inside 1..version - 1; per version the from symbols and the
*       non-empty to symbols are unique; no step changes nothing; a value map keeps the symbol, the unit, factor 1 and
*       offset 0, its old and new values each unique, and on its last version lands on an enumerated parameter's range;
*       a removed symbol never comes back; the unit pair is one the conversion table holds
* R3 (the contract is the same after params.rescan) is the rescan's to check: pc_read again, then pc_same against what the
* open held.
*
************************************************************************************************************************
*/

#ifndef OMX_PARAM_CONTRACT_INTERNAL_H
#define OMX_PARAM_CONTRACT_INTERNAL_H


/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <clap/clap.h>

#include "omx_clap_ext.h"


/*
************************************************************************************************************************
*           SHA-256 (FIPS 180-4)
************************************************************************************************************************
*/

struct pc_sha256
{
    uint32_t h[8];
    uint64_t length;
    uint8_t block[64];
    uint32_t used;
};

static const uint32_t pc_sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define PC_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void pc_sha256_block(struct pc_sha256 *s, const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h, t1, t2;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (i = 16; i < 64; i++)
        w[i] = (PC_ROR(w[i - 2], 17) ^ PC_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10)) + w[i - 7]
             + (PC_ROR(w[i - 15], 7) ^ PC_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 16];
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (i = 0; i < 64; i++)
    {
        t1 = h + (PC_ROR(e, 6) ^ PC_ROR(e, 11) ^ PC_ROR(e, 25)) + ((e & f) ^ (~e & g)) + pc_sha256_k[i] + w[i];
        t2 = (PC_ROR(a, 2) ^ PC_ROR(a, 13) ^ PC_ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void pc_sha256_init(struct pc_sha256 *s)
{
    static const uint32_t h0[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };

    memcpy(s->h, h0, sizeof(h0));
    s->length = 0;
    s->used = 0;
}

static void pc_sha256_update(struct pc_sha256 *s, const void *data, size_t size)
{
    const uint8_t *p = data;

    s->length += size;
    while (size > 0)
    {
        const size_t n = size < 64 - s->used ? size : 64 - s->used;

        memcpy(s->block + s->used, p, n);
        s->used += (uint32_t)n;
        p += n;
        size -= n;
        if (s->used == 64)
        {
            pc_sha256_block(s, s->block);
            s->used = 0;
        }
    }
}

/* the digest as 64 lowercase hex characters and the NUL */
static void pc_sha256_hex(struct pc_sha256 *s, char hex[65])
{
    const uint64_t bits = s->length * 8;
    uint8_t pad = 0x80, len[8];
    int i;

    pc_sha256_update(s, &pad, 1);
    pad = 0;
    while (s->used != 56)
        pc_sha256_update(s, &pad, 1);
    for (i = 0; i < 8; i++)
        len[i] = (uint8_t)(bits >> (56 - 8 * i));
    pc_sha256_update(s, len, 8);
    for (i = 0; i < 32; i++)
        snprintf(hex + 2 * i, 3, "%02x", (s->h[i / 4] >> (24 - 8 * (i % 4))) & 0xff);
}


/*
************************************************************************************************************************
*           NUMBERS AND NAMES
************************************************************************************************************************
*/

/* `value` as the shortest decimal that reads back to the same double, in ECMAScript's Number.prototype.toString form
 * (what the canonical serialisation writes, and node's String(n)), -0 written 0: 0 and the length written, or -1 for
 * a value that is not finite or does not fit */
static int pc_number(double value, char *out, size_t size)
{
    char e[40], digits[20], text[48];
    int precision, k = 0, n, i, used = 0;
    const char *p;

    if (!isfinite(value) || size == 0)
        return -1;
    if (value == 0)
        return snprintf(out, size, "0") < (int)size ? 1 : -1;
    for (precision = 1; precision <= 17; precision++)
    {
        snprintf(e, sizeof(e), "%.*e", precision - 1, value);
        if (strtod(e, NULL) == value)
            break;
    }
    // e is [-]d[.ddd]e(+|-)xx: its k digits, trailing zeros dropped, are d1d2...dk and the value is 0.d1d2...dk x 10^n
    for (p = e[0] == '-' ? e + 1 : e; *p && *p != 'e'; p++)
        if (*p != '.')
            digits[k++] = *p;
    while (k > 1 && digits[k - 1] == '0')
        k--;
    n = atoi(p + 1) + 1;

    if (value < 0)
        text[used++] = '-';
    if (k <= n && n <= 21)
    {
        // the digits and n - k zeros
        for (i = 0; i < n; i++)
            text[used++] = i < k ? digits[i] : '0';
    }
    else if (0 < n && n <= 21)
    {
        // the point after the n-th digit
        for (i = 0; i < k; i++)
        {
            if (i == n)
                text[used++] = '.';
            text[used++] = digits[i];
        }
    }
    else if (-6 < n && n <= 0)
    {
        // 0. then -n zeros then the digits
        text[used++] = '0';
        text[used++] = '.';
        for (i = 0; i < -n; i++)
            text[used++] = '0';
        for (i = 0; i < k; i++)
            text[used++] = digits[i];
    }
    else
    {
        // d[.ddd]e+x or e-x
        text[used++] = digits[0];
        if (k > 1)
        {
            text[used++] = '.';
            for (i = 1; i < k; i++)
                text[used++] = digits[i];
        }
        used += snprintf(text + used, sizeof(text) - used, "e%c%d", n - 1 < 0 ? '-' : '+', abs(n - 1));
    }
    text[used] = '\0';
    if ((size_t)used >= size)
        return -1;
    memcpy(out, text, (size_t)used + 1);
    return used;
}

static const char *pc_unit_name(uint32_t unit)
{
#define PC_UNIT_CASE(code, name) case code: return name;
    switch (unit)
    {
        OMX_CLAP_PARAM_UNITS(PC_UNIT_CASE)
        default: return NULL;
    }
#undef PC_UNIT_CASE
}

static const char *pc_scale_name(uint32_t scale)
{
#define PC_SCALE_CASE(code, name) case code: return name;
    switch (scale)
    {
        OMX_CLAP_PARAM_SCALES(PC_SCALE_CASE)
        default: return NULL;
    }
#undef PC_SCALE_CASE
}

/* [A-Za-z_][A-Za-z0-9_]*, terminated inside its buffer */
static int pc_symbol_ok(const char symbol[OMX_CLAP_PARAM_CONTRACT_SYMBOL_SIZE])
{
    size_t i;

    if (!memchr(symbol, '\0', OMX_CLAP_PARAM_CONTRACT_SYMBOL_SIZE))
        return 0;
    if (!((symbol[0] >= 'A' && symbol[0] <= 'Z') || (symbol[0] >= 'a' && symbol[0] <= 'z') || symbol[0] == '_'))
        return 0;
    for (i = 1; symbol[i]; i++)
        if (!((symbol[i] >= 'A' && symbol[i] <= 'Z') || (symbol[i] >= 'a' && symbol[i] <= 'z')
              || (symbol[i] >= '0' && symbol[i] <= '9') || symbol[i] == '_'))
            return 0;
    return 1;
}

/* the conversion table: a unit pair a migration step may take, identity included. The affine families are LV2's own
 * units:conversion pairs (time, length, frequency with bpm, pc and coef, pitch); db and coef are the one non-affine
 * pair. The vendor units convert to nothing but themselves. */
static int pc_unit_family(uint32_t unit)
{
    switch (unit)
    {
        case 1: case 2: case 3: return 1;  // s ms min
        case 7: case 8: case 9: case 10: case 11: case 12: return 2;  // m cm mm km inch mile
        case 16: case 17: case 18: case 19: return 3;  // hz khz mhz bpm
        case 20: case 21: case 22: return 4;  // oct cent semitone12TET
        default: return 0;
    }
}

static int pc_convertible(uint32_t from, uint32_t to)
{
    const int family = pc_unit_family(from);

    if (from == to)
        return 1;
    if (family && family == pc_unit_family(to))
        return 1;
    // pc and coef (a factor of 100), db and coef (amplitude)
    return (from == 14 && to == 15) || (from == 15 && to == 14) || (from == 13 && to == 15) || (from == 15 && to == 13);
}


/*
************************************************************************************************************************
*           THE CONTRACT
************************************************************************************************************************
*/

/* the contract of one instance as the open read it: entries by clap.params index, the version, or why there is none.
 * The entries are one allocation whose tail, after the count of them, holds the fingerprint the host computed (64 hex
 * digits and a NUL): what a rescan's contract is held to (R3), kept with the entries so the instance needs no field of
 * its own for it. */
#define PC_FINGERPRINT_SIZE 65

struct pc_contract
{
    omx_clap_param_contract_entry_t *entry;  // count of them, by the params index, then the fingerprint; NULL when uncontracted
    uint32_t count;
    uint32_t version;
    char why[128];  // "" when contracted; "absent" with no extension; else the rule and what broke it
};

struct pc_param
{
    clap_param_info_t info;
    omx_clap_param_contract_entry_t entry;
};

static int pc_by_id(const void *a, const void *b)
{
    const clap_id x = ((const struct pc_param *)a)->info.id, y = ((const struct pc_param *)b)->info.id;

    return x < y ? -1 : x > y;
}

static void pc_append(char *buffer, size_t size, size_t *used, const char *text, size_t most)
{
    while (*text && most-- > 0 && *used + 1 < size)
        buffer[(*used)++] = *text++;
    buffer[*used] = '\0';
}

/* the reason: the rule, what broke it and the symbol it broke on, cut to the buffer */
static int pc_refuse(struct pc_contract *c, const char *rule, const char *what, const char *symbol)
{
    size_t used = 0;

    pc_append(c->why, sizeof(c->why), &used, rule, SIZE_MAX);
    if (*what)
    {
        pc_append(c->why, sizeof(c->why), &used, " ", 1);
        pc_append(c->why, sizeof(c->why), &used, what, SIZE_MAX);
    }
    if (symbol)
    {
        pc_append(c->why, sizeof(c->why), &used, " ", 1);
        pc_append(c->why, sizeof(c->why), &used, symbol, OMX_CLAP_PARAM_CONTRACT_SYMBOL_SIZE - 1);
    }
    return -1;
}

/* the line of one parameter in the canonical serialisation; -1 for a number that has no form */
static int pc_line(const struct pc_param *p, char *line, size_t size)
{
    char min[32], max[32], def[32], step[32];

    if (pc_number(p->info.min_value, min, sizeof(min)) < 0 || pc_number(p->info.max_value, max, sizeof(max)) < 0
        || pc_number(p->info.default_value, def, sizeof(def)) < 0 || pc_number(p->entry.step, step, sizeof(step)) < 0)
        return -1;
    return snprintf(line, size, "%u\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%x\n", p->info.id, p->entry.symbol,
                    pc_unit_name(p->entry.unit), pc_scale_name(p->entry.scale), min, max, def, step,
                    p->info.flags & (CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_PERIODIC | CLAP_PARAM_IS_READONLY
                                     | CLAP_PARAM_IS_BYPASS | CLAP_PARAM_IS_ENUM));
}

/* the fingerprint of `n` parameters sorted by id: the extension id line, then one line each */
static int pc_fingerprint(const struct pc_param *p, uint32_t n, char hex[65])
{
    struct pc_sha256 s;
    char line[512];
    uint32_t i;

    pc_sha256_init(&s);
    pc_sha256_update(&s, OMX_CLAP_EXT_PARAM_CONTRACT, strlen(OMX_CLAP_EXT_PARAM_CONTRACT));
    pc_sha256_update(&s, "\n", 1);
    for (i = 0; i < n; i++)
    {
        const int len = pc_line(&p[i], line, sizeof(line));

        if (len < 0 || (size_t)len >= sizeof(line))
            return -1;
        pc_sha256_update(&s, line, (size_t)len);
    }
    pc_sha256_hex(&s, hex);
    return 0;
}

/* R5: the migration steps, against the live parameters `p` (sorted by id) */
static int pc_migrations(struct pc_contract *c, const clap_plugin_t *plugin, const omx_clap_plugin_param_contract_ext_t *ext,
                         const struct pc_param *p, uint32_t n)
{
    const uint32_t count = ext->migration_count ? ext->migration_count(plugin) : 0;
    omx_clap_param_contract_step_t *steps;
    uint32_t i, j, k;
    int ret = 0;

    if (count == 0)
        return 0;
    if (!ext->get_migration)
        return pc_refuse(c, "R5", "migration steps counted but no get_migration", NULL);
    if (count > 4096)
        return pc_refuse(c, "R5", "more migration steps than the host reads", NULL);
    steps = calloc(count, sizeof(*steps));
    if (!steps)
        return pc_refuse(c, "R5", "no memory for the migration steps", NULL);
    for (i = 0; i < count && ret == 0; i++)
    {
        omx_clap_param_contract_step_t *s = &steps[i];

        if (!ext->get_migration(plugin, i, s))
            ret = pc_refuse(c, "R5", "a counted migration step is missing", NULL);
        else if (!memchr(s->from_symbol, '\0', sizeof(s->from_symbol)) || !memchr(s->to_symbol, '\0', sizeof(s->to_symbol))
                 || !pc_symbol_ok(s->from_symbol) || (s->to_symbol[0] && !pc_symbol_ok(s->to_symbol)))
            ret = pc_refuse(c, "R5", "a migration symbol breaks the grammar", NULL);
        else if (s->from_version < 1 || s->from_version >= c->version || (i > 0 && s->from_version < steps[i - 1].from_version))
            ret = pc_refuse(c, "R5", "a step's from_version is out of order or out of 1..version-1:", s->from_symbol);
        else if (!pc_unit_name(s->from_unit) || !pc_unit_name(s->to_unit) || !pc_convertible(s->from_unit, s->to_unit))
            ret = pc_refuse(c, "R5", "a unit pair the conversion table does not hold:", s->from_symbol);
        else if (!isfinite(s->factor) || !isfinite(s->offset))
            ret = pc_refuse(c, "R5", "a factor or offset that is not finite:", s->from_symbol);
        else if (s->to_symbol[0] && !strcmp(s->from_symbol, s->to_symbol) && s->from_unit == s->to_unit && s->factor == 1
                 && s->offset == 0 && s->value_map_count == 0)
            ret = pc_refuse(c, "R5", "a step that changes nothing:", s->from_symbol);
        else if (s->value_map_count > 0)
        {
            if (!s->value_map || strcmp(s->from_symbol, s->to_symbol) || s->from_unit != s->to_unit || s->factor != 1
                || s->offset != 0)
                ret = pc_refuse(c, "R5", "a value map that is not a plain reorder:", s->from_symbol);
            for (j = 0; j < s->value_map_count && ret == 0; j++)
                for (k = j + 1; k < s->value_map_count && ret == 0; k++)
                    if (s->value_map[2 * j] == s->value_map[2 * k] || s->value_map[2 * j + 1] == s->value_map[2 * k + 1])
                        ret = pc_refuse(c, "R5", "a value map whose values repeat:", s->from_symbol);
            if (ret == 0 && s->from_version + 1 == c->version)
            {
                const struct pc_param *live = NULL;

                for (j = 0; j < n; j++)
                    if (!strcmp(p[j].entry.symbol, s->to_symbol))
                        live = &p[j];
                if (!live || !(live->info.flags & CLAP_PARAM_IS_ENUM))
                    ret = pc_refuse(c, "R5", "a value map on a parameter that is not enumerated:", s->from_symbol);
                for (j = 0; j < s->value_map_count && ret == 0; j++)
                    if (s->value_map[2 * j + 1] < live->info.min_value || s->value_map[2 * j + 1] > live->info.max_value)
                        ret = pc_refuse(c, "R5", "a value map lands outside the range:", s->from_symbol);
            }
        }
    }
    // per version: from symbols unique, non-empty to symbols unique; a removed symbol never comes back
    for (i = 0; i < count && ret == 0; i++)
    {
        for (j = i + 1; j < count && ret == 0 && steps[j].from_version == steps[i].from_version; j++)
            if (!strcmp(steps[i].from_symbol, steps[j].from_symbol)
                || (steps[i].to_symbol[0] && !strcmp(steps[i].to_symbol, steps[j].to_symbol)))
                ret = pc_refuse(c, "R5", "two steps of one version share a symbol:", steps[i].from_symbol);
        if (ret == 0 && steps[i].to_symbol[0] == '\0')
        {
            for (j = 0; j < count && ret == 0; j++)
                if (steps[j].from_version > steps[i].from_version && !strcmp(steps[j].to_symbol, steps[i].from_symbol))
                    ret = pc_refuse(c, "R5", "a removed symbol comes back:", steps[i].from_symbol);
            for (j = 0; j < n && ret == 0; j++)
                if (!strcmp(p[j].entry.symbol, steps[i].from_symbol))
                    ret = pc_refuse(c, "R5", "a removed symbol is live again:", steps[i].from_symbol);
        }
    }
    free(steps);
    return ret;
}

/* Read and check the contract of `plugin`, whose clap.params is `params` and lists `count` parameters: 0 and `c`
 * holds the entries by params index; -1 and `c->why` says why it is uncontracted (`c->entry` NULL). */
static int pc_read(struct pc_contract *c, const clap_plugin_t *plugin, const clap_plugin_params_t *params, uint32_t count)
{
    const omx_clap_plugin_param_contract_ext_t *ext = plugin->get_extension
        ? plugin->get_extension(plugin, OMX_CLAP_EXT_PARAM_CONTRACT) : NULL;
    struct pc_param *p = NULL;
    const char *stated;
    char hex[65];
    uint32_t i;
    int ret = -1;

    memset(c, 0, sizeof(*c));
    if (!ext)
        return pc_refuse(c, "absent", "", NULL);
    if (!ext->version || !ext->fingerprint || !ext->get || !params || count == 0)
        return pc_refuse(c, "R1", "the extension or clap.params is incomplete", NULL);
    c->version = ext->version(plugin);
    if (c->version < 1)
        return pc_refuse(c, "R5", "version 0", NULL);
    p = calloc(count, sizeof(*p));
    if (!p)
        return pc_refuse(c, "R1", "no memory for the entries", NULL);
    for (i = 0; i < count; i++)
    {
        struct pc_param *q = &p[i];

        if (!params->get_info(plugin, i, &q->info))
        {
            pc_refuse(c, "R1", "a parameter clap.params counts has no info", NULL);
            goto out;
        }
        if (!ext->get(plugin, q->info.id, &q->entry) || q->entry.param_id != q->info.id)
        {
            char what[64];

            snprintf(what, sizeof(what), "no entry for parameter %u", q->info.id);
            pc_refuse(c, "R1", what, NULL);
            goto out;
        }
        if (!pc_symbol_ok(q->entry.symbol))
        {
            pc_refuse(c, "R2", "a symbol breaks the grammar:", memchr(q->entry.symbol, '\0', sizeof(q->entry.symbol)) ? q->entry.symbol : NULL);
            goto out;
        }
        if (!pc_unit_name(q->entry.unit) || !pc_scale_name(q->entry.scale))
        {
            pc_refuse(c, "R2", "an unknown unit or scale:", q->entry.symbol);
            goto out;
        }
        if (!isfinite(q->info.min_value) || !isfinite(q->info.max_value) || !isfinite(q->info.default_value)
            || !isfinite(q->entry.step) || q->entry.step < 0)
        {
            pc_refuse(c, "R2", "a bound or a step that is not finite, or a negative step:", q->entry.symbol);
            goto out;
        }
        if (q->entry.scale == 1 && !(q->info.min_value > 0 && q->info.max_value > 0))
        {
            pc_refuse(c, "R2", "a log scale over a travel that is not positive:", q->entry.symbol);
            goto out;
        }
        if (q->entry.scale == 2 && (!(q->info.flags & CLAP_PARAM_IS_STEPPED) || q->entry.step != 1))
        {
            pc_refuse(c, "R2", "a stepped scale without CLAP_PARAM_IS_STEPPED and step 1:", q->entry.symbol);
            goto out;
        }
    }
    qsort(p, count, sizeof(*p), pc_by_id);
    for (i = 1; i < count; i++)
        if (p[i].info.id == p[i - 1].info.id)
        {
            pc_refuse(c, "R1", "two parameters share an id", NULL);
            goto out;
        }
    for (i = 0; i < count; i++)
    {
        uint32_t j;

        for (j = i + 1; j < count; j++)
            if (!strcmp(p[i].entry.symbol, p[j].entry.symbol))
            {
                pc_refuse(c, "R1", "two entries share the symbol", p[i].entry.symbol);
                goto out;
            }
    }
    stated = ext->fingerprint(plugin);
    if (pc_fingerprint(p, count, hex) != 0 || !stated || strncmp(stated, hex, sizeof(hex)) != 0)
    {
        snprintf(c->why, sizeof(c->why), "R4 the fingerprint is %s", hex);
        goto out;
    }
    if (pc_migrations(c, plugin, ext, p, count) != 0)
        goto out;
    c->entry = calloc(1, count * sizeof(*c->entry) + PC_FINGERPRINT_SIZE);
    if (!c->entry)
    {
        pc_refuse(c, "R1", "no memory for the entries", NULL);
        goto out;
    }
    // back in the params index order, which is how the instance asks
    for (i = 0; i < count; i++)
    {
        clap_param_info_t info;
        uint32_t j;

        params->get_info(plugin, i, &info);
        for (j = 0; j < count; j++)
            if (p[j].info.id == info.id)
                c->entry[i] = p[j].entry;
    }
    memcpy(c->entry + count, hex, PC_FINGERPRINT_SIZE);
    c->count = count;
    c->why[0] = '\0';
    ret = 0;
out:
    free(p);
    return ret;
}

/* the fingerprint the host computed when `entry` (`count` of them, from pc_read) was read */
static const char *pc_fingerprint_of(const omx_clap_param_contract_entry_t *entry, uint32_t count)
{
    return (const char *)(entry + count);
}

/* R3: whether `now`, a contract read after params.rescan, is the one `entry` (`count` of them, at `version`) held: the
 * same version and the same fingerprint, which serialises every id, symbol, unit, scale, bound, step and flag. The name
 * and the module are display text, free to change. */
static int pc_same(const struct pc_contract *now, const omx_clap_param_contract_entry_t *entry, uint32_t count, uint32_t version)
{
    return now->entry && entry && now->version == version && now->count == count
           && strcmp(pc_fingerprint_of(now->entry, now->count), pc_fingerprint_of(entry, count)) == 0;
}

/* the entry of `id`, or NULL for an uncontracted instance or an id the contract does not hold */
static const omx_clap_param_contract_entry_t *pc_entry(const struct pc_contract *c, clap_id id)
{
    uint32_t i;

    for (i = 0; c->entry && i < c->count; i++)
        if (c->entry[i].param_id == id)
            return &c->entry[i];
    return NULL;
}


/*
************************************************************************************************************************
*           END HEADER
************************************************************************************************************************
*/

#endif
