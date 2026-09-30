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


/* Lists what CLAP plugin files hold: every plugin of every factory, with its
 * parameters, audio and note ports and latency, read from an initialised
 * instance that is never activated. Each file is scanned in a child process
 * so a plugin that crashes or hangs is reported and the scan goes on. */

/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "clap_host.h"


/*
************************************************************************************************************************
*           LOCAL DEFINES
************************************************************************************************************************
*/

#define VERSION             "0.1.0"
#define SCAN_TIMEOUT_S      30
#define REASON_SIZE         512
#define CLAP_SUFFIX         ".clap"


/*
************************************************************************************************************************
*           LOCAL DATA TYPES
************************************************************************************************************************
*/

typedef struct FLAG_NAME_T {
    uint32_t bit;
    const char *name;
} flag_name_t;

typedef struct LIST_T {
    char **items;
    size_t count;
} list_t;


/*
************************************************************************************************************************
*           LOCAL GLOBAL VARIABLES
************************************************************************************************************************
*/

static int g_json;

static const flag_name_t g_param_flags[] = {
    { CLAP_PARAM_IS_STEPPED, "stepped" },
    { CLAP_PARAM_IS_PERIODIC, "periodic" },
    { CLAP_PARAM_IS_HIDDEN, "hidden" },
    { CLAP_PARAM_IS_READONLY, "readonly" },
    { CLAP_PARAM_IS_BYPASS, "bypass" },
    { CLAP_PARAM_IS_AUTOMATABLE, "automatable" },
    { CLAP_PARAM_IS_AUTOMATABLE_PER_NOTE_ID, "automatable_per_note_id" },
    { CLAP_PARAM_IS_AUTOMATABLE_PER_KEY, "automatable_per_key" },
    { CLAP_PARAM_IS_AUTOMATABLE_PER_CHANNEL, "automatable_per_channel" },
    { CLAP_PARAM_IS_AUTOMATABLE_PER_PORT, "automatable_per_port" },
    { CLAP_PARAM_IS_MODULATABLE, "modulatable" },
    { CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID, "modulatable_per_note_id" },
    { CLAP_PARAM_IS_MODULATABLE_PER_KEY, "modulatable_per_key" },
    { CLAP_PARAM_IS_MODULATABLE_PER_CHANNEL, "modulatable_per_channel" },
    { CLAP_PARAM_IS_MODULATABLE_PER_PORT, "modulatable_per_port" },
    { CLAP_PARAM_REQUIRES_PROCESS, "requires_process" },
    { CLAP_PARAM_IS_ENUM, "enum" },
};

#define PARAM_FLAG_COUNT (sizeof(g_param_flags) / sizeof(g_param_flags[0]))


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS
************************************************************************************************************************
*/

/* the length of the UTF-8 sequence at s, 0 when it is not a valid one */
static int utf8_length(const unsigned char *s)
{
    if (s[0] < 0x80)
        return 1;
    if (s[0] >= 0xC2 && s[0] <= 0xDF && (s[1] & 0xC0) == 0x80)
        return 2;
    if (s[0] >= 0xE0 && s[0] <= 0xEF && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80)
        return (s[0] == 0xE0 && s[1] < 0xA0) || (s[0] == 0xED && s[1] >= 0xA0) ? 0 : 3;
    if (s[0] >= 0xF0 && s[0] <= 0xF4 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80)
        return (s[0] == 0xF0 && s[1] < 0x90) || (s[0] == 0xF4 && s[1] >= 0x90) ? 0 : 4;
    return 0;
}

/* a string as a JSON string, or bare in the text listing; a byte that is no UTF-8 becomes ? */
static void put_string(FILE *out, const char *s)
{
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    int n;

    if (g_json)
        fputc('"', out);
    while (*p)
    {
        n = utf8_length(p);
        if (n == 0)
        {
            fputc('?', out);
            p++;
        }
        else if (n > 1)
        {
            fwrite(p, 1, (size_t)n, out);
            p += n;
        }
        else
        {
            if (*p < 0x20 || *p == 0x7F)
            {
                if (g_json)
                    fprintf(out, "\\u%04x", *p);
                else
                    fputc('?', out);
            }
            else if (g_json && (*p == '"' || *p == '\\'))
                fprintf(out, "\\%c", *p);
            else
                fputc(*p, out);
            p++;
        }
    }
    if (g_json)
        fputc('"', out);
}

