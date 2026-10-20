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

/* The scanner, run as a program: what it lists for tests/fake.clap, a .clap
 * that is no library, one that crashes its process, a directory walk and a
 * path that can't be read. Arguments: omx-clap-scan, fake.clap, crash.clap
 * and optionally omx-delay.clap (parameter 0 = time, 5 = its bypass) and
 * fake_synth.clap. */

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

/* whether the entry of plugin org.omx-clap-host.test.<id> (up to the next plugin's) holds `text` */
static int plugin_has(const char *out, const char *id, const char *text)
{
    char key[128];
    const char *a, *b, *t;

    snprintf(key, sizeof(key), "\"id\":\"org.omx-clap-host.test.%s\"", id);
    if (!(a = strstr(out, key)))
        return 0;
    b = strstr(a + strlen(key), "\"id\":\"org.omx-clap-host.test.");
    t = strstr(a, text);
    return t && (!b || t < b);
}

static void fake_checks(const char *scan, const char *fake, char *out)
{
    int status = run(out, "%s --json %s", scan, fake);

    CHECK(status == 0, "scan of fake.clap exits 0 (%i)", status);
    CHECK(out[0] == '{' && has(out, "\"scanner\":\"omx-clap-scan\""), "one JSON document with the scanner named");
    CHECK(count(out, "\"id\":\"org.omx-clap-host.test.") == 23, "the twenty-three plugins of the factory are listed (%i)",
          count(out, "\"id\":\"org.omx-clap-host.test."));
    CHECK(has(out, "\"id\":\"org.omx-clap-host.test.passthrough\",\"name\":\"passthrough\",\"vendor\":\"omx-clap-host\","
                   "\"version\":\"0\""), "descriptor id, name, vendor, version");
    CHECK(has(out, "\"features\":[\"audio-effect\"]"), "features");
    CHECK(count(out, "{\"id\":0,\"name\":\"latency\",\"module\":\"\",\"min\":0,\"max\":4096,\"default\":64,"
                     "\"flags\":[\"stepped\"]}") == 23, "each plugin's parameter: id, name, module, min, max, default, flags");
    CHECK(has(out, "\"audio_ports\":{\"inputs\":[{\"id\":0,\"name\":\"in 0\",\"role\":\"main\",\"channels\":6,\"kind\":\"unknown\","
                   "\"jack\":[\"in_1\",\"in_2\",\"in_3\",\"in_4\",\"in_5\",\"in_6\"]}]"),
          "the wide plugin's 6-channel main input, of no port type, a JACK port per channel");
    CHECK(has(out, "{\"id\":1,\"name\":\"in 1\",\"role\":\"aux\",\"channels\":1,\"kind\":\"mono\",\"jack\":[\"aux_in_1_1\"]}"),
          "the sidechain plugin's mono aux input, aux_in_1_1");
    CHECK(has(out, "{\"id\":1,\"name\":\"in 1\",\"role\":\"aux\",\"channels\":4,\"kind\":\"unknown\","
                   "\"jack\":[\"aux_in_1_1\",\"aux_in_1_2\",\"aux_in_1_3\",\"aux_in_1_4\"]}"), "the aux-wide plugin's 4-channel aux input");
    CHECK(has(out, "{\"id\":1,\"name\":\"out 1\",\"role\":\"aux\",\"channels\":2,\"kind\":\"stereo\",\"jack\":[\"aux_out_1_1\",\"aux_out_1_2\"]}"),
          "the auxout plugin's stereo aux output, aux_out_1_1 and aux_out_1_2");
    CHECK(plugin_has(out, "many-aux", "\"name\":\"in 65\"") && !plugin_has(out, "many-aux", "\"jack\"") && !has(out, "aux_in_65_"),
          "many-aux, refused too-many-ports: none of its 65 aux inputs is named");
    CHECK(plugin_has(out, "too-wide", "\"name\":\"in 0\"") && !plugin_has(out, "too-wide", "\"jack\""),
          "too-wide, refused port-too-wide: its admissible main pair is not named either");
    CHECK(plugin_has(out, "two-mains", "\"name\":\"out 0\"") && !plugin_has(out, "two-mains", "\"jack\""),
          "two-mains, refused too-many-ports: no port named");
    CHECK(plugin_has(out, "aux-max", "\"jack\":[\"aux_in_64_1\","), "aux-max, admitted: its 64th aux input is named");
    CHECK(has(out, "\"role\":\"aux\",\"channels\":65,\"kind\":\"unknown\"}"), "too-wide: a 65-channel port has no JACK port");
    CHECK(count(out, "{\"id\":0,\"name\":\"out 0\",\"role\":\"main\",\"channels\":6,\"kind\":\"surround\",\"map\":[0,1,2,3,9,10],\"jack\":[\"out_1\",\"out_2\",\"out_3\",\"out_4\",\"out_5\",\"out_6\"]}") == 1,
          "the surround plugin's 5.1 map by clap.surround, SL and SR as 9 and 10 (%i)",
          count(out, "{\"id\":0,\"name\":\"out 0\",\"role\":\"main\",\"channels\":6,\"kind\":\"surround\",\"map\":[0,1,2,3,9,10],\"jack\":[\"out_1\",\"out_2\",\"out_3\",\"out_4\",\"out_5\",\"out_6\"]}"));
    CHECK(count(out, "{\"id\":0,\"name\":\"out 0\",\"role\":\"main\",\"channels\":6,\"kind\":\"surround\",\"map\":[0,1,2,3,4,5],\"jack\":[\"out_1\",\"out_2\",\"out_3\",\"out_4\",\"out_5\",\"out_6\"]}") == 1,
          "the plain surround plugin's map in index order, no clap.surround (%i)",
          count(out, "{\"id\":0,\"name\":\"out 0\",\"role\":\"main\",\"channels\":6,\"kind\":\"surround\",\"map\":[0,1,2,3,4,5],\"jack\":[\"out_1\",\"out_2\",\"out_3\",\"out_4\",\"out_5\",\"out_6\"]}"));
    CHECK(has(out, "\"kind\":\"surround\",\"map\":[0,1,2,3,9,25]}"), "surround-bad's map, its id 25 as the plugin names it, and no JACK name: refused");
    CHECK(has(out, "{\"id\":0,\"name\":\"in 0\",\"role\":\"main\",\"channels\":4,\"kind\":\"ambisonic\",\"ordering\":\"acn\","
                   "\"normalization\":\"sn3d\",\"jack\":[\"in_1\",\"in_2\",\"in_3\",\"in_4\"]}"),
          "the ambisonic plugin's ordering and normalisation by clap.ambisonic");
    CHECK(!has(out, "\"assumed\""), "a configuration every plugin states is not marked assumed");
    CHECK(count(out, "\"note_ports\":{\"inputs\":1,\"outputs\":0}") == 1, "the note input of exactly one plugin");
    CHECK(!has(out, "\"presets\""), "fake.clap has no preset-discovery factory: no presets key");
    CHECK(count(out, "\"latency\":64") == 23, "latency 64 read without activation (%i)", count(out, "\"latency\":64"));
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
    CHECK(has(out, "\"code\":\"dlopen\""), "and the code dlopen");
    CHECK(!has(out, "\"plugins\""), "and lists no plugins");

    status = run(out, "%s --json %s", scan, crash);
    CHECK(status == 0, "a plugin that aborts its process: exit 0 (%i)", status);
    CHECK(has(out, "\"error\":\"crashed with signal 6"), "the crash is the reason");
    CHECK(has(out, "\"code\":\"crash:6\""), "and the code crash:6");

    snprintf(path, sizeof(path), "%s/sub", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/a_crash.clap", dir);
    CHECK(symlink(crash, path) == 0, "a_crash.clap links to the crashing plugin");
    snprintf(path, sizeof(path), "%s/sub/z_fake.clap", dir);
    CHECK(symlink(fake, path) == 0, "sub/z_fake.clap links to the fake plugin");
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
    CHECK(has(out, "\"path\":\"/nonexistent/clap\",\"code\":\"unreadable\",\"error\":\"No such file or directory\""), "and the path is named in the document, code unreadable");

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
    CHECK(has(out, "\n  plugin org.omx-clap-host.test.wide \"wide\" vendor omx-clap-host"), "the plugin's name quoted");
    CHECK(has(out, "\n    in  aux 1 channels mono \"in 1\" aux_in_1_1\n"), "a port per line: role, channels, kind, name, JACK names");
    CHECK(has(out, "\n    out main 6 channels surround map 0 1 2 3 9 10 \"out 0\" out_1 out_2 out_3 out_4 out_5 out_6\n"),
          "a surround port's map before its name");
    CHECK(has(out, "\n    in  main 4 channels ambisonic acn sn3d \"in 0\" in_1 in_2 in_3 in_4\n"), "an ambisonic port's ordering and normalisation");
    CHECK(!has(out, " assumed "), "without --json the ambisonic port's ordering and normalisation, stated, not assumed");
    status = run(out, "FAKE_AMBISONIC_SILENT=1 %s %s", scan, fake);
    CHECK(status == 0 && has(out, "\n    in  main 4 channels ambisonic acn sn3d assumed \"in 0\" in_1 in_2 in_3 in_4\n"),
          "without --json an ambisonic port whose get_config answers false reads acn sn3d assumed (%i)", status);
    status = run(out, "FAKE_AMBISONIC_SILENT=1 %s --json %s", scan, fake);
    CHECK(status == 0 && has(out, "{\"id\":0,\"name\":\"in 0\",\"role\":\"main\",\"channels\":4,\"kind\":\"ambisonic\",\"ordering\":\"acn\","
                                  "\"normalization\":\"sn3d\",\"assumed\":true,\"jack\":[\"in_1\",\"in_2\",\"in_3\",\"in_4\"]}"),
          "an ambisonic port whose get_config answers false: ACN and SN3D, \"assumed\":true, from the core's description (%i)", status);
}

/* the synths: an instrument feature, no audio input, one note input with the dialects the port declares, and a note
 * output on the arpeggiators */
static void synth_checks(const char *scan, const char *synth, char *out)
{
    int status = run(out, "%s --json %s", scan, synth);

    CHECK(status == 0 && !has(out, "\"error\""), "fake_synth.clap scans (%i)", status);
    CHECK(count(out, "\"id\":\"org.omx-clap-host.test.") == 12, "its twelve plugins are listed (%i)",
          count(out, "\"id\":\"org.omx-clap-host.test."));
    CHECK(has(out, "\"id\":\"org.omx-clap-host.test.synth\",\"name\":\"synth\"") && has(out, "\"features\":[\"instrument\",\"synthesizer\"]"),
          "the synth is an instrument");
    CHECK(has(out, "\"audio_ports\":{\"inputs\":[],\"outputs\":[{\"id\":0,\"name\":\"out 0\",\"role\":\"main\",\"channels\":2,\"kind\":\"stereo\","
                   "\"jack\":[\"out_1\",\"out_2\"]}]}"),
          "with no audio input and a stereo main output");
    CHECK(count(out, "\"note_ports\":{\"inputs\":1,\"outputs\":0}") == 8, "one note input on eight of them (%i)",
          count(out, "\"note_ports\":{\"inputs\":1,\"outputs\":0}"));
    CHECK(count(out, "\"note_ports\":{\"inputs\":2,\"outputs\":0}") == 1, "two on synth-notes (%i)",
          count(out, "\"note_ports\":{\"inputs\":2,\"outputs\":0}"));
    CHECK(count(out, "\"note_ports\":{\"inputs\":1,\"outputs\":1}") == 2, "a note input and a note output on the two arpeggiators (%i)",
          count(out, "\"note_ports\":{\"inputs\":1,\"outputs\":1}"));
    // synth-poly names two keys (a third its get refuses is left out) and declares 4 of 8 voices, read in the scan's one
    // activation; no other plugin lists either
    CHECK(has(out, "\"note_ports\":{\"inputs\":1,\"outputs\":0},\"note_names\":[{\"name\":\"Kick\",\"port\":-1,\"key\":36,\"channel\":-1},"
                   "{\"name\":\"Snare \\\"rim\\\"\",\"port\":0,\"key\":38,\"channel\":9}]"),
          "synth-poly's note names: name, port, key and channel, the quote escaped, the refused third left out");
    CHECK(count(out, "\"note_names\":") == 1, "only synth-poly lists note names (%i)", count(out, "\"note_names\":"));
    CHECK(has(out, "\"voice_info\":{\"count\":4,\"capacity\":8,\"overlapping_notes\":true}") && count(out, "\"voice_info\":") == 1,
          "synth-poly's voice info, 4 of 8 voices with overlapping notes, and no other plugin's (%i)", count(out, "\"voice_info\":"));
    // the file's preset-discovery factory: its PLUGIN location's two presets, with name, location and load key
    CHECK(has(out, "\"presets\":[\n{\"name\":\"Init\",\"location_kind\":\"plugin\",\"location\":null,\"load_key\":\"init\","
                   "\"provider\":\"org.omx-clap-host.test.presets\",\"plugins\":[\"org.omx-clap-host.test.synth-poly\"],\"flags\":[\"factory\"]},\n"
                   "{\"name\":\"Bright \\\"pad\\\"\",\"location_kind\":\"plugin\",\"location\":null,\"load_key\":\"bright\","
                   "\"provider\":\"org.omx-clap-host.test.presets\",\"plugins\":[\"org.omx-clap-host.test.synth-poly\"],"
                   "\"flags\":[\"factory\",\"favorite\"]}\n]}"),
          "the file's two presets: name, location kind and location, load key, provider, plugin and flags");
    CHECK(count(out, "\"presets\":") == 1, "listed once, for the file (%i)", count(out, "\"presets\":"));
    status = run(out, "%s %s", scan, synth);
    CHECK(status == 0 && has(out, "  preset \"Init\" plugin - key init for org.omx-clap-host.test.synth-poly\n"),
          "the presets in the listing for a person");
    CHECK(status == 0 && has(out, "    note name \"Kick\" port -1 key 36 channel -1\n") && has(out, "    voices 4 of 8, overlapping notes\n"),
          "and in the listing for a person (%i)", status);
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
        fprintf(stderr, "usage: %s <omx-clap-scan> <fake.clap> <crash.clap> [omx-delay.clap | -] [fake_synth.clap]\n", argv[0]);
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
    if (argc > 5)
        synth_checks(scan, argv[5], out);

    printf("%s\n", g_failures == 0 ? "clap scan test ok" : "clap scan test FAILED");
    free(out);
    return g_failures == 0 ? 0 : 1;
}
