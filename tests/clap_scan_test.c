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
 * and optionally omx-delay.clap (parameter 0 = time, 5 = its bypass),
 * fake_synth.clap, the LV2 fixtures and fake_panel.clap. */

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

/* every scanner runs from a scratch directory: a plugin the walk of the installed directories loads may write into
 * its host's working directory (clap-protoplug drops "protoplug last compile.lua" there, #300), and that must never
 * be the tree */
static char g_scratch[PATH_MAX];

static int run(char *out, const char *format, ...)
{
    char command[4096 + PATH_MAX + 16], rest[4096], inner[4096];
    va_list args;
    FILE *pipe;
    size_t n;
    int status;

    va_start(args, format);
    vsnprintf(inner, sizeof(inner), format, args);
    va_end(args);
    snprintf(command, sizeof(command), "cd '%s' && %s", g_scratch, inner);
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

/* the file name, in the scratch directory, a command's stderr is sent to with 2>, and its text after the run */
#define ERR_FILE "scan.err"
static char g_err[65536];

static const char *err_text(void)
{
    char path[PATH_MAX + 16];
    FILE *file;
    size_t n = 0;

    snprintf(path, sizeof(path), "%s/" ERR_FILE, g_scratch);
    file = fopen(path, "r");
    if (file)
    {
        n = fread(g_err, 1, sizeof(g_err) - 1, file);
        fclose(file);
    }
    g_err[n] = '\0';
    return g_err;
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

static void broken_checks(const char *scan, const char *fake, const char *crash, const char *synth, char *out)
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

    // a bundle directory (#259): a .clap directory with no .clap file inside is its one shared object, whatever its name;
    // two objects are an ambiguity named with the code bundle; a directory of other files is nothing, as before
    snprintf(path, sizeof(path), "%s/odd.clap", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/odd.clap/libodd_name.so", dir);
    CHECK(symlink(synth, path) == 0, "odd.clap/libodd_name.so links to the synth fixture (a symlink to a plugin already listed is skipped, so not the fake)");
    snprintf(path, sizeof(path), "%s/odd.clap/readme.txt", dir);
    file = fopen(path, "w");
    fclose(file);
    snprintf(path, sizeof(path), "%s/two.clap", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/two.clap/liba.so", dir);
    CHECK(symlink(fake, path) == 0, "two.clap/liba.so links to the fake plugin");
    snprintf(path, sizeof(path), "%s/two.clap/libb.so", dir);
    CHECK(symlink(fake, path) == 0, "and two.clap/libb.so too");
    status = run(out, "%s --json %s", scan, dir);
    CHECK(status == 0, "a walk over bundle directories: exit 0 (%i)", status);
    CHECK(count(out, "{\"path\":") == 5, "the three files, the bundle's object and the ambiguous bundle (%i rows)", count(out, "{\"path\":"));
    CHECK(has(out, "odd.clap/libodd_name.so\"") && has(out, "\"org.omx-clap-host.test.synth\""), "odd.clap is listed by its object, which lists the synth");
    CHECK(has(out, "two.clap\",\"code\":\"bundle\",\"error\":\"2 shared objects, one expected\""), "two.clap is a failure, code bundle, the count named");
    CHECK(!has(out, "liba.so") && !has(out, "libb.so"), "and neither of its objects is listed on its own");
    snprintf(path, sizeof(path), "%s/two.clap/libb.so", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/two.clap/liba.so", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/two.clap", dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/odd.clap/readme.txt", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/odd.clap/libodd_name.so", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/odd.clap", dir);
    rmdir(path);

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
    CHECK(has(out, "\"id\":\"org.openmixer.delay\""), "its plugin id");
    CHECK(count(out, "{\"id\":") >= 6, "its parameters and ports (%i ids)", count(out, "{\"id\":"));
    CHECK(has(out, "\"flags\":[") && has(out, "\"bypass\""), "the bypass parameter carries its flag");
}

/* the field dump: the records plugin-hostd's format tables read, from the fake plugins and the LV2 host fixture
 * (`lv2_fixtures` is the fixture directory put on LV2_PATH, or "-" for none: no LV2 check then) */
static void fields_checks(const char *scan, const char *fake, const char *crash, const char *synth, const char *lv2_fixtures, char *out)
{
    int status = run(out, "%s --fields clap:%s", scan, fake);

    CHECK(status == 2 && out[0] == '\0', "a file named without a plugin id is a usage error, nothing dumped (%i)", status);
    status = run(out, "%s --fields clap:%s#org.omx-clap-host.test.none 2>/dev/null", scan, fake);
    CHECK(status == 1 && out[0] == '\0', "an id the file lacks is refused (%i)", status);
    status = run(out, "%s --fields lv2 2>/dev/null", scan);
    CHECK(status == 2, "a target of neither form is a usage error (%i)", status);
    status = run(out, "%s --fields clap:%s#org.omx-clap-host.test.passthrough", scan, fake);
    CHECK(status == 0 && strncmp(out, "# omx-clap-scan --fields 1\n", 27) == 0, "a CLAP dump opens with the format's version line (%i)", status);
    CHECK(has(out, "\nplugin\tclap_entry\tfake.clap\n"), "its provenance is the file's name");
    CHECK(has(out, "\nplugin\tclap_plugin_descriptor.id\torg.omx-clap-host.test.passthrough\n") && has(out, "\nplugin\tclap_plugin_descriptor.vendor\t"),
          "the descriptor's fields as <struct>.<member>");
    CHECK(has(out, "\nplugin\tclap_plugin_descriptor.features\tCLAP_PLUGIN_FEATURE_AUDIO_EFFECT\n"), "a feature by its macro name");
    CHECK(has(out, "\nplugin\tclap_plugin.get_extension\tCLAP_EXT_PARAMS\n") && has(out, "\nplugin\tclap_plugin.get_extension\tCLAP_EXT_LATENCY\n")
          && !has(out, "CLAP_EXT_VOICE_INFO"),
          "the extensions it answers, by their macro names, and none it does not");
    CHECK(has(out, "\nparam:0\tclap_param_info.name\tlatency\n") && has(out, "\nparam:0\tclap_param_info.min_value\t0\n"),
          "a parameter's fields under param:<index>");
    CHECK(count(out, "\tclap_param_info.flags\tCLAP_PARAM_IS_STEPPED\n") == 4 && count(out, "\tclap_param_info.flags\tCLAP_PARAM_IS_READONLY\n") == 3,
          "one record per flag bit set (%i stepped, %i readonly)",
          count(out, "\tclap_param_info.flags\tCLAP_PARAM_IS_STEPPED\n"), count(out, "\tclap_param_info.flags\tCLAP_PARAM_IS_READONLY\n"));
    CHECK(has(out, "\naudio_in:0\tclap_audio_port_info.flags\tCLAP_AUDIO_PORT_IS_MAIN\n") && has(out, "\naudio_out:0\tclap_audio_port_info.port_type\tCLAP_PORT_STEREO\n")
          && has(out, "\naudio_out:0\tclap_audio_port_info.channel_count\t2\n"),
          "an audio port's flags and type by name, its count as a number");
    CHECK(!has(out, "note_in:") && !has(out, "clap_voice_info"), "no note port and no voice info on an effect");
    CHECK(has(out, "\nplugin\tclap_plugin_latency.get\t64\n"), "the latency it answers before activation");
    status = run(out, "%s --fields clap:%s#org.omx-clap-host.test.synth-poly", scan, synth);
    CHECK(status == 0 && has(out, "\nplugin\tclap_plugin_descriptor.features\tCLAP_PLUGIN_FEATURE_INSTRUMENT\n"), "the synth is an instrument (%i)", status);
    CHECK(has(out, "\nnote_in:0\tclap_note_port_info.supported_dialects\tCLAP_NOTE_DIALECT_CLAP\n")
          && has(out, "\nnote_in:0\tclap_note_port_info.preferred_dialect\tCLAP_NOTE_DIALECT_CLAP\n"),
          "its note input's dialects by name");
    CHECK(has(out, "\nplugin\tclap_voice_info.voice_count\t4\n") && has(out, "\nplugin\tclap_voice_info.voice_capacity\t8\n")
          && has(out, "\nplugin\tclap_voice_info.flags\tCLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES\n"),
          "synth-poly's voices, read in the dump's one activation");
    status = run(out, "%s --fields clap:%s#org.omx-clap-host.test.synth-mpe", scan, synth);
    CHECK(status == 0 && has(out, "\nnote_in:0\tclap_note_port_info.supported_dialects\tCLAP_NOTE_DIALECT_MIDI_MPE\n") && !has(out, "clap_voice_info"),
          "synth-mpe reads MIDI with MPE and has no voice info (%i)", status);
    status = run(out, "%s --fields clap:%s#x 2>" ERR_FILE, scan, crash);
    CHECK(status == 1 && out[0] == '\0', "a plugin that aborts in its entry takes the child, not the scanner, and leaves no dump (%i)", status);
    CHECK(has(err_text(), "--fields: clap:") && has(g_err, ": crashed with signal 6"), "the reason on stderr: crashed with signal 6");
    status = run(out, "OMX_CRASH_MODE=hang OMX_CLAP_SCAN_FIELDS_TIMEOUT=1 %s --fields clap:%s#x 2>" ERR_FILE, scan, crash);
    CHECK(status == 1 && out[0] == '\0', "a plugin that hangs in its entry is ended by the child's timeout, no dump (%i)", status);
    CHECK(has(err_text(), ": timed out after 1 s"), "the reason on stderr: timed out after 1 s");
    status = run(out, "OMX_CRASH_MODE=exit0 %s --fields clap:%s#x 2>" ERR_FILE, scan, crash);
    CHECK(status == 1 && out[0] == '\0', "a plugin that ends its process with status 0 in its entry is no dump (%i)", status);
    CHECK(has(err_text(), "ended before its dump was whole"), "the reason on stderr: the process ended before its dump was whole");
    if (strcmp(lv2_fixtures, "-") == 0)
        return;
    status = run(out, "LV2_PATH=%s %s --fields lv2:urn:openmixer:test:host-fixture", lv2_fixtures, scan);
    CHECK(status == 0 && strncmp(out, "# omx-clap-scan --fields 1\n", 27) == 0, "an LV2 dump, from the fixture's data alone, opens with the version line (%i)", status);
    CHECK(has(out, "\nplugin\trdfs:seeAlso\tomx-host-fixture.lv2/omx-host-fixture.ttl\n"), "its provenance is the bundle directory and the data file, not the manifest");
    CHECK(has(out, "\nplugin\tdoap:name\topenmixer host fixture\n") && has(out, "\nplugin\trdf:type\tlv2:AmplifierPlugin\n"), "the plugin's name and classes");
    CHECK(has(out, "\nplugin\tlv2:requiredFeature\turid:map\n") && has(out, "\nplugin\tlv2:extensionData\tstate:interface\n"), "features and extension data as CURIEs");
    CHECK(count(out, "\tlv2:symbol\t") == 6 && has(out, "\nport:3\tlv2:symbol\tgain_db\n"), "one port scope per port by lv2:index (%i)", count(out, "\tlv2:symbol\t"));
    CHECK(has(out, "\nport:3\tlv2:minimum\t-60\n") && has(out, "\nport:3\tlv2:default\t-20\n") && has(out, "\nport:2\tlv2:maximum\t1\n"), "a range as numbers");
    CHECK(has(out, "\nport:2\tlv2:portProperty\tlv2:toggled\n") && has(out, "\nport:2\tlv2:designation\tlv2:enabled\n")
          && has(out, "\nport:5\tlv2:portProperty\tlv2:reportsLatency\n") && has(out, "\nport:5\tlv2:designation\tlv2:latency\n"),
          "properties and designations as CURIEs");
    CHECK(has(out, "\nport:0\trdf:type\tlv2:AudioPort\n") && has(out, "\nport:0\trdf:type\tlv2:InputPort\n"), "a port's classes");
    status = run(out, "LV2_PATH=%s %s --fields lv2:urn:openmixer:test:none 2>/dev/null", lv2_fixtures, scan);
    CHECK(status == 1 && out[0] == '\0', "a URI no bundle holds is refused (%i)", status);
}

/* the panel description fake_panel.clap's panel plugin publishes (org.openmixer.panel-description/1), as --json and
 * --fields print it, value by value; its panel-none plugin and every plugin of fake.clap serve none; its panel-hostile
 * plugin counts two tables past OMX_CLAP_PANEL_DESCRIPTION_MAX_ENTRIES (4096), which the scan refuses */
static void panel_checks(const char *scan, const char *fake, const char *panel, char *out)
{
    int status = run(out, "%s --json %s", scan, panel);

    CHECK(status == 0 && !has(out, "\"error\""), "fake_panel.clap scans (%i)", status);
    CHECK(plugin_has(out, "panel","\"panel\":{\"family\":\"dynamics\","
                                     "\"fingerprint\":\"5f2c1e0a9b8d7c6e5f4a3b2c1d0e9f8a7b6c5d4e3f2a1b0c9d8e7f6a5b4c3d2e\","),
          "the panel's family by its word and its fingerprint as the plugin gives it");
    CHECK(has(out, ",\"sections\":[{\"key\":\"main\",\"labelKey\":\"panel.section.main\",\"label\":\"Main\"},"
                   "{\"key\":\"detail\",\"labelKey\":\"panel.section.detail\",\"label\":\"Detail\"}],"),
          "its two sections in order: key, label key, label");
    CHECK(has(out, ",\"controls\":[{\"paramId\":0,\"section\":0,\"widget\":\"fader\",\"labelKey\":\"panel.threshold\",\"label\":\"Threshold\","
                   "\"visibleParamId\":null,\"visibleMask\":\"0\"},"),
          "control 0: threshold, a fader in main, always visible");
    CHECK(has(out, "},{\"paramId\":1,\"section\":1,\"widget\":\"switch\",\"labelKey\":\"panel.mode\",\"label\":\"Mode\","
                   "\"visibleParamId\":null,\"visibleMask\":\"0\"},"),
          "control 1: mode, a switch in detail");
    CHECK(has(out, "},{\"paramId\":2,\"section\":1,\"widget\":\"knob\",\"labelKey\":\"panel.knee\",\"label\":\"Knee\","
                   "\"visibleParamId\":1,\"visibleMask\":\"6\"}],"),
          "control 2: knee, a knob in detail shown while mode is 1 or 2 (parameter 1, mask 6)");
    CHECK(has(out, ",\"roles\":[{\"role\":\"threshold\",\"index\":0,\"paramId\":0}],"), "its one role: threshold, parameter 0");
    CHECK(has(out, ",\"displays\":[{\"kind\":\"gain_reduction_bar\",\"section\":0,\"source\":\"meters\",\"meterIds\":[7,null],"
                   "\"curveIndex\":0,\"floorParamId\":null,\"floorDb\":-24,\"labelKey\":\"panel.display.gain_reduction\","
                   "\"label\":\"Gain reduction\"}],"),
          "its one display: a gain reduction bar over main from meter 7, a -24 dB floor no parameter sets");
    CHECK(has(out, ",\"placementDefaults\":[]}"), "a table the plugin leaves empty is empty, nothing filled in");
    CHECK(count(out, "\"panel\":{") == 2 && plugin_has(out, "panel-none", "\"panel\":null"),
          "panel-none serves no description: \"panel\":null (%i described)", count(out, "\"panel\":{"));
    CHECK(plugin_has(out, "panel-hostile", "\"controls\":{\"refused\":\"too-many\",\"count\":4294967295,\"max\":4096},"
                                           "\"roles\":{\"refused\":\"too-many\",\"count\":4097,\"max\":4096},"),
          "panel-hostile's controls counted UINT32_MAX and its roles 4097 are refused, each with its count and the maximum");
    CHECK(plugin_has(out, "panel-hostile", "\"label\":\"Detail\"}],\"controls\":{\"refused\"")
          && plugin_has(out, "panel-hostile", "},\"displays\":[{\"kind\":\"gain_reduction_bar\",")
          && plugin_has(out, "panel-hostile", ",\"placementDefaults\":[]}") && !has(out, "out of memory"),
          "and its sections, displays and placement defaults still print, nothing reported out of memory");
    status = run(out, "%s --json %s", scan, fake);
    CHECK(status == 0 && count(out, "\"panel\":null") == 23, "and none of fake.clap's 23 plugins does (%i null)", count(out, "\"panel\":null"));
    status = run(out, "%s %s", scan, panel);
    CHECK(status == 0 && has(out, "\n    panel dynamics, 2 sections, 3 controls, 1 roles, 1 displays, 0 placement defaults\n")
          && count(out, "    panel ") == 2,
          "the listing for a person counts the panel's tables, for the plugins that have one (%i)", status);
    CHECK(has(out, "\n    panel dynamics, 2 sections, controls refused (4294967295 past 4096), roles refused (4097 past 4096), "
                   "1 displays, 0 placement defaults\n"),
          "and names the tables it refused");

    status = run(out, "%s --fields clap:%s#org.omx-clap-host.test.panel", scan, panel);
    CHECK(status == 0 && has(out, "\nplugin\tclap_plugin.get_extension\torg.openmixer.panel-description/1\n"),
          "the dump names the extension the plugin answers (%i)", status);
    CHECK(has(out, "\npanel\tomx_clap_plugin_panel_description.family\tOMX_CLAP_FAMILY_DYNAMICS\n")
          && has(out, "\npanel\tomx_clap_plugin_panel_description.fingerprint\t5f2c1e0a9b8d7c6e5f4a3b2c1d0e9f8a7b6c5d4e3f2a1b0c9d8e7f6a5b4c3d2e\n"),
          "the family by its enumerator and the fingerprint under panel");
    CHECK(has(out, "\nsection:0\tomx_clap_panel_section.key\tmain\nsection:0\tomx_clap_panel_section.label_key\tpanel.section.main\n"
                   "section:0\tomx_clap_panel_section.label\tMain\n")
          && has(out, "\nsection:1\tomx_clap_panel_section.key\tdetail\nsection:1\tomx_clap_panel_section.label_key\tpanel.section.detail\n"
                      "section:1\tomx_clap_panel_section.label\tDetail\n"),
          "each section under section:<index>");
    CHECK(has(out, "\ncontrol:0\tomx_clap_panel_control.param_id\t0\ncontrol:0\tomx_clap_panel_control.section\t0\n"
                   "control:0\tomx_clap_panel_control.widget\tOMX_CLAP_WIDGET_FADER\ncontrol:0\tomx_clap_panel_control.label_key\tpanel.threshold\n"
                   "control:0\tomx_clap_panel_control.label\tThreshold\ncontrol:1\t"),
          "control 0: threshold, a fader in section 0, no visibility rule stated");
    CHECK(has(out, "\ncontrol:1\tomx_clap_panel_control.param_id\t1\ncontrol:1\tomx_clap_panel_control.section\t1\n"
                   "control:1\tomx_clap_panel_control.widget\tOMX_CLAP_WIDGET_SWITCH\n"),
          "control 1: mode, a switch in section 1");
    CHECK(has(out, "\ncontrol:2\tomx_clap_panel_control.widget\tOMX_CLAP_WIDGET_KNOB\ncontrol:2\tomx_clap_panel_control.label_key\tpanel.knee\n"
                   "control:2\tomx_clap_panel_control.label\tKnee\ncontrol:2\tomx_clap_panel_control.visible_param_id\t1\n"
                   "control:2\tomx_clap_panel_control.visible_mask\t6\n"),
          "control 2: knee, a knob, shown while parameter 1 is 1 or 2 (mask 6)");
    CHECK(count(out, "visible_param_id") == 1, "the visibility rule is stated once, for the control that has one (%i)",
          count(out, "visible_param_id"));
    CHECK(has(out, "\nrole:0\tomx_clap_panel_role.role\tthreshold\nrole:0\tomx_clap_panel_role.index\t0\nrole:0\tomx_clap_panel_role.param_id\t0\n"),
          "the role under role:0");
    CHECK(has(out, "\ndisplay:0\tomx_clap_panel_display.kind\tOMX_CLAP_DISPLAY_GAIN_REDUCTION_BAR\ndisplay:0\tomx_clap_panel_display.section\t0\n"
                   "display:0\tomx_clap_panel_display.source\tOMX_CLAP_DISPLAY_FROM_METERS\ndisplay:0\tomx_clap_panel_display.meter_ids[0]\t7\n"
                   "display:0\tomx_clap_panel_display.curve_index\t0\ndisplay:0\tomx_clap_panel_display.floor_db\t-24\n"
                   "display:0\tomx_clap_panel_display.label_key\tpanel.display.gain_reduction\n"
                   "display:0\tomx_clap_panel_display.label\tGain reduction\n"),
          "the display under display:0, no second meter and no floor parameter stated");
    CHECK(!has(out, "placement_default:"), "no placement default record: the plugin's table is empty");
    CHECK(count(out, "\nsection:") == 6 && count(out, "\ncontrol:") == 17 && count(out, "\nrole:") == 3 && count(out, "\ndisplay:") == 8,
          "and no record more: 6 section, 17 control, 3 role, 8 display (%i, %i, %i, %i)",
          count(out, "\nsection:"), count(out, "\ncontrol:"), count(out, "\nrole:"), count(out, "\ndisplay:"));
    status = run(out, "%s --fields clap:%s#org.omx-clap-host.test.panel-none", scan, panel);
    CHECK(status == 0 && !has(out, "\npanel\t") && !has(out, "panel-description") && !has(out, "\nsection:") && !has(out, "\ncontrol:"),
          "a plugin that serves none has no panel record (%i)", status);
    CHECK(has(out, "\nparam:2\tclap_param_info.name\tknee\n"), "and its parameters are dumped as before");
    status = run(out, "%s --fields clap:%s#org.omx-clap-host.test.panel-hostile", scan, panel);
    CHECK(status == 0 && has(out, "\npanel\tomx_clap_plugin_panel_description.count\tcontrols 4294967295 refused, past 4096\n")
          && has(out, "\npanel\tomx_clap_plugin_panel_description.count\troles 4097 refused, past 4096\n"),
          "the dump records each refused table with its count and the maximum (%i)", status);
    CHECK(!has(out, "\ncontrol:") && !has(out, "\nrole:") && count(out, "\nsection:") == 6 && count(out, "\ndisplay:") == 8,
          "no entry of a refused table, the other tables whole (%i section, %i display)", count(out, "\nsection:"),
          count(out, "\ndisplay:"));
}

int main(int argc, char **argv)
{
    char *out;
    char scan[PATH_MAX];

    if (argc < 6)
    {
        fprintf(stderr, "usage: %s <omx-clap-scan> <fake.clap> <crash.clap> <omx-delay.clap | -> <fake_synth.clap> [<lv2 fixtures dir> | - [<fake_panel.clap>]]\n", argv[0]);
        return 2;
    }
    if (!realpath(argv[1], scan))
    {
        fprintf(stderr, "%s: no scanner at %s\n", argv[0], argv[1]);
        return 2;
    }
    snprintf(g_scratch, sizeof(g_scratch), "/tmp/clap_scan_cwd_XXXXXX");
    if (!mkdtemp(g_scratch))
    {
        fprintf(stderr, "%s: no scratch directory\n", argv[0]);
        return 2;
    }
    out = malloc(BUFFER_SIZE);

    fake_checks(scan, argv[2], out);
    relative_checks(scan, argv[2], out);
    broken_checks(scan, argv[2], argv[3], argv[5], out);
    unreadable_checks(scan, argv[2], out);
    text_checks(scan, argv[2], out);
    if (argc > 4 && strcmp(argv[4], "-") != 0)
        delay_checks(scan, argv[4], out);
    if (argc > 5)
        synth_checks(scan, argv[5], out);
    fields_checks(scan, argv[2], argv[3], argv[5], argc > 6 ? argv[6] : "-", out);
    if (argc > 7)
        panel_checks(scan, argv[2], argv[7], out);

    printf("%s\n", g_failures == 0 ? "clap scan test ok" : "clap scan test FAILED");
    free(out);
    {
        /* the scratch directory and whatever a plugin left in it */
        char command[PATH_MAX + 32];
        snprintf(command, sizeof(command), "rm -rf '%s'", g_scratch);
        if (system(command) != 0)
            fprintf(stderr, "%s: the scratch directory %s stays\n", argv[0], g_scratch);
    }
    return g_failures == 0 ? 0 : 1;
}
