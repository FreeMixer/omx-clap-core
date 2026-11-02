/* SPDX-License-Identifier: GPL-3.0-or-later */

/** @file
 * @brief The live instances of the host, as a list of ids kept in ascending order. */

/*
 * live_list.h: the ids of the instances that have a client, plugins and tool instances alike. The control tick and the
 * other walks over the instances serve these and no empty slot: a worker with one plugin visits one slot, not the
 * whole table.
 *
 * Owner: the control thread of omx-clap-host, the one that serves the socket and runs effects_idle. Every add and
 * removal happens in a command callback on that thread (effects_add, effects_remove, the chain commands) and every walk
 * on it too. JACK's threads and the plugins' threads never see this list; they hold the instance itself.
 *
 * A walk asks for the next id above the one it just served (live_next), so the body of a walk may add or remove any
 * instance: an id above the cursor that is added is served, one that is removed is skipped, one at or below it is not
 * served twice.
 */

#ifndef OMX_LIVE_LIST_H
#define OMX_LIVE_LIST_H

#include <stddef.h>
#include <string.h>

struct live_list {
    int *ids;       /* ascending, `capacity` entries */
    int count;
};

/* the index of the first live id above `after`, the count when none */
static inline int live_index(const struct live_list *list, int after)
{
    int lo = 0, hi = list->count;

    while (lo < hi)
    {
        const int mid = lo + (hi - lo) / 2;

        if (list->ids[mid] > after)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

/* the first live id above `after` (-1 for the first of all), -1 when none */
static inline int live_next(const struct live_list *list, int after)
{
    const int at = live_index(list, after);

    return at < list->count ? list->ids[at] : -1;
}

/* `id` joins the list; nothing when it is there already. The caller's table has room for every id it can name. */
static inline void live_add(struct live_list *list, int id)
{
    const int at = live_index(list, id - 1);

    if (at < list->count && list->ids[at] == id)
        return;
    memmove(&list->ids[at + 1], &list->ids[at], (size_t)(list->count - at) * sizeof(list->ids[0]));
    list->ids[at] = id;
    list->count++;
}

/* `id` leaves the list; nothing when it is not there */
static inline void live_remove(struct live_list *list, int id)
{
    const int at = live_index(list, id - 1);

    if (at >= list->count || list->ids[at] != id)
        return;
    memmove(&list->ids[at], &list->ids[at + 1], (size_t)(list->count - at - 1) * sizeof(list->ids[0]));
    list->count--;
}

#define LIVE_EACH(list, id) for ((id) = live_next((list), -1); (id) >= 0; (id) = live_next((list), (id)))

#endif