/* JSON has no infinity: a bound the plugin leaves open is null */
static void put_number(FILE *out, double value)
{
    if (isfinite(value))
        fprintf(out, "%.17g", value);
    else
        fputs(g_json ? "null" : "inf", out);
}

static void put_flags(FILE *out, uint32_t flags)
{
    size_t i;
    int first = 1;

    if (g_json)
        fputc('[', out);
    for (i = 0; i < PARAM_FLAG_COUNT; i++)
    {
        if (!(flags & g_param_flags[i].bit))
            continue;
        if (g_json)
            fprintf(out, "%s\"%s\"", first ? "" : ",", g_param_flags[i].name);
        else
            fprintf(out, "%s%s", first ? "" : ",", g_param_flags[i].name);
        first = 0;
    }
    if (g_json)
        fputc(']', out);
}

static void put_features(FILE *out, const char *const *features)
{
    int first = 1;

    if (g_json)
        fputc('[', out);
    for (; features && *features; features++)
    {
        if (!first)
            fputs(g_json ? "," : " ", out);
        put_string(out, *features);
        first = 0;
    }
    if (g_json)
        fputc(']', out);
}

static void put_params(FILE *out, const struct omx_clap_instance *instance)
{
    const clap_plugin_params_t *params = instance->params;
    clap_param_info_t info;
    uint32_t count = params ? params->count(instance->plugin) : 0;
    uint32_t i;
    int first = 1;

    if (g_json)
        fputs("\"params\":[", out);
    for (i = 0; i < count; i++)
    {
        memset(&info, 0, sizeof(info));
        if (!params->get_info(instance->plugin, i, &info))
            continue;
        info.name[sizeof(info.name) - 1] = '\0';
        info.module[sizeof(info.module) - 1] = '\0';
        if (g_json)
        {
            fprintf(out, "%s\n{\"id\":%u,\"name\":", first ? "" : ",", info.id);
            put_string(out, info.name);
            fputs(",\"module\":", out);
            put_string(out, info.module);
            fputs(",\"min\":", out);
            put_number(out, info.min_value);
            fputs(",\"max\":", out);
            put_number(out, info.max_value);
            fputs(",\"default\":", out);
            put_number(out, info.default_value);
            fputs(",\"flags\":", out);
            put_flags(out, info.flags);
            fputc('}', out);
        }
        else
        {
            fprintf(out, "    param %u ", info.id);
            put_string(out, info.name);
            fputs(" [", out);
            put_number(out, info.min_value);
            fputs(" .. ", out);
            put_number(out, info.max_value);
            fputs("] default ", out);
            put_number(out, info.default_value);
            fputc(' ', out);
            put_flags(out, info.flags);
            fputc('\n', out);
        }
        first = 0;
    }
    if (g_json)
        fputs(first ? "]" : "\n]", out);
}

static void put_audio_ports(FILE *out, const struct omx_clap_instance *instance)
{
    const clap_plugin_audio_ports_t *ports = instance->audio_ports;
    clap_audio_port_info_t info;
    uint32_t count, i;
    int dir;

    if (g_json)
        fputs("\"audio_ports\":{", out);
    for (dir = 0; dir < 2; dir++)
    {
        const bool is_input = dir == 0;
        int first = 1;

        count = ports ? ports->count(instance->plugin, is_input) : 0;
        if (g_json)
            fprintf(out, "%s\"%s\":[", dir ? "," : "", is_input ? "inputs" : "outputs");
        for (i = 0; i < count; i++)
        {
            memset(&info, 0, sizeof(info));
            if (!ports->get(instance->plugin, i, is_input, &info))
                continue;
            info.name[sizeof(info.name) - 1] = '\0';
            if (g_json)
            {
                fprintf(out, "%s{\"id\":%u,\"name\":", first ? "" : ",", info.id);
                put_string(out, info.name);
                fprintf(out, ",\"role\":\"%s\",\"channels\":%u}", (info.flags & CLAP_AUDIO_PORT_IS_MAIN) ? "main" : "aux",
                        info.channel_count);
            }
            else
            {
                fprintf(out, "    %s %u ", is_input ? "in " : "out", info.id);
                put_string(out, info.name);
                fprintf(out, " %s %u channels\n", (info.flags & CLAP_AUDIO_PORT_IS_MAIN) ? "main" : "aux", info.channel_count);
            }
            first = 0;
        }
        if (g_json)
            fputc(']', out);
    }
    if (g_json)
        fputc('}', out);
}

