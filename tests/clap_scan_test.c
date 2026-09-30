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

/* The scanner, run as a program: what it lists for tests/fake.clap, a .clap
 * that is no library, one that crashes its process, a directory walk and a
 * path that can't be read. Arguments: omx-clap-scan, fake.clap, crash.clap
 * and optionally omx-delay.clap (parameter 0 = time, 5 = its bypass). */

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define BUFFER_SIZE     (1024 * 1024)

static int g_failures;

#define CHECK(cond, ...) \
    do { \
        if (cond) { printf("ok   " __VA_ARGS__); printf("\n"); } \
        else { printf("FAIL " __VA_ARGS__); printf("\n"); g_failures++; } \
    } while (0)

/* run a command, its stdout in out, its exit status returned */
static int run(char *out, const char *format, ...) __attribute__((format(printf, 2, 3)));

static int run(char *out, const char *format, ...)
{
    char command[4096], rest[4096];
    va_list args;
    FILE *pipe;
    size_t n;
    int status;

    va_start(args, format);
    vsnprintf(command, sizeof(command), format, args);
    va_end(args);
    pipe = popen(command, "r");
    if (!pipe)
        return -1;
    n = fread(out, 1, BUFFER_SIZE - 1, pipe);
    out[n] = '\0';
    while (fread(rest, 1, sizeof(rest), pipe) > 0)
        ;
    status = pclose(pipe);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int has(const char *out, const char *text)
{
    return strstr(out, text) != NULL;
}

static int count(const char *out, const char *text)
{
    int n = 0;
    const char *p = out;

    while ((p = strstr(p, text)) != NULL)
    {
        n++;
        p += strlen(text);
    }
    return n;
}

static void fake_checks(const char *scan, const char *fake, char *out)
{
    int status = run(out, "%s --json %s", scan, fake);

    CHECK(status == 0, "scan of fake.clap exits 0 (%i)", status);
    CHECK(out[0] == '{' && has(out, "\"scanner\":\"omx-clap-scan\""), "one JSON document with the scanner named");
    CHECK(count(out, "\"id\":\"org.omx-clap-host.test.") == 4, "the four plugins of the factory are listed (%i)",
          count(out, "\"id\":\"org.omx-clap-host.test."));
    CHECK(has(out, "\"id\":\"org.omx-clap-host.test.passthrough\",\"name\":\"passthrough\",\"vendor\":\"omx-clap-host\","
                   "\"version\":\"0\""), "descriptor id, name, vendor, version");
    CHECK(has(out, "\"features\":[\"audio-effect\"]"), "features");
    CHECK(count(out, "{\"id\":0,\"name\":\"latency\",\"module\":\"\",\"min\":0,\"max\":4096,\"default\":64,"
                     "\"flags\":[\"stepped\"]}") == 4, "each plugin's parameter: id, name, module, min, max, default, flags");
    CHECK(has(out, "\"audio_ports\":{\"inputs\":[{\"id\":0,\"name\":\"in 0\",\"role\":\"main\",\"channels\":4}]"),
          "the wide plugin's 4-channel main input");
    CHECK(has(out, "{\"id\":1,\"name\":\"in 1\",\"role\":\"aux\",\"channels\":1}"), "the sidechain plugin's aux input");
    CHECK(count(out, "\"note_ports\":{\"inputs\":1,\"outputs\":0}") == 1, "the note input of exactly one plugin");
    CHECK(count(out, "\"latency\":64") == 4, "latency 64 read without activation (%i)", count(out, "\"latency\":64"));
    CHECK(!has(out, "\"error\""), "no error on a clean file");
}

/* a bare file name is a file in the working directory, not a library on the loader's path */
static void relative_checks(const char *scan, const char *fake, char *out)
{
    char dir[512];
    const char *slash = strrchr(fake, '/');
    int status;

    snprintf(dir, sizeof(dir), "%.*s", (int)(slash - fake), fake);
    status = run(out, "cd %s && %s --json %s", dir, scan, slash + 1);
    CHECK(status == 0, "a bare file name: exit 0 (%i)", status);
    CHECK(has(out, "test.passthrough") && !has(out, "\"error\""), "found in the working directory");
}

static void broken_checks(const char *scan, const char *fake, const char *crash, char *out)
{
    char dir[] = "/tmp/clap_scan_test_XXXXXX";
    char path[512];
    FILE *file;
    int status;

    CHECK(mkdtemp(dir) != NULL, "a scratch directory");
    snprintf(path, sizeof(path), "%s/broken.clap", dir);
    file = fopen(path, "w");
    fputs("this is not a shared library\n", file);
    fclose(file);

    status = run(out, "%s --json %s", scan, path);
    CHECK(status == 0, "a .clap that is no library: exit 0 (%i)", status);
    CHECK(has(out, "\"error\":\"can't open ") && has(out, "broken.clap"), "it is refused with the reason");
    CHECK(!has(out, "\"plugins\""), "and lists no plugins");

    status = run(out, "%s --json %s", scan, crash);
    CHECK(status == 0, "a plugin that aborts its process: exit 0 (%i)", status);
    CHECK(has(out, "\"error\":\"crashed with signal 6"), "the crash is the reason");

    snprintf(path, sizeof(path), "%s/sub", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/a_crash.clap", dir);
    symlink(crash, path);
    snprintf(path, sizeof(path), "%s/sub/z_fake.clap", dir);
    symlink(fake, path);
    snprintf(path, sizeof(path), "%s/notes.txt", dir);
    file = fopen(path, "w");
    fclose(file);

    status = run(out, "%s --json %s", scan, dir);
    CHECK(status == 0, "a directory walk: exit 0 (%i)", status);
    CHECK(count(out, "{\"path\":") == 3, "three .clap files found, notes.txt skipped (%i)", count(out, "{\"path\":"));
    CHECK(has(out, "a_crash.clap") && has(out, "broken.clap") && has(out, "sub/z_fake.clap"), "each by its path");
    CHECK(strstr(out, "a_crash.clap") < strstr(out, "broken.clap") && strstr(out, "broken.clap") < strstr(out, "z_fake.clap"),
          "in name order, the subdirectory walked in place");
    CHECK(has(out, "test.passthrough") && has(out, "crashed with signal"), "the scan went on past the crash and the broken file");

    snprintf(path, sizeof(path), "%s/sub/z_fake.clap", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/sub", dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/a_crash.clap", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/broken.clap", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/notes.txt", dir);
    unlink(path);
    rmdir(dir);
}

static const char *dirname_of(const char *path)
{
    static char dir[512];
    char *slash;

    snprintf(dir, sizeof(dir), "%s", path);
    slash = strrchr(dir, '/');
    if (slash)
        *slash = '\0';
    return dir;
}

static void unreadable_checks(const char *scan, const char *fake, char *out)
{
    int status = run(out, "%s --json /nonexistent/clap 2>/dev/null", scan);

    CHECK(status == 1, "no readable path: exit 1 (%i)", status);
    CHECK(has(out, "\"path\":\"/nonexistent/clap\",\"error\":\"No such file or directory\""), "and the path is named in the document");

    status = run(out, "%s --json /nonexistent/clap %s 2>/dev/null", scan, fake);
    CHECK(status == 0, "one readable path among the unreadable: exit 0 (%i)", status);
    CHECK(has(out, "\"error\":\"No such file or directory\"") && has(out, "test.passthrough"), "both reported");

    status = run(out, "CLAP_PATH=%s %s --json", dirname_of(fake), scan);
    CHECK(status == 0 && has(out, "test.passthrough"), "no path given: CLAP_PATH is searched first (%i)", status);
}

static void text_checks(const char *scan, const char *fake, char *out)
{
    int status = run(out, "%s %s", scan, fake);

    CHECK(status == 0 && has(out, "plugin org.omx-clap-host.test.wide") && has(out, "param 0 latency"),
          "without --json a listing for a person");
}

static void delay_checks(const char *scan, const char *delay, char *out)
{
    int status = run(out, "%s --json %s", scan, delay);

    CHECK(status == 0, "omx-delay.clap scans (%i)", status);
    CHECK(has(out, "\"id\":\"org.freemixer.openmixer.delay\""), "its plugin id");
    CHECK(count(out, "{\"id\":") >= 6, "its parameters and ports (%i ids)", count(out, "{\"id\":"));
    CHECK(has(out, "\"flags\":[") && has(out, "\"bypass\""), "the bypass parameter carries its flag");
}

int main(int argc, char **argv)
{
    char *out;
    char scan[PATH_MAX];

    if (argc < 4)
    {
        fprintf(stderr, "usage: %s <omx-clap-scan> <fake.clap> <crash.clap> [omx-delay.clap]\n", argv[0]);
        return 2;
    }
    if (!realpath(argv[1], scan))
    {
        fprintf(stderr, "%s: no scanner at %s\n", argv[0], argv[1]);
        return 2;
    }
    out = malloc(BUFFER_SIZE);

    fake_checks(scan, argv[2], out);
    relative_checks(scan, argv[2], out);
    broken_checks(scan, argv[2], argv[3], out);
    unreadable_checks(scan, argv[2], out);
    text_checks(scan, argv[2], out);
    if (argc > 4 && strcmp(argv[4], "-") != 0)
        delay_checks(scan, argv[4], out);

    printf("%s\n", g_failures == 0 ? "clap scan test ok" : "clap scan test FAILED");
    free(out);
    return g_failures == 0 ? 0 : 1;
}
