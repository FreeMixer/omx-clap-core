/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * live_list_test: the list the control tick walks (src/live_list.h). The tool instances (the ids above the plugins') are
 * in it, so they are ticked; an id added or removed while a walk runs leaves the walk right; an id that is freed and
 * taken again is in the list once; the list is what a model of the table says after a long run of adds and removals.
 */

#include <stdio.h>
#include <stdlib.h>

#include "live_list.h"

/* src/effects.h's; tests/live_list_check.sh holds them to it */
#define MAX_PLUGIN_INSTANCES 9990
#define MAX_INSTANCES        10000

static int failures;

#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        if (!(cond)) {                                                                                                \
            printf("FAIL " __VA_ARGS__);                                                                              \
            printf("\n");                                                                                             \
            failures++;                                                                                               \
        }                                                                                                             \
    } while (0)

static int g_ids[MAX_INSTANCES];
static struct live_list g_list = { g_ids, 0 };

static void reset(void)
{
    g_list.count = 0;
}

static int serve(int *out, int max)
{
    int id, n = 0;

    LIVE_EACH(&g_list, id)
        if (n < max)
            out[n++] = id;
    return n;
}

static void test_tool_ids_are_walked(void)
{
    int out[16], n;

    reset();
    live_add(&g_list, MAX_PLUGIN_INSTANCES + 5);
    live_add(&g_list, 3);
    live_add(&g_list, MAX_PLUGIN_INSTANCES - 1);
    live_add(&g_list, MAX_INSTANCES - 1);
    n = serve(out, 16);
    CHECK(n == 4, "tool ids walked: %d ids, want 4", n);
    CHECK(out[0] == 3 && out[1] == MAX_PLUGIN_INSTANCES - 1 && out[2] == MAX_PLUGIN_INSTANCES + 5 &&
              out[3] == MAX_INSTANCES - 1,
          "tool ids walked in ascending order: %d %d %d %d", out[0], out[1], out[2], out[3]);
}

static void test_walk_visits_only_live(void)
{
    int out[16], n;

    reset();
    live_add(&g_list, 1);
    live_add(&g_list, 24);
    n = serve(out, 16);
    CHECK(n == 2 && out[0] == 1 && out[1] == 24, "two live instances: walked %d", n);
    CHECK(live_next(&g_list, 24) == -1 && live_next(&g_list, -1) == 1 && live_next(&g_list, 1) == 24, "live_next");
    reset();
    CHECK(serve(out, 16) == 0, "an empty list walks nothing");
}

/* the body of the walk at id 4 removes the next (6) and adds 5 and 3: 5 is served, 6 is not, 3 (below) is not */
static void test_change_during_walk(void)
{
    int out[16], n = 0, id;

    reset();
    live_add(&g_list, 2);
    live_add(&g_list, 4);
    live_add(&g_list, 6);
    live_add(&g_list, 8);
    LIVE_EACH(&g_list, id)
    {
        out[n++] = id;
        if (id == 4)
        {
            live_remove(&g_list, 6);
            live_add(&g_list, 5);
            live_add(&g_list, 3);
        }
    }
    CHECK(n == 4 && out[0] == 2 && out[1] == 4 && out[2] == 5 && out[3] == 8, "change during walk: %d ids %d %d %d %d", n,
          out[0], out[1], out[2], out[3]);
    CHECK(g_list.count == 5 && g_list.ids[0] == 2 && g_list.ids[1] == 3 && g_list.ids[2] == 4 && g_list.ids[3] == 5 &&
              g_list.ids[4] == 8,
          "list after the walk: count %d", g_list.count);
}

/* the current id is removed by the walk's body, and taken again (the id is reused): it is served once, the next goes on */
static void test_current_removed_and_reused(void)
{
    int out[16], n = 0, id, reused = 0;

    reset();
    live_add(&g_list, 2);
    live_add(&g_list, 4);
    live_add(&g_list, 6);
    LIVE_EACH(&g_list, id)
    {
        out[n++] = id;
        if (id == 4 && !reused)
        {
            reused = 1;
            live_remove(&g_list, 4);
            live_add(&g_list, 4);
        }
    }
    CHECK(n == 3 && out[0] == 2 && out[1] == 4 && out[2] == 6, "current removed and reused: %d ids", n);
    CHECK(g_list.count == 3, "reused id is in the list once: count %d", g_list.count);

    n = 0;
    LIVE_EACH(&g_list, id)
    {
        out[n++] = id;
        if (id == 2)
            live_remove(&g_list, 2);   /* the current leaves: the walk goes on to 4 */
    }
    CHECK(n == 3 && out[1] == 4 && out[2] == 6 && g_list.count == 2, "current removed: %d ids, count %d", n, g_list.count);
}

static void test_add_remove_idempotent(void)
{
    reset();
    live_add(&g_list, 7);
    live_add(&g_list, 7);
    CHECK(g_list.count == 1, "added twice: count %d", g_list.count);
    live_remove(&g_list, 8);
    CHECK(g_list.count == 1, "absent removed: count %d", g_list.count);
    live_remove(&g_list, 7);
    live_remove(&g_list, 7);
    CHECK(g_list.count == 0, "removed twice: count %d", g_list.count);
}

/* a long run of adds and removals against a table of flags: the list is the flags' ids in order, every time */
static void test_against_model(void)
{
    static char model[MAX_INSTANCES];
    unsigned seed = 12345;
    int step, id, n, want, i;

    reset();
    memset(model, 0, sizeof(model));
    for (step = 0; step < 200000; step++)
    {
        seed = seed * 1103515245u + 12345u;
        /* ids drawn near the two ends and the plugin/tool border, where an off-by-one would be */
        id = (int)((seed >> 8) % 40);
        if (step & 1)
            id = MAX_INSTANCES - 1 - id;
        if ((seed >> 20) & 1)
        {
            model[id] = 1;
            live_add(&g_list, id);
        }
        else
        {
            model[id] = 0;
            live_remove(&g_list, id);
        }
        if (g_list.count > 100)    /* at most 80 ids are ever drawn: a list longer than that holds an id twice */
        {
            CHECK(0, "model step %d: %d ids in the list", step, g_list.count);
            return;
        }
        if (step % 1000 == 0)
        {
            n = 0;
            for (i = 0; i < MAX_INSTANCES; i++)
                if (model[i])
                {
                    CHECK(n < g_list.count && g_list.ids[n] == i, "model step %d id %d", step, i);
                    n++;
                }
            want = n;
            CHECK(g_list.count == want, "model count %d want %d at step %d", g_list.count, want, step);
        }
    }
}

int main(void)
{
    test_tool_ids_are_walked();
    test_walk_visits_only_live();
    test_change_during_walk();
    test_current_removed_and_reused();
    test_add_remove_idempotent();
    test_against_model();
    if (failures)
    {
        printf("live_list_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("ok   live_list_test\n");
    return 0;
}
