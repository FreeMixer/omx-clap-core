// SPDX-License-Identifier: GPL-3.0-or-later
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

/* The xrun count a worker reports (#371): what src/xrun_report.h decides is due, value by value, and that JACK's thread
 * and the control thread agree on the count. */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "xrun_report.h"

static int g_failed;

#define CHECK(cond, ...)                                                                                              \
    do                                                                                                                \
    {                                                                                                                 \
        if (cond)                                                                                                     \
            printf("ok   " __VA_ARGS__);                                                                              \
        else                                                                                                          \
        {                                                                                                             \
            printf("FAIL " __VA_ARGS__);                                                                              \
            g_failed++;                                                                                               \
        }                                                                                                             \
        printf("\n");                                                                                                 \
    } while (0)

#define THREAD_NOTES 4
#define NOTES_EACH 25000

static void *noter(void *arg)
{
    for (int i = 0; i < NOTES_EACH; i++)
        xrun_report_note(arg);
    return NULL;
}

int main(void)
{
    xrun_report_t r;
    uint64_t count = 99;

    xrun_report_init(&r);
    CHECK(!xrun_report_due(&r, 0, &count) && count == 99, "a worker with no xrun writes nothing at start");
    CHECK(!xrun_report_due(&r, 5000, &count), "and nothing later while the count stays 0");

    xrun_report_note(&r);
    CHECK(xrun_report_due(&r, 10, &count) && count == 1, "the first xrun is written at once, count 1");
    CHECK(!xrun_report_due(&r, 11, &count) && count == 1, "the same count is not written twice");
    CHECK(!xrun_report_due(&r, 5000, &count), "not even long after");

    xrun_report_note(&r);
    xrun_report_note(&r);
    CHECK(!xrun_report_due(&r, 500, &count), "a change 490 ms after the last line waits");
    CHECK(!xrun_report_due(&r, 1009, &count), "and still waits at 999 ms");
    CHECK(xrun_report_due(&r, 1010, &count) && count == 3, "it is written at 1000 ms, as the count then, 3");

    xrun_report_note(&r);
    CHECK(!xrun_report_due(&r, 1500, &count), "one more waits for the second after 1010");
    xrun_report_note(&r);
    CHECK(xrun_report_due(&r, 2010, &count) && count == 5, "the lines in between are folded: one line, count 5");
    CHECK(!xrun_report_due(&r, 9000, &count), "nothing is written for a count already written");

    xrun_report_init(&r);
    pthread_t t[THREAD_NOTES];
    for (int i = 0; i < THREAD_NOTES; i++)
        pthread_create(&t[i], NULL, noter, &r);
    for (int i = 0; i < THREAD_NOTES; i++)
        pthread_join(t[i], NULL);
    CHECK(xrun_report_due(&r, 1, &count) && count == (uint64_t)THREAD_NOTES * NOTES_EACH,
          "four threads noting %d each give the count %d, none lost", NOTES_EACH, THREAD_NOTES * NOTES_EACH);

    return g_failed ? 1 : 0;
}
