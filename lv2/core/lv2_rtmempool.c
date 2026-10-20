/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/*
 * rtmempool#Pool (lv2_rtmempool.h), the realtime-safe memory pool feature of lv2dynparam, under its current URI and
 * under its deprecated one, whose struct orders create's arguments differently (zynadd requires that one, #265). A pool
 * is chunks of one size on two lists, free and used. `create`, `destroy` and `allocate_sleepy` may sleep: they call the
 * allocator. `allocate_atomic` and `deallocate` never do: a chunk moves between the lists in O(1) under a
 * priority-inheriting mutex held for those few stores, as mod-host's and Carla's pools hold theirs, and an empty free
 * list answers NULL rather than allocate. `min_preallocated` chunks are made at create, and allocate_sleepy makes the
 * one it hands out when the free list is empty and then tops the list back up to the minimum. No chunk goes back to
 * the allocator before destroy, which frees every chunk, used or free: a plugin that forgets one leaks nothing.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lv2_core_internal.h"
#include "lv2_rtmempool.h"


/*
************************************************************************************************************************
*           LOCAL DEFINES AND TYPES
************************************************************************************************************************
*/

/* the chunk's header, the data after it on a 16-byte boundary */
#define CHUNK_HEADER                    ((sizeof(struct chunk) + 15u) & ~(size_t)15u)

struct chunk
{
    struct chunk *next, *prev;
};

struct pool
{
    pthread_mutex_t lock;
    struct chunk *free_list, *used_list;
    size_t data_size, min, max, n_free, n_used;
    char name[LV2_RTSAFE_MEMORY_POOL_NAME_MAX];
};


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS
************************************************************************************************************************
*/

static void *chunk_data(struct chunk *c)
{
    return (char *)c + CHUNK_HEADER;
}

static struct chunk *chunk_of(void *data)
{
    return (struct chunk *)((char *)data - CHUNK_HEADER);
}

static struct chunk *chunk_new(const struct pool *p)
{
    return malloc(CHUNK_HEADER + p->data_size);
}

static void list_push(struct chunk **head, struct chunk *c)
{
    c->prev = NULL;
    c->next = *head;
    if (*head)
        (*head)->prev = c;
    *head = c;
}

static void list_unlink(struct chunk **head, struct chunk *c)
{
    if (c->prev)
        c->prev->next = c->next;
    else
        *head = c->next;
    if (c->next)
        c->next->prev = c->prev;
}

static void list_free(struct chunk *head)
{
    while (head)
    {
        struct chunk *next = head->next;

        free(head);
        head = next;
    }
}

static void pool_destroy(LV2_RtMemPool_Handle handle)
{
    struct pool *p = handle;

    if (!p)
        return;
    list_free(p->free_list);
    list_free(p->used_list);
    pthread_mutex_destroy(&p->lock);
    free(p);
}

static bool pool_create(LV2_RtMemPool_Handle *handle_ptr, const char *name, size_t data_size, size_t min, size_t max)
{
    pthread_mutexattr_t attr;
    struct pool *p;

    if (!handle_ptr || data_size == 0 || max < min)
        return false;
    p = calloc(1, sizeof(*p));
    if (!p)
        return false;
    p->data_size = data_size;
    p->min = min;
    p->max = max;
    if (name)
        snprintf(p->name, sizeof(p->name), "%s", name);
    // the audio thread may hold the lock for a few stores: a lower thread holding it is lifted, not waited for
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&p->lock, &attr);
    pthread_mutexattr_destroy(&attr);
    while (p->n_free < min)
    {
        struct chunk *c = chunk_new(p);

        if (!c)
        {
            pool_destroy(p);
            return false;
        }
        list_push(&p->free_list, c);
        p->n_free++;
    }
    *handle_ptr = p;
    return true;
}

/* RT: a free chunk moved to the used list, or NULL when the free list is empty */
static void *pool_allocate_atomic(LV2_RtMemPool_Handle handle)
{
    struct pool *p = handle;
    struct chunk *c;

    if (!p)
        return NULL;
    pthread_mutex_lock(&p->lock);
    c = p->free_list;
    if (c)
    {
        list_unlink(&p->free_list, c);
        p->n_free--;
        list_push(&p->used_list, c);
        p->n_used++;
    }
    pthread_mutex_unlock(&p->lock);
    return c ? chunk_data(c) : NULL;
}

/* may sleep: the chunk from the free list, else a new one; then the free list topped up to the minimum */
static void *pool_allocate_sleepy(LV2_RtMemPool_Handle handle)
{
    struct pool *p = handle;
    void *data;

    if (!p)
        return NULL;
    data = pool_allocate_atomic(p);
    if (!data)
    {
        struct chunk *c = chunk_new(p);

        if (!c)
            return NULL;
        pthread_mutex_lock(&p->lock);
        list_push(&p->used_list, c);
        p->n_used++;
        pthread_mutex_unlock(&p->lock);
        data = chunk_data(c);
    }
    for (;;)
    {
        struct chunk *c;
        int full;

        pthread_mutex_lock(&p->lock);
        full = p->n_free >= p->min;
        pthread_mutex_unlock(&p->lock);
        if (full)
            break;
        c = chunk_new(p);
        if (!c)
            break;
        pthread_mutex_lock(&p->lock);
        list_push(&p->free_list, c);
        p->n_free++;
        pthread_mutex_unlock(&p->lock);
    }
    return data;
}

/* RT: the chunk back on the free list; NULL is nothing */
static void pool_deallocate(LV2_RtMemPool_Handle handle, void *data)
{
    struct pool *p = handle;
    struct chunk *c;

    if (!p || !data)
        return;
    c = chunk_of(data);
    pthread_mutex_lock(&p->lock);
    list_unlink(&p->used_list, c);
    p->n_used--;
    list_push(&p->free_list, c);
    p->n_free++;
    pthread_mutex_unlock(&p->lock);
}

/* the deprecated struct's create: the same pool, the handle last and a byte for the verdict */
static unsigned char pool_create_v1(const char *name, size_t data_size, size_t min, size_t max, LV2_RtMemPool_Handle *handle_ptr)
{
    return pool_create(handle_ptr, name, data_size, min, max) ? 1u : 0u;
}


/*
************************************************************************************************************************
*           LOCAL GLOBAL VARIABLES
************************************************************************************************************************
*/

static const LV2_RtMemPool_Pool g_pool =
{
    pool_create, pool_destroy, pool_allocate_atomic, pool_allocate_sleepy, pool_deallocate
};

static const LV2_RtMemPool_Pool_Deprecated g_pool_v1 =
{
    pool_create_v1, pool_destroy, pool_allocate_atomic, pool_allocate_sleepy, pool_deallocate
};


/*
************************************************************************************************************************
*           GLOBAL FUNCTIONS
************************************************************************************************************************
*/

const LV2_RtMemPool_Pool *lv2_rtmempool_feature(void)
{
    return &g_pool;
}

const LV2_RtMemPool_Pool_Deprecated *lv2_rtmempool_feature_v1(void)
{
    return &g_pool_v1;
}