static void put_note_ports(FILE *out, const struct omx_clap_instance *instance)
{
    const clap_plugin_note_ports_t *ports = instance->note_ports;
    const uint32_t inputs = ports ? ports->count(instance->plugin, true) : 0;
    const uint32_t outputs = ports ? ports->count(instance->plugin, false) : 0;

    if (g_json)
        fprintf(out, "\"note_ports\":{\"inputs\":%u,\"outputs\":%u}", inputs, outputs);
    else
        fprintf(out, "    note ports %u in, %u out\n", inputs, outputs);
}

/* what a plugin that is only initialised answers; a plugin not yet activated may say 0, so 0 is left out as unknown */
static void put_latency(FILE *out, const struct omx_clap_instance *instance)
{
    uint32_t frames;

    if (!instance->latency)
        return;
    frames = instance->latency->get(instance->plugin);
    if (frames == 0)
        return;
    if (g_json)
        fprintf(out, ",\"latency\":%u", frames);
    else
        fprintf(out, "    latency %u frames\n", frames);
}

static void put_descriptor(FILE *out, const clap_plugin_descriptor_t *desc)
{
    if (g_json)
    {
        fputs("{\"id\":", out);
        put_string(out, desc->id);
        fputs(",\"name\":", out);
        put_string(out, desc->name);
        fputs(",\"vendor\":", out);
        put_string(out, desc->vendor);
        fputs(",\"version\":", out);
        put_string(out, desc->version);
        fputs(",\"description\":", out);
        put_string(out, desc->description);
        fputs(",\"url\":", out);
        put_string(out, desc->url);
        fputs(",\"features\":", out);
        put_features(out, desc->features);
    }
    else
    {
        fputs("  plugin ", out);
        put_string(out, desc->id);
        fputs(" \"", out);
        put_string(out, desc->name);
        fputs("\" vendor ", out);
        put_string(out, desc->vendor);
        fputs(" version ", out);
        put_string(out, desc->version);
        fputs("\n    features ", out);
        put_features(out, desc->features);
        fputc('\n', out);
    }
}

static void put_plugin(FILE *out, struct omx_clap_binary *binary, const clap_plugin_descriptor_t *desc)
{
    struct omx_clap_instance *instance;

    put_descriptor(out, desc);
    if (omx_clap_host_create(binary, desc, &instance) != 0)
    {
        if (g_json)
            fputs(",\"error\":\"plugin failed to create or init\"}", out);
        else
            fputs("    error: plugin failed to create or init\n", out);
        return;
    }

    if (g_json)
        fputc(',', out);
    put_params(out, instance);
    if (g_json)
        fputc(',', out);
    put_audio_ports(out, instance);
    if (g_json)
        fputc(',', out);
    put_note_ports(out, instance);
    put_latency(out, instance);
    if (g_json)
        fputc('}', out);
    omx_clap_host_close(instance);
}

/* one file's entry, written from the child */
static void put_file(FILE *out, const char *path)
{
    char reason[REASON_SIZE], local[PATH_MAX];
    struct omx_clap_binary *binary;
    uint32_t count, i;
    int first = 1;

    if (g_json)
    {
        fputs("{\"path\":", out);
        put_string(out, path);
    }
    else
    {
        fputs("file ", out);
        put_string(out, path);
        fputc('\n', out);
    }

    /* dlopen searches the library path for a name with no slash, and the file is here */
    if (!strchr(path, '/') && snprintf(local, sizeof(local), "./%s", path) < (int)sizeof(local))
        binary = omx_clap_host_binary_open(local, reason, sizeof(reason));
    else
        binary = omx_clap_host_binary_open(path, reason, sizeof(reason));
    if (!binary)
    {
        if (g_json)
        {
            fputs(",\"error\":", out);
            put_string(out, reason);
            fputc('}', out);
        }
        else
        {
            fputs("  error: ", out);
            put_string(out, reason);
            fputc('\n', out);
        }
        return;
    }

    if (g_json)
        fputs(",\"plugins\":[", out);
    count = omx_clap_host_binary_count(binary);
    for (i = 0; i < count; i++)
    {
        const clap_plugin_descriptor_t *desc = omx_clap_host_binary_descriptor(binary, i);
        if (!desc || !desc->id)
            continue;
        if (g_json)
            fputs(first ? "\n" : ",\n", out);
        put_plugin(out, binary, desc);
        first = 0;
    }
    if (g_json)
        fputs(first ? "]}" : "\n]}", out);
    omx_clap_host_binary_close(binary);
}

