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
************************************************************************************************************************
*
* The preset file of docs/presets.md, org.openmixer.preset/1 and /2, as a JSON tree: read whole, members kept in their
* order and every number with the text it was written as, so a rewrite keeps what this host does not know byte for byte.
* Nothing here knows a plugin: effects.c fills the members and checks them against the instance.
*
************************************************************************************************************************
*/

#ifndef PRESET_FILE_H
#define PRESET_FILE_H

#include <stddef.h>
#include <stdint.h>

/* the format word and the two versions a file may carry; any other is refused */
#define PRESET_FORMAT_PREFIX  "org.openmixer.preset/"
#define PRESET_FORMAT_PARAMS  PRESET_FORMAT_PREFIX "1"
#define PRESET_FORMAT_STATE   PRESET_FORMAT_PREFIX "2"

/* carries */
#define PRESET_CARRIES_PARAMS       "params"
#define PRESET_CARRIES_PARAMS_STATE "params+state"
#define PRESET_CARRIES_STATE        "state"

/* state.context: a CLAP plugin's preset context, an LV2 plugin's state through the adapter */
#define PRESET_CONTEXT_PRESET "preset"
#define PRESET_CONTEXT_PROJECT "project"
#define PRESET_CONTEXT_LV2    "lv2"
#define PRESET_ENCODING_BASE64 "base64"

/* the refusals of a preset verb, plugin-hostd#131's codes, after mod-host's -104: "resp -104 <code>" */
#define PRESET_CODE_NOT_OFFERED     "PRESET_NOT_OFFERED"
#define PRESET_CODE_INVALID         "PRESET_INVALID"
#define PRESET_CODE_NEEDS_STATE     "PRESET_NEEDS_STATE"
#define PRESET_CODE_PLUGIN_CHANGED  "PRESET_PLUGIN_CHANGED"
#define PRESET_CODE_LOAD_REFUSED    "LOAD_REFUSED"

/* the largest preset file read, in bytes: a state body of CLAP_HOST_STATE_MAX_BYTES in base64 and its parameters */
#define PRESET_FILE_MAX_BYTES (3u * 1024u * 1024u)
/* how deep a file's arrays and objects nest at most */
#define PRESET_JSON_DEPTH_MAX 32

enum pj_type { PJ_NULL, PJ_FALSE, PJ_TRUE, PJ_NUMBER, PJ_STRING, PJ_ARRAY, PJ_OBJECT };

/* one JSON value: a number keeps its text (`str`) beside its value, an object its members in order (`keys`) */
struct pj
{
    enum pj_type type;
    double num;
    char *str;          // a string's bytes (NUL-terminated, may hold none inside: \u0000 is refused), a number's text
    struct pj *kids;    // an array's items, an object's member values
    char **keys;        // an object's member names, as many as kids
    size_t n, cap;
};

/* parse `len` bytes of `text` into `out`: 0, or -1 for anything that is not one JSON value (trailing bytes included) */
int pj_parse(const char *text, size_t len, struct pj *out);
/* read the file at `path` whole and parse it: 0, or -1 (unreadable, larger than PRESET_FILE_MAX_BYTES, not JSON) */
int pj_read_file(const char *path, struct pj *out);
void pj_free(struct pj *v);

/* an object's member by name, NULL when absent or `obj` is no object */
struct pj *pj_get(const struct pj *obj, const char *key);
/* a member's string, NULL when absent or not a string */
const char *pj_get_str(const struct pj *obj, const char *key);

/* set an object's member, replacing one of that name in place or appending it; the value is moved in (left PJ_NULL)
 * and the old one freed. 0, or -1 when memory runs out (the value is freed) */
int pj_set(struct pj *obj, const char *key, struct pj *val);
/* append a value to an array, moved in as pj_set does */
int pj_push(struct pj *arr, struct pj *val);
/* the values pj_set and pj_push take: a string copied, a number written as the shortest text that reads back to the same
 * double; 0, or -1 when memory runs out (or `num` is not finite) */
int pj_string(struct pj *v, const char *s);
int pj_number(struct pj *v, double num);
void pj_object(struct pj *v);
void pj_array(struct pj *v);

/* the tree as JSON text, malloc'd: `pretty` breaks it in lines with two spaces of indent, a newline at the end; NULL when
 * memory runs out */
char *pj_write(const struct pj *v, int pretty);

/* base64 (RFC 4648, padded) of `len` bytes, malloc'd and NUL-terminated; NULL when memory runs out */
char *preset_base64_encode(const uint8_t *data, size_t len);
/* decode base64 text into a malloc'd buffer of `*len` bytes: NULL for text that is not base64 (whitespace is skipped) */
uint8_t *preset_base64_decode(const char *text, size_t *len);

/* a load key of preset_get: a space as %20, a % as %25, into `out` of `size` bytes; 0, or -1 when it does not fit */
int preset_key_encode(const char *in, char *out, size_t size);
/* the other way, in place */
void preset_key_decode(char *s);

#endif // PRESET_FILE_H
