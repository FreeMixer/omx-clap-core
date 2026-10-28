/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * This file is part of omx-clap-host.
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * src/preset_file.c on its own: a document written and read back is the same tree, a value reads back to the same
 * double, unknown members survive a rewrite, base64 and the load keys go both ways, and what is not JSON is refused.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "preset_file.h"

static int failures;

#define CHECK(cond, what) do { if (cond) printf("ok   %s\n", what); else { printf("FAIL %s\n", what); failures++; } } while (0)

static int parses(const char *text)
{
    struct pj v;
    const int rc = pj_parse(text, strlen(text), &v);

    pj_free(&v);
    return rc == 0;
}

/* 2^31 '=', more than an int counts, in one MiB: the same MiB mapped over and over, then a page of zeros. 1 when the
 * decoder refuses it, 0 when it reads it, -1 when the address space cannot hold the text and the decoder's buffer */
static int pads_past_int_refused(void)
{
    const size_t tile = (size_t)1 << 20, run = (size_t)1 << 31, size = run + (size_t)sysconf(_SC_PAGESIZE);
    const int fd = memfd_create("pads", MFD_CLOEXEC);
    char *text = mmap(NULL, size, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    uint8_t *dec;
    size_t i, len;
    int refused = -1;

    if (fd < 0 || ftruncate(fd, (off_t)tile) != 0 || text == MAP_FAILED)
        goto out;
    for (i = 0; i < run; i += tile)
        if (mmap(text + i, tile, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED)
            goto out;
    memset(text, '=', tile);
    // ENOMEM: the decoder's own malloc failed, which refuses the text whatever it counts
    errno = 0;
    dec = preset_base64_decode(text, &len);
    refused = dec ? 0 : errno == ENOMEM ? -1 : 1;
    free(dec);
out:
    if (text != MAP_FAILED)
        munmap(text, size);
    if (fd >= 0)
        close(fd);
    return refused;
}

int main(void)
{
    static const double values[] = { 0.1, 1.0 / 3.0, -2.5e-300, 123456789.123456789, 0.0, 1e300, 0.7f };
    const char *kept = "{\"format\":\"org.openmixer.preset/2\",\"bank\":\"A\",\"x-vendor\":{\"n\":1.50,\"l\":[true,null]},"
                       "\"name\":\"caf\\u00e9 \\\"q\\\"\",\"params\":[]}";
    struct pj doc, back, v, arr;
    uint8_t bytes[256];
    char *text, key[64];
    size_t i, len;
    uint8_t *dec;
    int ok;

    // a number goes out as the shortest text that reads back to the same double, never through a float
    ok = 1;
    for (i = 0; i < sizeof(values) / sizeof(values[0]); i++)
    {
        pj_number(&v, values[i]);
        ok &= strtod(v.str, NULL) == values[i];
        pj_free(&v);
    }
    CHECK(ok, "every value reads back to the same double");
    pj_number(&v, 0.25);
    CHECK(strcmp(v.str, "0.25") == 0, "0.25 is written 0.25");
    pj_free(&v);

    // a tree written and parsed again is the same text: members in order, a number's own text kept
    CHECK(pj_parse(kept, strlen(kept), &doc) == 0, "a preset document parses");
    CHECK(strcmp(pj_get_str(&doc, "name"), "caf\xc3\xa9 \"q\"") == 0, "escapes resolve, \\u00e9 to UTF-8");
    pj_string(&v, "renamed");
    pj_set(&doc, "name", &v);
    pj_array(&arr);
    pj_number(&v, 0.1);
    pj_push(&arr, &v);
    pj_set(&doc, "params", &arr);
    text = pj_write(&doc, 1);
    CHECK(text && pj_parse(text, strlen(text), &back) == 0, "the rewrite parses");
    CHECK(pj_get(&back, "x-vendor") && strcmp(pj_get(pj_get(&back, "x-vendor"), "n")->str, "1.50") == 0,
          "an unknown member is kept, its number's text as written");
    CHECK(strcmp(pj_get_str(&back, "bank"), "A") == 0 && strcmp(back.keys[1], "bank") == 0, "members keep their order");
    CHECK(strcmp(pj_get_str(&back, "name"), "renamed") == 0, "a member set is replaced in place");
    CHECK(pj_get(&back, "params")->kids[0].num == 0.1, "a value written reads back exactly");
    free(text);
    pj_free(&back);
    pj_free(&doc);

    // a lax number the reader accepted (RFC 8259 refuses "1." and "1.e5") is normalised on write, to text that
    // parses with this reader and would with a strict one too; a strict number's text is kept as written
    {
        static const char *const lax[] = { "1.", "1.e5", "-0.e+2" };

        ok = 1;
        for (i = 0; i < sizeof(lax) / sizeof(lax[0]); i++)
        {
            const double before = strtod(lax[i], NULL);

            CHECK(pj_parse(lax[i], strlen(lax[i]), &v) == 0, "the lax reader accepts it");
            text = pj_write(&v, 0);
            ok &= text && strcmp(text, lax[i]) != 0 && strtod(text, NULL) == before;
            free(text);
            pj_free(&v);
        }
        CHECK(ok, "a lax number is written back as a strict one of the same value");
    }
    pj_number(&v, 1.5);
    free(v.str);
    v.str = strdup("1.50");
    text = pj_write(&v, 0);
    CHECK(text && strcmp(text, "1.50") == 0, "a strict number's own text survives the write unchanged");
    free(text);
    pj_free(&v);

    // not one JSON value: refused
    CHECK(!parses("{\"a\":1,}") && !parses("{\"a\" 1}") && !parses("[1] 2") && !parses("\"\\u0000\"") && !parses("01")
          && !parses("{\"a\":\"\\ud800\"}") && !parses("") && !parses("nul"), "what is not JSON is refused");
    {
        char deep[PRESET_JSON_DEPTH_MAX * 2 + 8];

        memset(deep, '[', PRESET_JSON_DEPTH_MAX + 1);
        memset(deep + PRESET_JSON_DEPTH_MAX + 1, ']', PRESET_JSON_DEPTH_MAX + 1);
        deep[2 * (PRESET_JSON_DEPTH_MAX + 1)] = '\0';
        CHECK(!parses(deep), "nesting past PRESET_JSON_DEPTH_MAX is refused");
    }

    // base64 both ways, every length mod 3, and text that is not base64 refused
    for (i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t)(i * 37 + 11);
    ok = 1;
    for (len = 0; len < 8; len++)
    {
        size_t n;

        text = preset_base64_encode(bytes, sizeof(bytes) - len);
        dec = preset_base64_decode(text, &n);
        ok &= dec && n == sizeof(bytes) - len && memcmp(dec, bytes, n) == 0;
        free(dec);
        free(text);
    }
    CHECK(ok, "base64 round trip at every length");
    text = preset_base64_encode((const uint8_t *)"state", 5);
    CHECK(strcmp(text, "c3RhdGU=") == 0, "base64 of \"state\" is c3RhdGU=");
    free(text);
    CHECK(!preset_base64_decode("c3R*dGU=", &len) && !preset_base64_decode("c3RhdGV=", &len)
          && !preset_base64_decode("c", &len) && !preset_base64_decode("c3=RhdGU=", &len), "what is not base64 is refused");
    ok = pads_past_int_refused();
    if (ok < 0)
        printf("skip the address space cannot hold 2^31 '=' and the decoder's buffer: a third '=' past an int unchecked\n");
    else
        CHECK(ok, "a third '=' is refused past the 2^31 an int counts");

    // a load key with a space and a percent sign survives the reply line
    CHECK(preset_key_encode("/a b/100%.fxp", key, sizeof(key)) == 0 && strcmp(key, "/a%20b/100%25.fxp") == 0,
          "a load key is written without a space");
    preset_key_decode(key);
    CHECK(strcmp(key, "/a b/100%.fxp") == 0, "and reads back");
    CHECK(preset_key_encode("a b", key, 4) != 0, "a key that does not fit is refused");

    if (failures)
        printf("preset file test FAILED (%d)\n", failures);
    else
        printf("preset file test passed\n");
    return failures != 0;
}