static void put_failure(FILE *out, const char *path, const char *reason)
{
    if (g_json)
    {
        fputs("{\"path\":", out);
        put_string(out, path);
        fputs(",\"error\":", out);
        put_string(out, reason);
        fputc('}', out);
    }
    else
    {
        fputs("file ", out);
        put_string(out, path);
        fputs("\n  error: ", out);
        put_string(out, reason);
        fputc('\n', out);
    }
}

static int read_all(int fd, char **data, size_t *size)
{
    size_t capacity = 65536;
    ssize_t n;

    *data = malloc(capacity);
    *size = 0;
    for (;;)
    {
        if (*size == capacity)
        {
            capacity *= 2;
            *data = realloc(*data, capacity);
        }
        n = read(fd, *data + *size, capacity - *size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return n < 0 ? -1 : 0;
        *size += (size_t)n;
    }
}

/* the file scanned in a child, its entry written to out; a child that dies or hangs is the reason */
static void scan_file(FILE *out, const char *path)
{
    char reason[REASON_SIZE];
    int fds[2], status = 0;
    char *data;
    size_t size;
    pid_t pid;

    fflush(out);
    if (pipe(fds) != 0)
    {
        put_failure(out, path, "no pipe for the scan");
        return;
    }
    pid = fork();
    if (pid < 0)
    {
        close(fds[0]);
        close(fds[1]);
        put_failure(out, path, "no process for the scan");
        return;
    }
    if (pid == 0)
    {
        FILE *pipe_out;

        close(fds[0]);
        dup2(STDERR_FILENO, STDOUT_FILENO);
        alarm(SCAN_TIMEOUT_S);
        pipe_out = fdopen(fds[1], "w");
        put_file(pipe_out, path);
        fflush(pipe_out);
        _exit(0);
    }
    close(fds[1]);
    read_all(fds[0], &data, &size);
    close(fds[0]);
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && size > 0)
        fwrite(data, 1, size, out);
    else if (WIFSIGNALED(status))
    {
        if (WTERMSIG(status) == SIGALRM)
            snprintf(reason, sizeof(reason), "timed out after %i s", SCAN_TIMEOUT_S);
        else
            snprintf(reason, sizeof(reason), "crashed with signal %i (%s)", WTERMSIG(status), strsignal(WTERMSIG(status)));
        put_failure(out, path, reason);
    }
    else
        put_failure(out, path, "the scan ended without a result");
    free(data);
}

static void list_add(list_t *list, char *item)
{
    list->items = realloc(list->items, (list->count + 1) * sizeof(char *));
    list->items[list->count++] = item;
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int has_suffix(const char *name, const char *suffix)
{
    const size_t n = strlen(name), m = strlen(suffix);
    return n > m && strcmp(name + n - m, suffix) == 0;
}

/* the .clap files under a directory, in name order; a directory that can't be read goes on the failures list */
static void walk(const char *dir, list_t *files, list_t *failures)
{
    list_t names = { NULL, 0 };
    struct dirent *entry;
    struct stat st;
    DIR *handle = opendir(dir);
    size_t i;

    if (!handle)
    {
        char *failure;
        if (asprintf(&failure, "%s\t%s", dir, strerror(errno)) > 0)
            list_add(failures, failure);
        return;
    }
    while ((entry = readdir(handle)) != NULL)
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
            list_add(&names, strdup(entry->d_name));
    closedir(handle);
    qsort(names.items, names.count, sizeof(char *), compare_names);

    for (i = 0; i < names.count; i++)
    {
        char *path;
        if (asprintf(&path, "%s/%s", dir, names.items[i]) < 0)
            continue;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            walk(path, files, failures);
        else if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && has_suffix(names.items[i], CLAP_SUFFIX))
            list_add(files, path);
        else
            free(path);
        free(names.items[i]);
    }
    free(names.items);
}

