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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "preset_file.h"

/* ---------------------------------------------------------------- reading */

struct pj_in
{
    const char *p, *end;
    int depth;
};

static void skip_ws(struct pj_in *in)
{
    while (in->p < in->end && (*in->p == ' ' || *in->p == '\t' || *in->p == '\n' || *in->p == '\r'))
        in->p++;
}

static int grow(struct pj *v)
{
    size_t cap;
    struct pj *kids;
    char **keys;

    if (v->n < v->cap)
        return 0;
    cap = v->cap ? v->cap * 2 : 8;
    kids = realloc(v->kids, cap * sizeof(*kids));
    if (!kids)
        return -1;
    v->kids = kids;
    if (v->type == PJ_OBJECT)
    {
        keys = realloc(v->keys, cap * sizeof(*keys));
        if (!keys)
            return -1;
        v->keys = keys;
    }
    v->cap = cap;
    return 0;
}

static int hex4(const char *p, unsigned *out)
{
    unsigned v = 0;
    int i;

    for (i = 0; i < 4; i++)
    {
        const char c = p[i];

        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (unsigned)(c - 'A' + 10);
        else
            return -1;
    }
    *out = v;
    return 0;
}

static size_t put_utf8(char *o, unsigned cp)
{
    if (cp < 0x80)
    {
        o[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800)
    {
        o[0] = (char)(0xC0 | (cp >> 6));
        o[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000)
    {
        o[0] = (char)(0xE0 | (cp >> 12));
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18));
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* a string at in->p (its opening quote) into a malloc'd copy, escapes resolved */
static char *parse_string(struct pj_in *in)
{
    const char *p = in->p + 1;
    char *out, *o;

    // the decoded text is never longer than the escaped one
    for (o = (char *)p; o < in->end && *o != '"'; o += (*o == '\\') ? 2 : 1)
        ;
    if (o >= in->end)
        return NULL;
    out = malloc((size_t)(o - p) + 1);
    if (!out)
        return NULL;
    o = out;
    while (p < in->end && *p != '"')
    {
        unsigned cp, lo;

        if ((unsigned char)*p < 0x20)
            goto bad;
        if (*p != '\\')
        {
            *o++ = *p++;
            continue;
        }
        if (++p >= in->end)
            goto bad;
        switch (*p++)
        {
        case '"': *o++ = '"'; break;
        case '\\': *o++ = '\\'; break;
        case '/': *o++ = '/'; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'n': *o++ = '\n'; break;
        case 'r': *o++ = '\r'; break;
        case 't': *o++ = '\t'; break;
        case 'u':
            if (in->end - p < 4 || hex4(p, &cp) != 0 || cp == 0)
                goto bad;
            p += 4;
            if (cp >= 0xD800 && cp < 0xDC00)
            {
                if (in->end - p < 6 || p[0] != '\\' || p[1] != 'u' || hex4(p + 2, &lo) != 0 || lo < 0xDC00 || lo > 0xDFFF)
                    goto bad;
                p += 6;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            else if (cp >= 0xDC00 && cp < 0xE000)
                goto bad;
            o += put_utf8(o, cp);
            break;
        default:
            goto bad;
        }
    }
    if (p >= in->end)
        goto bad;
    *o = '\0';
    in->p = p + 1;
    return out;
bad:
    free(out);
    return NULL;
}

static int parse_value(struct pj_in *in, struct pj *v);

static int parse_number(struct pj_in *in, struct pj *v)
{
    const char *s = in->p;
    char *end;
    size_t len;

    if (*s == '-')
        s++;
    if (s >= in->end || *s < '0' || *s > '9' || (*s == '0' && s + 1 < in->end && s[1] >= '0' && s[1] <= '9'))
        return -1;
    while (s < in->end && ((*s >= '0' && *s <= '9') || *s == '.' || *s == 'e' || *s == 'E' || *s == '+' || *s == '-'))
        s++;
    len = (size_t)(s - in->p);
    v->str = malloc(len + 1);
    if (!v->str)
        return -1;
    memcpy(v->str, in->p, len);
    v->str[len] = '\0';
    v->num = strtod(v->str, &end);
    if (*end != '\0' || !isfinite(v->num))
        return -1;
    v->type = PJ_NUMBER;
    in->p = s;
    return 0;
}

static int parse_items(struct pj_in *in, struct pj *v, int object)
{
    v->type = object ? PJ_OBJECT : PJ_ARRAY;
    if (++in->depth > PRESET_JSON_DEPTH_MAX)
        return -1;
    in->p++;
    skip_ws(in);
    if (in->p < in->end && *in->p == (object ? '}' : ']'))
    {
        in->p++;
        in->depth--;
        return 0;
    }
    for (;;)
    {
        if (grow(v) != 0)
            return -1;
        if (object)
        {
            skip_ws(in);
            if (in->p >= in->end || *in->p != '"')
                return -1;
            if (!(v->keys[v->n] = parse_string(in)))
                return -1;
            skip_ws(in);
            if (in->p >= in->end || *in->p != ':')
            {
                free(v->keys[v->n]);
                return -1;
            }
            in->p++;
        }
        memset(&v->kids[v->n], 0, sizeof(v->kids[v->n]));
        if (parse_value(in, &v->kids[v->n]) != 0)
        {
            v->n++;   // freed with the rest
            return -1;
        }
        v->n++;
        skip_ws(in);
        if (in->p < in->end && *in->p == ',')
        {
            in->p++;
            continue;
        }
        if (in->p < in->end && *in->p == (object ? '}' : ']'))
        {
            in->p++;
            in->depth--;
            return 0;
        }
        return -1;
    }
}

static int literal(struct pj_in *in, const char *word, enum pj_type type, struct pj *v)
{
    const size_t len = strlen(word);

    if ((size_t)(in->end - in->p) < len || memcmp(in->p, word, len) != 0)
        return -1;
    in->p += len;
    v->type = type;
    return 0;
}

static int parse_value(struct pj_in *in, struct pj *v)
{
    skip_ws(in);
    if (in->p >= in->end)
        return -1;
    switch (*in->p)
    {
    case '{': return parse_items(in, v, 1);
    case '[': return parse_items(in, v, 0);
    case '"':
        v->type = PJ_STRING;
        return (v->str = parse_string(in)) ? 0 : -1;
    case 't': return literal(in, "true", PJ_TRUE, v);
    case 'f': return literal(in, "false", PJ_FALSE, v);
    case 'n': return literal(in, "null", PJ_NULL, v);
    default: return parse_number(in, v);
    }
}

int pj_parse(const char *text, size_t len, struct pj *out)
{
    struct pj_in in = { text, text + len, 0 };

    memset(out, 0, sizeof(*out));
    if (parse_value(&in, out) == 0)
    {
        skip_ws(&in);
        if (in.p == in.end)
            return 0;
    }
    pj_free(out);
    return -1;
}

int pj_read_file(const char *path, struct pj *out)
{
    FILE *file = fopen(path, "rb");
    char *text;
    size_t len;
    int rc;

    memset(out, 0, sizeof(*out));
    if (!file)
        return -1;
    text = malloc(PRESET_FILE_MAX_BYTES + 1);
    if (!text)
    {
        fclose(file);
        return -1;
    }
    len = fread(text, 1, PRESET_FILE_MAX_BYTES + 1, file);
    rc = ferror(file) || len > PRESET_FILE_MAX_BYTES ? -1 : pj_parse(text, len, out);
    fclose(file);
    free(text);
    return rc;
}

void pj_free(struct pj *v)
{
    size_t i;

    if (!v)
        return;
    for (i = 0; i < v->n; i++)
    {
        pj_free(&v->kids[i]);
        if (v->keys)
            free(v->keys[i]);
    }
    free(v->kids);
    free(v->keys);
    free(v->str);
    memset(v, 0, sizeof(*v));
}

struct pj *pj_get(const struct pj *obj, const char *key)
{
    size_t i;

    if (!obj || obj->type != PJ_OBJECT)
        return NULL;
    for (i = 0; i < obj->n; i++)
        if (strcmp(obj->keys[i], key) == 0)
            return &obj->kids[i];
    return NULL;
}

const char *pj_get_str(const struct pj *obj, const char *key)
{
    const struct pj *v = pj_get(obj, key);

    return v && v->type == PJ_STRING ? v->str : NULL;
}

/* ---------------------------------------------------------------- building */

int pj_set(struct pj *obj, const char *key, struct pj *val)
{
    struct pj *old = pj_get(obj, key);

    if (old)
    {
        pj_free(old);
        *old = *val;
        memset(val, 0, sizeof(*val));
        return 0;
    }
    if (grow(obj) != 0 || !(obj->keys[obj->n] = strdup(key)))
    {
        pj_free(val);
        return -1;
    }
    obj->kids[obj->n++] = *val;
    memset(val, 0, sizeof(*val));
    return 0;
}

void pj_del(struct pj *obj, const char *key)
{
    struct pj *v = pj_get(obj, key);
    size_t i;

    if (!v)
        return;
    i = (size_t)(v - obj->kids);
    pj_free(v);
    free(obj->keys[i]);
    memmove(&obj->kids[i], &obj->kids[i + 1], (obj->n - i - 1) * sizeof(*obj->kids));
    memmove(&obj->keys[i], &obj->keys[i + 1], (obj->n - i - 1) * sizeof(*obj->keys));
    obj->n--;
}

int pj_push(struct pj *arr, struct pj *val)
{
    if (grow(arr) != 0)
    {
        pj_free(val);
        return -1;
    }
    arr->kids[arr->n++] = *val;
    memset(val, 0, sizeof(*val));
    return 0;
}

int pj_string(struct pj *v, const char *s)
{
    memset(v, 0, sizeof(*v));
    v->type = PJ_STRING;
    return (v->str = strdup(s ? s : "")) ? 0 : -1;
}

int pj_number(struct pj *v, double num)
{
    char text[40];
    int digits;

    memset(v, 0, sizeof(*v));
    if (!isfinite(num))
        return -1;
    // the shortest text that reads back to the same double: a value is never widened nor rounded on its way to the file
    for (digits = 1; digits <= 17; digits++)
    {
        snprintf(text, sizeof(text), "%.*g", digits, num);
        if (strtod(text, NULL) == num)
            break;
    }
    v->type = PJ_NUMBER;
    v->num = num;
    return (v->str = strdup(text)) ? 0 : -1;
}

void pj_object(struct pj *v)
{
    memset(v, 0, sizeof(*v));
    v->type = PJ_OBJECT;
}

void pj_array(struct pj *v)
{
    memset(v, 0, sizeof(*v));
    v->type = PJ_ARRAY;
}

/* ---------------------------------------------------------------- writing */

struct pj_out
{
    char *buf;
    size_t len, cap;
    int failed;
};

static void put(struct pj_out *o, const char *s, size_t n)
{
    if (o->failed)
        return;
    if (o->len + n + 1 > o->cap)
    {
        size_t cap = o->cap ? o->cap : 4096;
        char *buf;

        while (o->len + n + 1 > cap)
            cap *= 2;
        buf = realloc(o->buf, cap);
        if (!buf)
        {
            o->failed = 1;
            return;
        }
        o->buf = buf;
        o->cap = cap;
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = '\0';
}

static void puts_(struct pj_out *o, const char *s)
{
    put(o, s, strlen(s));
}

static void put_string(struct pj_out *o, const char *s)
{
    const char *run = s;

    put(o, "\"", 1);
    for (; *s; s++)
    {
        const unsigned char c = (unsigned char)*s;
        char esc[8];

        if (c != '"' && c != '\\' && c >= 0x20)
            continue;
        put(o, run, (size_t)(s - run));
        if (c == '"' || c == '\\')
            snprintf(esc, sizeof(esc), "\\%c", c);
        else if (c == '\n')
            snprintf(esc, sizeof(esc), "\\n");
        else if (c == '\t')
            snprintf(esc, sizeof(esc), "\\t");
        else if (c == '\r')
            snprintf(esc, sizeof(esc), "\\r");
        else
            snprintf(esc, sizeof(esc), "\\u%04x", c);
        puts_(o, esc);
        run = s + 1;
    }
    put(o, run, (size_t)(s - run));
    put(o, "\"", 1);
}

static void indent(struct pj_out *o, int pretty, int depth)
{
    int i;

    if (!pretty)
        return;
    put(o, "\n", 1);
    for (i = 0; i < depth; i++)
        put(o, "  ", 2);
}

static void write_value(struct pj_out *o, const struct pj *v, int pretty, int depth)
{
    size_t i;

    switch (v->type)
    {
    case PJ_NULL: puts_(o, "null"); break;
    case PJ_FALSE: puts_(o, "false"); break;
    case PJ_TRUE: puts_(o, "true"); break;
    case PJ_NUMBER: puts_(o, v->str ? v->str : "0"); break;
    case PJ_STRING: put_string(o, v->str ? v->str : ""); break;
    case PJ_ARRAY:
    case PJ_OBJECT:
        put(o, v->type == PJ_OBJECT ? "{" : "[", 1);
        for (i = 0; i < v->n; i++)
        {
            if (i)
                put(o, ",", 1);
            indent(o, pretty, depth + 1);
            if (v->type == PJ_OBJECT)
            {
                put_string(o, v->keys[i]);
                puts_(o, pretty ? ": " : ":");
            }
            write_value(o, &v->kids[i], pretty, depth + 1);
        }
        if (v->n)
            indent(o, pretty, depth);
        put(o, v->type == PJ_OBJECT ? "}" : "]", 1);
        break;
    }
}

char *pj_write(const struct pj *v, int pretty)
{
    struct pj_out o = { NULL, 0, 0, 0 };

    write_value(&o, v, pretty, 0);
    if (pretty)
        put(&o, "\n", 1);
    if (o.failed)
    {
        free(o.buf);
        return NULL;
    }
    return o.buf;
}

/* ---------------------------------------------------------------- base64 and keys */

static const char g_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *preset_base64_encode(const uint8_t *data, size_t len)
{
    char *out = malloc((len + 2) / 3 * 4 + 1), *o = out;
    size_t i;

    if (!out)
        return NULL;
    for (i = 0; i + 2 < len; i += 3)
    {
        const uint32_t w = (uint32_t)data[i] << 16 | (uint32_t)data[i + 1] << 8 | data[i + 2];

        *o++ = g_b64[w >> 18];
        *o++ = g_b64[(w >> 12) & 63];
        *o++ = g_b64[(w >> 6) & 63];
        *o++ = g_b64[w & 63];
    }
    if (i < len)
    {
        const uint32_t w = (uint32_t)data[i] << 16 | (i + 1 < len ? (uint32_t)data[i + 1] << 8 : 0);

        *o++ = g_b64[w >> 18];
        *o++ = g_b64[(w >> 12) & 63];
        *o++ = i + 1 < len ? g_b64[(w >> 6) & 63] : '=';
        *o++ = '=';
    }
    *o = '\0';
    return out;
}

uint8_t *preset_base64_decode(const char *text, size_t *len)
{
    uint8_t *out = malloc(strlen(text) / 4 * 3 + 3);
    uint32_t w = 0;
    size_t n = 0;
    int bits = 0, pad = 0;

    if (!out)
        return NULL;
    for (; *text; text++)
    {
        const char *at;

        if (*text == ' ' || *text == '\n' || *text == '\r' || *text == '\t')
            continue;
        if (*text == '=')
        {
            pad++;
            continue;
        }
        at = strchr(g_b64, *text);
        if (!at || pad)
            goto bad;
        w = w << 6 | (uint32_t)(at - g_b64);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out[n++] = (uint8_t)(w >> bits);
        }
    }
    // what is left is the padding's: under a byte, and zero
    if (bits >= 6 || (w & ((1u << bits) - 1)) != 0 || pad > 2)
        goto bad;
    *len = n;
    return out;
bad:
    free(out);
    return NULL;
}

int preset_key_encode(const char *in, char *out, size_t size)
{
    size_t n = 0;

    for (; *in; in++)
    {
        const char *put_ = *in == ' ' ? "%20" : *in == '%' ? "%25" : NULL;
        const size_t k = put_ ? 3 : 1;

        if (n + k >= size)
            return -1;
        if (put_)
            memcpy(out + n, put_, 3);
        else
            out[n] = *in;
        n += k;
    }
    if (n >= size)
        return -1;
    out[n] = '\0';
    return 0;
}

void preset_key_decode(char *s)
{
    char *o = s;

    for (; *s; s++)
    {
        if (s[0] == '%' && s[1] == '2' && (s[2] == '0' || s[2] == '5'))
        {
            *o++ = s[2] == '0' ? ' ' : '%';
            s += 2;
        }
        else
            *o++ = *s;
    }
    *o = '\0';
}
