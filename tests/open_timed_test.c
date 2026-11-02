// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// omx_clap_host_open_timed after its timeout, under ThreadSanitizer (make test-open-timed): a copy of the fake named
// slow.clap sleeps in its entry's init (FAKE_SLOW_INIT_MS), so the open is abandoned. The caller then frees the path and
// the id it passed, as it may, and opens and closes the fake on its own thread while the abandoned open goes on, both
// changing the core's list of binaries; when the late open ends, the instance it made, which nobody will take, is closed
// (the fake logs its destroy). ThreadSanitizer halts on a read of the freed strings or an unordered change of the list.

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../src/clap_host.h"
#include "test_util.h"

#define SLOW_MS     300
#define TIMEOUT_MS  50

static int file_copy(const char *from_path, const char *to_path)
{
    FILE *from = fopen(from_path, "rb"), *to = fopen(to_path, "wb");
    char buf[65536];
    size_t n;
    int ok = from && to;

    while (ok && (n = fread(buf, 1, sizeof(buf), from)) > 0)
        ok = fwrite(buf, 1, n, to) == n;
    if (from)
        fclose(from);
    if (to && fclose(to) != 0)
        ok = 0;
    return ok;
}

// the lines of `log` that are exactly `line`
static unsigned lines_of(const char *log, const char *line)
{
    FILE *f = fopen(log, "r");
    char buf[512];
    unsigned n = 0;

    if (!f)
        return 0;
    while (fgets(buf, sizeof(buf), f))
    {
        buf[strcspn(buf, "\n")] = '\0';
        n += strcmp(buf, line) == 0;
    }
    fclose(f);
    return n;
}

int main(int argc, char **argv)
{
    char dir[] = "/tmp/omx-clap-open-timed.XXXXXX";
    char slow[PATH_MAX], log[PATH_MAX], why[OMX_CLAP_WHY_MAX];
    struct omx_clap_instance *in = NULL, *other = NULL;
    char *path, *id;
    unsigned waited;
    int rc, i;

    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <fake.clap>\n", argv[0]);
        return 2;
    }
    if (!mkdtemp(dir))
        return 2;
    snprintf(slow, sizeof(slow), "%s/slow.clap", dir);
    snprintf(log, sizeof(log), "%s/fake.log", dir);
    if (!file_copy(argv[1], slow))
        return 2;
    setenv("FAKE_LOG", log, 1);
    setenv("FAKE_SLOW_INIT_MS", "300", 1);    // SLOW_MS

    // the caller's own strings, freed as soon as the call returns
    path = strdup(slow);
    id = strdup(FAKE_PASSTHROUGH);
    rc = omx_clap_host_open_timed(path, id, TIMEOUT_MS, &in, why);
    CHECK(rc == -1 && !in, "a %u ms open of a plugin whose init takes %u ms is abandoned (%d, %s)", TIMEOUT_MS, SLOW_MS, rc, why);
    memset(path, 'x', strlen(path));
    memset(id, 'x', strlen(id));
    free(path);
    free(id);

    // meanwhile the caller goes on: the fake opened and closed, the list of binaries changed on this thread
    for (i = 0; i < 4; i++)
    {
        other = NULL;
        CHECK(omx_clap_host_open(argv[1], FAKE_PASSTHROUGH, &other, why) == 0 && other, "the fake opens while the late open runs (%d)", i);
        if (other)
            omx_clap_host_close(other);
    }

    // the late open ends: what it made is closed, since nobody takes it
    for (waited = 0; waited < 5000 && lines_of(log, FAKE_PASSTHROUGH " destroy") < 5; waited += 10)
        usleep(10000);
    CHECK(lines_of(log, FAKE_PASSTHROUGH " init") == 5, "five instances were made, the late one among them (%u)", lines_of(log, FAKE_PASSTHROUGH " init"));
    CHECK(lines_of(log, FAKE_PASSTHROUGH " destroy") == 5, "and five destroyed: the late open closed the instance nobody took (%u)",
          lines_of(log, FAKE_PASSTHROUGH " destroy"));
    CHECK(omx_clap_host_binaries_open() == 0, "no binary is left open (%u)", omx_clap_host_binaries_open());

    unlink(slow);
    unlink(log);
    rmdir(dir);
    return report("open_timed ok");
}