/* CLAP_PATH, then the per-user and system directories */
static void default_paths(list_t *paths)
{
    const char *env = getenv("CLAP_PATH");
    const char *home = getenv("HOME");
    char *copy, *save, *dir;

    if (env && *env)
    {
        copy = strdup(env);
        for (dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save))
            list_add(paths, strdup(dir));
        free(copy);
    }
    if (home && *home)
    {
        char *user;
        if (asprintf(&user, "%s/.clap", home) > 0)
            list_add(paths, user);
    }
    list_add(paths, strdup("/usr/lib64/clap"));
    list_add(paths, strdup("/usr/lib/clap"));
}

static int seen_before(const list_t *seen, const char *path)
{
    char resolved[PATH_MAX];
    size_t i;

    if (!realpath(path, resolved))
        return 0;
    for (i = 0; i < seen->count; i++)
        if (strcmp(seen->items[i], resolved) == 0)
            return 1;
    return 0;
}

static void remember(list_t *seen, const char *path)
{
    char resolved[PATH_MAX];

    if (realpath(path, resolved))
        list_add(seen, strdup(resolved));
}

static void usage(const char *name)
{
    fprintf(stderr,
            "usage: %s [--json] [<path>...]\n"
            "  <path> is a .clap file or a directory searched for .clap files;\n"
            "  none given: CLAP_PATH, then ~/.clap, /usr/lib64/clap and /usr/lib/clap\n",
            name);
}


/*
************************************************************************************************************************
*           MAIN
************************************************************************************************************************
*/

int main(int argc, char **argv)
{
    list_t roots = { NULL, 0 }, files = { NULL, 0 }, failures = { NULL, 0 }, seen = { NULL, 0 };
    int explicit_paths, readable = 0, first = 1, a;
    struct stat st;
    size_t i;

    for (a = 1; a < argc; a++)
    {
        if (strcmp(argv[a], "--json") == 0)
            g_json = 1;
        else if (strcmp(argv[a], "-h") == 0 || strcmp(argv[a], "--help") == 0)
        {
            usage(argv[0]);
            return 0;
        }
        else if (argv[a][0] == '-' && argv[a][1] != '\0')
        {
            usage(argv[0]);
            return 2;
        }
        else
            list_add(&roots, strdup(argv[a]));
    }

    explicit_paths = roots.count > 0;
    if (!explicit_paths)
        default_paths(&roots);

    for (i = 0; i < roots.count; i++)
    {
        if (stat(roots.items[i], &st) != 0)
        {
            if (explicit_paths)
            {
                char *failure;
                if (asprintf(&failure, "%s\t%s", roots.items[i], strerror(errno)) > 0)
                    list_add(&failures, failure);
            }
            continue;
        }
        if (S_ISDIR(st.st_mode))
        {
            const size_t failed = failures.count;
            walk(roots.items[i], &files, &failures);
            if (failures.count == failed || files.count > 0)
                readable++;
        }
        else
        {
            list_add(&files, strdup(roots.items[i]));
            readable++;
        }
    }

    if (g_json)
        printf("{\"scanner\":\"omx-clap-scan\",\"version\":\"%s\",\"files\":[", VERSION);
    for (i = 0; i < files.count; i++)
    {
        if (seen_before(&seen, files.items[i]))
            continue;
        remember(&seen, files.items[i]);
        if (g_json)
            fputs(first ? "\n" : ",\n", stdout);
        scan_file(stdout, files.items[i]);
        first = 0;
    }
    for (i = 0; i < failures.count; i++)
    {
        char *reason = strchr(failures.items[i], '\t');
        *reason++ = '\0';
        if (g_json)
            fputs(first ? "\n" : ",\n", stdout);
        put_failure(stdout, failures.items[i], reason);
        first = 0;
    }
    if (g_json)
        fputs(first ? "]}\n" : "\n]}\n", stdout);

    if (readable == 0)
    {
        fprintf(stderr, "%s: no path could be read\n", argv[0]);
        return 1;
    }
    return 0;
}
