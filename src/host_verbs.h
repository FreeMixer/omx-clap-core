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
* The grammar of the verbs data/verbs.json declares, over its renders: a verb's word and format are the OMX_VERB_
* macros of omx_clap_core_host_verbs.h, a strip kind and a feedback event the words of omx_clap_core_vocabulary.h. What
* the declaration states in prose (a string word's bytes, a note's ranges) is checked here, and the refusal numbers the
* host answers past mod-host's are named here, each the resp of the HOST_REPLY_CODES code its name spells
* (tests/host-verbs-check.sh holds them to the declaration).
*
************************************************************************************************************************
*/

#ifndef HOST_VERBS_H
#define HOST_VERBS_H


/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <string.h>
#include <clap/string-sizes.h>
#include <clap/ext/remote-controls.h>

#include "omx_clap_core_host_verbs.h"
#include "omx_clap_core_vocabulary.h"


/*
************************************************************************************************************************
*           DEFINES
************************************************************************************************************************
*/

/* the refusals of HOST_REPLY_CODES the host answers past mod-host's own numbers: HOST_REPLY_<NAME> is the resp of
 * reply.host.<name> */
#define HOST_REPLY_PIN_ABSENT           (-508)
#define HOST_REPLY_PIN_LAYOUT_MISMATCH  (-510)
#define HOST_REPLY_NO_PARAM_CONTRACT    (-511)
#define HOST_REPLY_NO_NOTE_INPUT        (-513)
#define HOST_REPLY_NOTE_QUEUE_FULL      (-517)

/* the word for what is not there: no colour in track_info, an empty slot of remote_page_get, no note input */
#define HOST_WORD_NONE          "-"
/* the longest string word, in bytes: a CLAP name without its terminator */
#define HOST_STRING_MAX         (CLAP_NAME_SIZE - 1)
/* the slots of one remote page */
#define HOST_REMOTE_PAGE_SLOTS  CLAP_REMOTE_CONTROLS_COUNT
/* a note's words: MIDI's ranges */
#define HOST_NOTE_CHANNEL_MAX   15
#define HOST_NOTE_KEY_MAX       127
#define HOST_NOTE_VELOCITY_MAX  127
/* the feedback line of a plugin that changed its pages, with the instance number */
#define HOST_EVENT_REMOTE_PAGES_CHANGED_FMT OMX_HOST_EVENTS_REMOTE_PAGES_CHANGED " %d"


/*
************************************************************************************************************************
*           INLINE FUNCTIONS
************************************************************************************************************************
*/

/* whether s is a string word: valid UTF-8, at most HOST_STRING_MAX bytes, no byte below 0x20 */
static inline int host_string_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;

    while (p[n])
    {
        unsigned c = p[n], cp;
        int more, k;

        if (c < 0x20)
            return 0;
        if (c < 0x80)
        {
            n++;
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf)
            more = 1, cp = c & 0x1f;
        else if (c >= 0xe0 && c <= 0xef)
            more = 2, cp = c & 0x0f;
        else if (c >= 0xf0 && c <= 0xf4)
            more = 3, cp = c & 0x07;
        else
            return 0;
        for (k = 1; k <= more; k++)
        {
            if ((p[n + k] & 0xc0) != 0x80)
                return 0;
            cp = (cp << 6) | (p[n + k] & 0x3f);
        }
        if ((more == 2 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) || (more == 3 && (cp < 0x10000 || cp > 0x10ffff)))
            return 0;
        n += more + 1;
    }
    return n <= HOST_STRING_MAX;
}

/* whether c is a colour of track_info: #RRGGBB, either case, or HOST_WORD_NONE */
static inline int host_color_valid(const char *c)
{
    int n;

    if (!strcmp(c, HOST_WORD_NONE))
        return 1;
    if (c[0] != '#')
        return 0;
    for (n = 1; n <= 6; n++)
        if (!((c[n] >= '0' && c[n] <= '9') || (c[n] >= 'a' && c[n] <= 'f') || (c[n] >= 'A' && c[n] <= 'F')))
            return 0;
    return c[7] == '\0';
}

/* the strip kind a last word of track_info names, 1 for the first word of HOST_TRACK_KINDS; 0 for NULL (an input
 * channel); -1 for a word that is not one */
static inline int host_track_kind(const char *word)
{
    static const char *const kinds[] = OMX_HOST_TRACK_KINDS_INIT;
    unsigned n;

    if (!word)
        return 0;
    for (n = 0; n < OMX_HOST_TRACK_KINDS_COUNT; n++)
        if (!strcmp(word, kinds[n]))
            return (int)n + 1;
    return -1;
}

/* whether the words of a track_info line, the verb first, are its grammar */
static inline int host_track_info_valid(char *const *word, int count)
{
    return (count == 4 || count == 5) && host_string_valid(word[2]) && host_color_valid(word[3]) &&
           host_track_kind(count == 5 ? word[4] : NULL) >= 0;
}

/* whether word is a whole number from lo to hi, in decimal with no sign but a leading '-' ('-0' is not a number: zero
 * has one spelling) */
static inline int host_number_in(const char *word, int lo, int hi)
{
    long v = 0;
    int n = word[0] == '-' ? 1 : 0;

    if (!word[n])
        return 0;
    for (; word[n]; n++)
    {
        if (word[n] < '0' || word[n] > '9' || v > 100000)
            return 0;
        v = v * 10 + (word[n] - '0');
    }
    if (word[0] == '-')
    {
        if (v == 0)
            return 0;
        v = -v;
    }
    return v >= lo && v <= hi;
}

/* whether the words of a note verb's line, the verb first, are its grammar; 0 for a line of another verb */
static inline int host_note_valid(char *const *word, int count)
{
    int on = !strcmp(word[0], OMX_VERB_NOTE_ON);

    if (count < 2 || !host_number_in(word[1], 0, 1 << 20))
        return 0;
    if (on || !strcmp(word[0], OMX_VERB_NOTE_OFF))
        return count == 5 && host_number_in(word[2], 0, HOST_NOTE_CHANNEL_MAX) && host_number_in(word[3], 0, HOST_NOTE_KEY_MAX) &&
               host_number_in(word[4], on ? 1 : 0, HOST_NOTE_VELOCITY_MAX);
    if (!strcmp(word[0], OMX_VERB_ALL_NOTES_OFF) || !strcmp(word[0], OMX_VERB_NOTE_INPUT))
        return count == 2;
    return 0;
}


/*
************************************************************************************************************************
*           END HEADER
************************************************************************************************************************
*/

#endif
