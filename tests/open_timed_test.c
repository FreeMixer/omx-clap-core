// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// omx_clap_host_open_timed after its timeout, under ThreadSanitizer (make test-open-timed): a copy of the fake named
// slow.clap sleeps in its entry's init (FAKE_SLOW_INIT_MS), so the open is abandoned. The caller then frees the path and
// the id it passed, as it may, and opens and closes the fake on its own thread while the abandoned open goes on, both
// changing the core's list of binaries; when the late open ends, the instance it made, which nobody will take, is closed
// and its binary let go. ThreadSanitizer halts on a read of the freed strings or an unordered change of the list.

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

int main(int argc, char **argv)
{
    char dir[] = "/tmp/omx-clap-open-timed.XXXXXX";
    char slow[PATH_MAX], why[OMX_CLAP_WHY_MAX];
    struct omx_clap_instance *in = NULL, *other = NULL, *keep = NULL;
    char *path, *id;
    unsigned waited, opens, failed = 0;
    int rc;

    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <fake.clap>\n", argv[0]);
        return 2;
    }
    if (!mkdtemp(dir))
        return 2;
    snprintf(slow, sizeof(slow), "%s/slow.clap", dir);
    if (!file_copy(argv[1], slow))
        return 2;
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

    // meanwhile the caller goes on, for three times the init the late open waits for: one instance of the fake held
    // throughout, so its binary stays listed, and the fake opened and closed over and over, each open reading the list
    // of binaries on this thread while the late open adds its own to it and takes it away. Nothing here touches a file
    // the late open's thread does: ThreadSanitizer takes a reused file descriptor for an order between the two
    CHECK(omx_clap_host_open(argv[1], FAKE_PASSTHROUGH, &keep, why) == 0 && keep, "the fake held open while the late open runs");
    opens = 0;
    for (waited = 0; waited < 3 * SLOW_MS; waited += 10)
    {
        other = NULL;
        if (omx_clap_host_open(argv[1], FAKE_PASSTHROUGH, &other, why) == 0 && other)
        {
            opens++;
            omx_clap_host_close(other);
        }
        else
            failed++;
        usleep(10000);
    }
    CHECK(opens > 0 && failed == 0, "the fake opened and closed %u times while the late open ran (%u refused)", opens, failed);
    CHECK(omx_clap_host_binaries_open() == 1,
          "and let go when it ended: the late open closed the instance nobody took (%u binaries open, the fake's alone wanted)",
          omx_clap_host_binaries_open());
    if (keep)
        omx_clap_host_close(keep);
    CHECK(omx_clap_host_binaries_open() == 0, "no binary is left open (%u)", omx_clap_host_binaries_open());

    unlink(slow);
    rmdir(dir);
    return report("open_timed ok");
}
