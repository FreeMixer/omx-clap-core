/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * clap_untrusted_test.c: what a plugin says about its parameters is its own answer, not a fact the core measured. A
 * fake this file owns reports a count and names of its choosing, no jack.
 *
 *   ./tests/clap_untrusted_test
 *
 *  1. THE COUNT IS CAPPED at CLAP_HOST_PARAM_COUNT_MAX (2048): a plugin reporting exactly 2048 parameters serves them all;
 *     one reporting 2049 or 10000000 serves the first 2048, an id past them is not a parameter of it, and the open
 *     leaves one log message saying so. The layout pin of a plugin past the cap is refused, never pinned short.
 *  2. A FAILED ALLOCATION REFUSES: the read-back table that open sizes from the count cannot be had, and the open is
 *     refused with a code, no instance; the roster's own table cannot be had, and the roster is refused, no rows. The
 *     same plugin opens and serves its rows once allocation works again.
 *  3. THE NAME IS BOUNDED: a name that fills CLAP_NAME_SIZE with no NUL, followed by a module path and numbers with no
 *     zero byte either, is read no further than its own buffer. The build with -fsanitize=address (make test-untrusted-asan)
 *     compiles the core into the test and catches a read past the plugin's info.
 *
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 */
/*
 * The unoffered table's flag, held by the test: the audio role never waits on it, a thread without the role waits until
 * it clears. clap_host.c is compiled into this test, so the flag and the unsaid count are read where they live, and the
 * library exports nothing for it.
 *   clap_unoffered_test <fake.clap>
 */

#include <time.h>

#include "../src/clap_host.c"
#include "../src/idle_hold.c"

#define FAKE_PASSTHROUGH "org.omx-clap-host.test.passthrough"

static int g_failures;

#define CHECK(cond, ...) \
    do { \
        if (cond) { printf("ok   " __VA_ARGS__); printf("\n"); } \
        else { printf("FAIL " __VA_ARGS__); printf("\n"); g_failures++; } \
    } while (0)

struct asker
{
    struct omx_clap_instance *in;
    const char *id;
    pthread_barrier_t go;        ///< the asker's thread id is known to the main thread before it asks
    _Atomic int done;
    const void *answer;
};

static void *ask(void *arg)
{
    struct asker *a = arg;

    pthread_barrier_wait(&a->go);   // the main thread names this thread the audio role, or not, then lets it go
    pthread_barrier_wait(&a->go);
    a->answer = a->in->host.get_extension(&a->in->host, a->id);
    atomic_store(&a->done, 1);
    return NULL;
}

/* 1 when `done` reads 1 within `ms` */
static int done_within(_Atomic int *done, unsigned ms)
{
    const struct timespec step = { 0, 1000000 };

    while (ms--)
    {
        if (atomic_load(done))
            return 1;
        nanosleep(&step, NULL);
    }
    return atomic_load(done) != 0;
}

static int pending(struct omx_clap_instance *in, const char *id)
{
    size_t i;

    for (i = 0; i < UNOFFERED_SLOTS; i++)
        if (atomic_load(&g_unoffered[i].owner) == in && atomic_load(&g_unoffered[i].state) == UNOFFERED_PENDING
            && strcmp(g_unoffered[i].id, id) == 0)
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    struct omx_clap_instance *in = NULL;
    char why[OMX_CLAP_WHY_MAX];
    struct asker a;
    pthread_t thread;
    uint32_t unsaid;

    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <fake.clap>\n", argv[0]);
        return 2;
    }
    CHECK(omx_clap_host_open(argv[1], FAKE_PASSTHROUGH, &in, why) == 0 && in, "open %s (%s)", FAKE_PASSTHROUGH, in ? "" : why);
    if (!in)
        return 1;

    // the audio role, the flag held: back at once, NULL, the id counted unsaid and not claimed
    memset(&a, 0, sizeof(a));
    a.in = in;
    a.id = "clap.omx-test-audio-busy";
    pthread_barrier_init(&a.go, NULL, 2);
    pthread_create(&thread, NULL, ask, &a);
    pthread_barrier_wait(&a.go);
    omx_clap_host_set_audio_thread(in, thread);
    CHECK(!atomic_flag_test_and_set(&g_unoffered_lock), "the test holds the flag");
    unsaid = atomic_load(&g_unoffered_unsaid);
    pthread_barrier_wait(&a.go);
    CHECK(done_within(&a.done, 1000), "the audio role asks with the flag held: it returns without waiting");
    CHECK(a.answer == NULL, "the answer is NULL");
    CHECK(atomic_load(&g_unoffered_unsaid) == unsaid + 1, "the unsaid count went up by one (%u -> %u)", unsaid,
          atomic_load(&g_unoffered_unsaid));
    CHECK(!pending(in, a.id), "and the id is not in the table");
    atomic_flag_clear(&g_unoffered_lock);
    pthread_join(thread, NULL);
    pthread_barrier_destroy(&a.go);
    omx_clap_host_release_role(in, &(struct omx_clap_role){ 0 }, OMX_CLAP_IDLE);   // no thread holds the role now

    // a thread without the role, the flag held: it waits until the flag clears, then claims the id
    memset(&a, 0, sizeof(a));
    a.in = in;
    a.id = "clap.omx-test-main-waits";
    pthread_barrier_init(&a.go, NULL, 2);
    CHECK(!atomic_flag_test_and_set(&g_unoffered_lock), "the test holds the flag again");
    unsaid = atomic_load(&g_unoffered_unsaid);
    pthread_create(&thread, NULL, ask, &a);
    pthread_barrier_wait(&a.go);
    pthread_barrier_wait(&a.go);
    CHECK(!done_within(&a.done, 200), "a thread without the audio role asks with the flag held: still waiting after 200 ms");
    atomic_flag_clear(&g_unoffered_lock);
    pthread_join(thread, NULL);
    CHECK(atomic_load(&a.done) && a.answer == NULL, "the flag cleared: it returns NULL");
    CHECK(pending(in, a.id), "and the id is claimed, pending its line");
    CHECK(atomic_load(&g_unoffered_unsaid) == unsaid, "with nothing counted unsaid (%u)", atomic_load(&g_unoffered_unsaid));

    omx_clap_host_close(in);
    printf(g_failures ? "clap_unoffered: FAILED\n" : "clap_unoffered: all checks passed\n");
    return g_failures ? 1 : 0;
}
