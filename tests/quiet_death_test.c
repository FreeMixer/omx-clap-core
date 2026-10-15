/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * This file is part of omx-clap-host.
 *
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 */

/*
 * quiet_death.h: a host that crashes dies of the same signal, and writes no core.
 *
 * Each case is a child that aborts, faults or raises a crash signal. A control child without the handler tells whether
 * this machine writes cores at all (core limit raised to its hard maximum): where it does not, the "no core" checks
 * cannot tell the handler from the machine and are skipped, said so; the "same signal" checks run everywhere.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "quiet_death.h"

static int g_failed;

#define CHECK(cond, ...) do { if (cond) { printf("ok   "); } else { printf("FAIL "); g_failed = 1; } printf(__VA_ARGS__); printf("\n"); } while (0)

enum how { HOW_ABORT, HOW_NULL_WRITE, HOW_RAISE_SEGV, HOW_RAISE_ABRT_AND_GO_ON };

static void cores_on(void)
{
    struct rlimit rl;

    if (getrlimit(RLIMIT_CORE, &rl) == 0)
    {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_CORE, &rl);
    }
}

/* the child's end: one of the ways a plugin takes its host down */
static void die(enum how how)
{
    volatile int *volatile nowhere = NULL;

    switch (how)
    {
        case HOW_ABORT:
            abort();
        case HOW_NULL_WRITE:
            *nowhere = 1;
            break;
        case HOW_RAISE_SEGV:
            raise(SIGSEGV);
            break;
        case HOW_RAISE_ABRT_AND_GO_ON:
            // a plugin that raises SIGABRT itself, not through abort(): the handler must not let it carry on
            raise(SIGABRT);
            break;
    }
    _exit(0);
}

/* the wait status of a child that installs the handler (or not) and then dies `how` */
static int run(int install, const char *keep, enum how how)
{
    int status = 0;
    pid_t pid = fork();

    if (pid == 0)
    {
        cores_on();
        if (keep)
            setenv(QUIET_DEATH_KEEP_CORES_ENV, keep, 1);
        else
            unsetenv(QUIET_DEATH_KEEP_CORES_ENV);
        if (install)
            quiet_death_install();
        die(how);
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return status;
}

int main(void)
{
    static const struct { enum how how; int sig; const char *name; } cases[] = {
        { HOW_ABORT, SIGABRT, "abort()" },
        { HOW_NULL_WRITE, SIGSEGV, "a write through NULL" },
        { HOW_RAISE_SEGV, SIGSEGV, "raise(SIGSEGV)" },
        { HOW_RAISE_ABRT_AND_GO_ON, SIGABRT, "raise(SIGABRT) with code after it" },
    };
    int control = run(0, NULL, HOW_ABORT);
    const int machine_dumps = WIFSIGNALED(control) && WCOREDUMP(control);
    size_t i;

    CHECK(WIFSIGNALED(control) && WTERMSIG(control) == SIGABRT, "control: abort() without the handler dies of SIGABRT");
    if (!machine_dumps)
        printf("skip this machine writes no core even without the handler: the no-core checks cannot tell here\n");

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        int status = run(1, NULL, cases[i].how);

        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == cases[i].sig, "%s: the host dies of signal %d (status 0x%x)",
              cases[i].name, cases[i].sig, (unsigned)status);
        if (machine_dumps)
            CHECK(!WCOREDUMP(status), "%s: and writes no core", cases[i].name);
    }

    // the opt-out: OMX_CLAP_HOST_CORE_DUMPS=1 leaves the core where the machine writes one, "0" does not
    {
        int kept = run(1, "1", HOW_ABORT);
        int zero = run(1, "0", HOW_ABORT);

        CHECK(WIFSIGNALED(kept) && WTERMSIG(kept) == SIGABRT, "%s=1: abort() still dies of SIGABRT", QUIET_DEATH_KEEP_CORES_ENV);
        CHECK(WIFSIGNALED(zero) && WTERMSIG(zero) == SIGABRT, "%s=0: abort() still dies of SIGABRT", QUIET_DEATH_KEEP_CORES_ENV);
        if (machine_dumps)
        {
            CHECK(WCOREDUMP(kept), "%s=1: the core is kept", QUIET_DEATH_KEEP_CORES_ENV);
            CHECK(!WCOREDUMP(zero), "%s=0: no core", QUIET_DEATH_KEEP_CORES_ENV);
        }
    }

    printf(g_failed ? "quiet death test FAILED\n" : "quiet death test ok\n");
    return g_failed;
}
