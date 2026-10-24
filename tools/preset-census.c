/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * preset-census: which plugins of a .clap file offer presets, and through which system.
 *
 *   preset-census [--timeout SECONDS] FILE|DIRECTORY...
 *
 * A directory is walked as omx-clap-scan walks it: its .clap files, its subdirectories, and a .clap bundle directory
 * through its one object.
 *
 * For every file and every plugin id it lists, one TSV line (the header first):
 *
 *   file  plugin_id  name  has_discovery_factory  providers  locations  extensions  presets_counted  has_preset_load
 *   has_state  error  file_presets_unnamed  presets_factory
 *
 * The preset-discovery factory is read as omx-clap-scan's design reads it (plugin-presets spec §2c): every provider is
 * created with an indexer that records declare_filetype, declare_location and declare_soundpack, init is called, and
 * get_metadata runs once per declared location (a PLUGIN location with a NULL location; a FILE location walked for the
 * declared extensions). A preset is counted under every plugin id of this file it names; one that names none of them
 * is counted in file_presets_unnamed. Unlike the scanner, the count is not capped. presets_factory counts the ones whose
 * flags (the preset's own, else its location's) say CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT.
 *
 * locations is "plugin:N/file:M" over every provider; providers the number the factory declares; extensions the
 * declared file extensions, comma separated ("*" for a filetype with no extension).
 *
 * has_preset_load and has_state come from an instance made by the core's omx_clap_host_create: init, never activate.
 *
 * Isolation: the file's plugin list, its preset crawl and each plugin's instance run in a child process of their own,
 * killed after the timeout (10 s by default); a child that crashes or hangs names itself in the error column and the
 * rest of the run goes on. Nothing here starts audio.
 */

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <clap/clap.h>

#include "clap_host.h"

#define CENSUS_IDS        256u
#define CENSUS_LOCATIONS  64u
#define CENSUS_EXTS       32u
#define CENSUS_DEPTH      16u
#define CENSUS_OUT_MAX    (1u << 20)

static int g_timeout = 10;
static FILE *g_out;     // a child's results: its own descriptor, so a plugin that prints to stdout cannot mix into them

/* ---------------------------------------------------------------------------------------------------------------- */
/* the preset crawl, in the child                                                                                    */

struct census_location
{
    uint32_t kind, flags;
    char *path;
};

struct census
{
    clap_preset_discovery_indexer_t indexer;
    clap_preset_discovery_metadata_receiver_t receiver;

    // the file's plugin ids, and the presets counted under each
    const char *ids[CENSUS_IDS];
    unsigned long counts[CENSUS_IDS], factory[CENSUS_IDS];
    uint32_t n_ids;
    unsigned long unnamed;

    // what the providers declared, over all of them
    uint32_t providers, plugin_locations, file_locations;
    char exts[CENSUS_EXTS][32];
    uint32_t n_exts;
    int any_ext;

    // the provider being crawled
    struct census_location locations[CENSUS_LOCATIONS];
    uint32_t n_locations;
    char p_exts[CENSUS_EXTS][32];
    uint32_t p_n_exts;
    int p_any_ext;

    // the preset under way
    int open;
    int named[CENSUS_IDS];
    int named_any;
    uint32_t location_flags, flags;
    int has_flags;
};

static void preset_end(struct census *c)
{
    uint32_t i;

    if (!c->open)
        return;
    c->open = 0;
    if (!c->named_any)
    {
        c->unnamed++;
        return;
    }
    for (i = 0; i < c->n_ids; i++)
        if (c->named[i])
        {
            c->counts[i]++;
            if ((c->has_flags ? c->flags : c->location_flags) & CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT)
                c->factory[i]++;
        }
}

static bool idx_filetype(const clap_preset_discovery_indexer_t *indexer, const clap_preset_discovery_filetype_t *ft)
{
    struct census *c = indexer->indexer_data;
    const char *ext;
    uint32_t i;

    if (!ft)
        return false;
    ext = ft->file_extension;
    if (!ext || !ext[0])
    {
        c->p_any_ext = c->any_ext = 1;
        return true;
    }
    if (ext[0] == '.')
        ext++;
    if (c->p_n_exts < CENSUS_EXTS)
        snprintf(c->p_exts[c->p_n_exts++], sizeof(c->p_exts[0]), "%s", ext);
    for (i = 0; i < c->n_exts; i++)
        if (!strcmp(c->exts[i], ext))
            return true;
    if (c->n_exts < CENSUS_EXTS)
        snprintf(c->exts[c->n_exts++], sizeof(c->exts[0]), "%s", ext);
    return true;
}

static bool idx_location(const clap_preset_discovery_indexer_t *indexer, const clap_preset_discovery_location_t *loc)
{
    struct census *c = indexer->indexer_data;
    struct census_location *l;

    if (!loc || c->n_locations >= CENSUS_LOCATIONS)
        return false;
    if (loc->kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN)
        c->plugin_locations++;
    else if (loc->kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE && loc->location && loc->location[0])
        c->file_locations++;
    else
        return false;
    l = &c->locations[c->n_locations++];
    l->kind = loc->kind;
    l->flags = loc->flags;
    l->path = loc->kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE ? strdup(loc->location) : NULL;
    return true;
}

static bool idx_soundpack(const clap_preset_discovery_indexer_t *indexer, const clap_preset_discovery_soundpack_t *sp)
{
    (void)indexer;
    return sp != NULL;
}

static const void *idx_get_extension(const clap_preset_discovery_indexer_t *indexer, const char *id)
{
    (void)indexer;
    (void)id;
    return NULL;
}

static void rx_on_error(const clap_preset_discovery_metadata_receiver_t *r, int32_t os_error, const char *message)
{
    (void)r;
    (void)os_error;
    (void)message;
}

static bool rx_begin(const clap_preset_discovery_metadata_receiver_t *r, const char *name, const char *load_key)
{
    struct census *c = r->receiver_data;

    (void)name;
    (void)load_key;
    preset_end(c);
    c->open = 1;
    c->named_any = 0;
    c->has_flags = 0;
    memset(c->named, 0, sizeof(c->named));
    return true;
}

static void rx_plugin_id(const clap_preset_discovery_metadata_receiver_t *r, const clap_universal_plugin_id_t *id)
{
    struct census *c = r->receiver_data;
    uint32_t i;

    if (!c->open || !id || !id->abi || strcmp(id->abi, "clap") != 0 || !id->id)
        return;
    for (i = 0; i < c->n_ids; i++)
        if (!strcmp(c->ids[i], id->id))
        {
            c->named[i] = 1;
            c->named_any = 1;
        }
}

static void rx_soundpack(const clap_preset_discovery_metadata_receiver_t *r, const char *id) { (void)r; (void)id; }
static void rx_flags(const clap_preset_discovery_metadata_receiver_t *r, uint32_t flags)
{
    struct census *c = r->receiver_data;

    if (c->open)
    {
        c->flags = flags;
        c->has_flags = 1;
    }
}
static void rx_creator(const clap_preset_discovery_metadata_receiver_t *r, const char *s) { (void)r; (void)s; }
static void rx_description(const clap_preset_discovery_metadata_receiver_t *r, const char *s) { (void)r; (void)s; }
static void rx_timestamps(const clap_preset_discovery_metadata_receiver_t *r, clap_timestamp a, clap_timestamp b)
{
    (void)r;
    (void)a;
    (void)b;
}
static void rx_feature(const clap_preset_discovery_metadata_receiver_t *r, const char *s) { (void)r; (void)s; }
static void rx_extra(const clap_preset_discovery_metadata_receiver_t *r, const char *k, const char *v)
{
    (void)r;
    (void)k;
    (void)v;
}

static void crawl_one(struct census *c, const clap_preset_discovery_provider_t *p, uint32_t kind, uint32_t flags, const char *location)
{
    c->open = 0;
    c->location_flags = flags;
    p->get_metadata(p, kind, location, &c->receiver);
    preset_end(c);
}

static int wants(const struct census *c, const char *name)
{
    const char *dot = strrchr(name, '.');
    uint32_t i;

    if (c->p_any_ext)
        return 1;
    if (!dot)
        return 0;
    for (i = 0; i < c->p_n_exts; i++)
        if (!strcmp(dot + 1, c->p_exts[i]))
            return 1;
    return 0;
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void crawl_path(struct census *c, const clap_preset_discovery_provider_t *p, uint32_t flags, const char *path, uint32_t depth)
{
    struct stat st;
    struct dirent *e;
    char **names = NULL;
    size_t n = 0, cap = 0, i;
    DIR *dir;

    if (lstat(path, &st) != 0)
        return;
    if (S_ISLNK(st.st_mode) && (stat(path, &st) != 0 || (S_ISDIR(st.st_mode) && depth > 0)))
        return;
    if (S_ISREG(st.st_mode))
    {
        crawl_one(c, p, CLAP_PRESET_DISCOVERY_LOCATION_FILE, flags, path);
        return;
    }
    if (!S_ISDIR(st.st_mode) || depth >= CENSUS_DEPTH || !(dir = opendir(path)))
        return;
    while ((e = readdir(dir)))
    {
        char full[PATH_MAX];

        if (e->d_name[0] == '.' || (size_t)snprintf(full, sizeof(full), "%s/%s", path, e->d_name) >= sizeof(full))
            continue;
        if (n == cap)
        {
            char **grown = realloc(names, (cap = cap ? cap * 2 : 64) * sizeof(*names));
            if (!grown)
                break;
            names = grown;
        }
        if (!(names[n] = strdup(full)))
            break;
        n++;
    }
    closedir(dir);
    if (n)
        qsort(names, n, sizeof(*names), by_name);
    for (i = 0; i < n; i++)
    {
        if (lstat(names[i], &st) == 0 && (S_ISDIR(st.st_mode) || wants(c, names[i])))
            crawl_path(c, p, flags, names[i], depth + 1);
        free(names[i]);
    }
    free(names);
}

static void crawl_provider(struct census *c, const clap_preset_discovery_factory_t *f, const char *id)
{
    const clap_preset_discovery_provider_t *p = f->create(f, &c->indexer, id);
    uint32_t i;

    c->n_locations = 0;
    c->p_n_exts = 0;
    c->p_any_ext = 0;
    if (!p)
        return;
    if (p->init && p->init(p) && p->get_metadata)
        for (i = 0; i < c->n_locations; i++)
        {
            if (c->locations[i].kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN)
                crawl_one(c, p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, c->locations[i].flags, NULL);
            else
                crawl_path(c, p, c->locations[i].flags, c->locations[i].path, 0);
        }
    if (p->destroy)
        p->destroy(p);
    for (i = 0; i < c->n_locations; i++)
        free(c->locations[i].path);
    c->n_locations = 0;
}

/* child: "F\t<factory>\t<providers>\t<locations>\t<extensions>\t<unnamed>" then "C\t<id>\t<count>\t<factory count>" per
 * plugin id */
static int child_discover(const char *path)
{
    char reason[256];
    struct omx_clap_binary *bin = omx_clap_host_binary_open(path, reason, sizeof(reason));
    const clap_plugin_entry_t *entry;
    const clap_preset_discovery_factory_t *f;
    struct census *c;
    void *so;
    uint32_t i, count;

    if (!bin)
    {
        fprintf(g_out, "E\tload: %s\n", reason);
        fflush(g_out);
        _exit(0);
    }
    // the core holds the file open; this names the same object and its already-initialised entry
    so = dlopen(path, RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
    entry = so ? dlsym(so, "clap_entry") : NULL;
    if (!entry)
    {
        fprintf(g_out, "E\tno clap_entry\n");
        fflush(g_out);
        _exit(0);
    }
    f = entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID);
    if (!f)
        f = entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT);
    // a file that answers every factory id with its plugin factory has no preset-discovery factory (SpectMorph 0.6.1)
    if (!f || f == entry->get_factory(CLAP_PLUGIN_FACTORY_ID) || !f->count || !f->get_descriptor || !f->create)
    {
        fprintf(g_out, "F\t0\t0\tplugin:0/file:0\t\t0\n");
        fflush(g_out);
        _exit(0);       // no deinit: a file's teardown is not what is measured
    }
    if (!(c = calloc(1, sizeof(*c))))
        return 1;
    c->indexer = (clap_preset_discovery_indexer_t){ CLAP_VERSION_INIT, "preset-census", "openmixer", "", "1", c, idx_filetype,
                                                    idx_location, idx_soundpack, idx_get_extension };
    c->receiver = (clap_preset_discovery_metadata_receiver_t){ c, rx_on_error, rx_begin, rx_plugin_id, rx_soundpack, rx_flags,
                                                               rx_creator, rx_description, rx_timestamps, rx_feature, rx_extra };
    count = omx_clap_host_binary_count(bin);
    for (i = 0; i < count && c->n_ids < CENSUS_IDS; i++)
    {
        const clap_plugin_descriptor_t *d = omx_clap_host_binary_descriptor(bin, i);
        if (d && d->id)
            c->ids[c->n_ids++] = d->id;
    }
    c->providers = f->count(f);
    for (i = 0; i < c->providers; i++)
    {
        const clap_preset_discovery_provider_descriptor_t *d = f->get_descriptor(f, i);
        if (d && d->id)
            crawl_provider(c, f, d->id);
    }
    fprintf(g_out, "F\t1\t%u\tplugin:%u/file:%u\t", c->providers, c->plugin_locations, c->file_locations);
    if (c->any_ext)
        fprintf(g_out, "*%s", c->n_exts ? "," : "");
    for (i = 0; i < c->n_exts; i++)
        fprintf(g_out, "%s%s", i ? "," : "", c->exts[i]);
    fprintf(g_out, "\t%lu\n", c->unnamed);
    for (i = 0; i < c->n_ids; i++)
        fprintf(g_out, "C\t%s\t%lu\t%lu\n", c->ids[i], c->counts[i], c->factory[i]);
    fflush(g_out);
    _exit(0);
}

/* child: "I\t<id>\t<name>" per plugin the file lists */
static int child_list(const char *path)
{
    char reason[256];
    struct omx_clap_binary *bin = omx_clap_host_binary_open(path, reason, sizeof(reason));
    uint32_t i, count;

    if (!bin)
    {
        fprintf(g_out, "E\tload: %s\n", reason);
        fflush(g_out);
        _exit(0);
    }
    count = omx_clap_host_binary_count(bin);
    for (i = 0; i < count; i++)
    {
        const clap_plugin_descriptor_t *d = omx_clap_host_binary_descriptor(bin, i);
        if (d && d->id)
            fprintf(g_out, "I\t%s\t%s\n", d->id, d->name ? d->name : "");
    }
    fflush(g_out);
    _exit(0);
}

/* child: "X\t<preset_load>\t<state>" for one plugin, made and init'ed, never activated */
static int child_instance(const char *path, const char *id)
{
    char reason[256];
    struct omx_clap_binary *bin = omx_clap_host_binary_open(path, reason, sizeof(reason));
    struct omx_clap_instance *in = NULL;
    uint32_t i, count;

    if (!bin)
    {
        fprintf(g_out, "E\tload: %s\n", reason);
        fflush(g_out);
        _exit(0);
    }
    count = omx_clap_host_binary_count(bin);
    for (i = 0; i < count; i++)
    {
        const clap_plugin_descriptor_t *d = omx_clap_host_binary_descriptor(bin, i);
        if (d && d->id && !strcmp(d->id, id))
        {
            if (omx_clap_host_create(bin, d, &in) != 0 || !in)
            {
                fprintf(g_out, "E\tcreate or init failed\n");
                fflush(g_out);
                _exit(0);
            }
            fprintf(g_out, "X\t%d\t%d\n", in->preset_load != NULL, in->state != NULL);
            fflush(g_out);
            _exit(0);   // the plugin's destroy is not what is measured
        }
    }
    fprintf(g_out, "E\tid not listed\n");
    fflush(g_out);
    _exit(0);
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* the parent: one child per step, its stdout collected, killed past the timeout                                     */

static double now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* runs fn in a child; returns its output (malloc'd) and writes "" or the failure (timeout, crash:SIG, exit:N) */
static char *run_child(int (*fn)(const char *, const char *), const char *a, const char *b, char *fail, size_t fail_size)
{
    int fds[2], status = 0;
    pid_t pid;
    char *out = calloc(1, CENSUS_OUT_MAX + 1);
    size_t len = 0;
    double deadline;
    int timed_out = 0;

    fail[0] = '\0';
    if (!out || pipe(fds) != 0)
    {
        snprintf(fail, fail_size, "pipe");
        return out;
    }
    fflush(stdout);
    pid = fork();
    if (pid < 0)
    {
        snprintf(fail, fail_size, "fork");
        close(fds[0]);
        close(fds[1]);
        return out;
    }
    if (pid == 0)
    {
        int devnull = open("/dev/null", 0);

        close(fds[0]);
        if (!(g_out = fdopen(fds[1], "w")))
            _exit(3);
        // the plugin's own chatter, on stdout or stderr, goes to stderr and is kept out of the table
        dup2(2, 1);
        if (devnull >= 0)
            dup2(devnull, 0);
        _exit(fn(a, b));
    }
    close(fds[1]);
    deadline = now() + g_timeout;
    for (;;)
    {
        struct pollfd pfd = { fds[0], POLLIN, 0 };
        double left = deadline - now();
        ssize_t r;

        if (left <= 0)
        {
            timed_out = 1;
            break;
        }
        if (poll(&pfd, 1, (int)(left * 1000) + 1) <= 0)
            continue;
        r = read(fds[0], out + len, len < CENSUS_OUT_MAX ? CENSUS_OUT_MAX - len : 1);
        if (r <= 0)
            break;
        if (len < CENSUS_OUT_MAX)
            len += (size_t)r;
    }
    close(fds[0]);
    if (timed_out)
    {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        snprintf(fail, fail_size, "timeout:%ds", g_timeout);
        return out;
    }
    // the pipe closed: the child is ending, but a plugin thread may hold it past the deadline
    while (waitpid(pid, &status, WNOHANG) == 0)
    {
        if (now() > deadline)
        {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            snprintf(fail, fail_size, "timeout:%ds", g_timeout);
            return out;
        }
        usleep(10000);
    }
    if (WIFSIGNALED(status))
        snprintf(fail, fail_size, "crash:%s", sigabbrev_np(WTERMSIG(status)) ? sigabbrev_np(WTERMSIG(status)) : "?");
    else if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
        snprintf(fail, fail_size, "exit:%d", WEXITSTATUS(status));
    return out;
}

static int fn_list(const char *path, const char *unused)
{
    (void)unused;
    return child_list(path);
}

static int fn_discover(const char *path, const char *unused)
{
    (void)unused;
    return child_discover(path);
}

static int fn_instance(const char *path, const char *id)
{
    return child_instance(path, id);
}

/* the value of the first line starting with tag, its fields after the tag; NULL when none */
static char *line_of(char *out, const char *tag, const char *key)
{
    size_t tl = strlen(tag), kl = key ? strlen(key) : 0;
    char *p = out;

    while (p && *p)
    {
        if (!strncmp(p, tag, tl) && p[tl] == '\t' && (!key || (!strncmp(p + tl + 1, key, kl) && p[tl + 1 + kl] == '\t')))
            return p + tl + 1 + (key ? kl + 1 : 0);
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    return NULL;
}

static void field(char *dst, size_t cap, const char *src, int n)
{
    int i;

    dst[0] = '\0';
    if (!src)
        return;
    for (i = 0; i < n && src; i++)
        if ((src = strchr(src, '\t')))
            src++;
    if (!src)
        return;
    for (i = 0; *src && *src != '\t' && *src != '\n' && (size_t)i + 1 < cap; src++)
        dst[i++] = *src;
    dst[i] = '\0';
}

static void join(char *dst, size_t cap, const char *a, const char *b)
{
    if (!b[0])
        return;
    if (dst[0])
        snprintf(dst + strlen(dst), cap - strlen(dst), "; ");
    snprintf(dst + strlen(dst), cap - strlen(dst), "%s%s", a, b);
}

static void census_file(const char *path)
{
    char fail[64], dfail[64], err[512], derr[512];
    char factory[8], providers[16], locations[64], exts[512], unnamed[32];
    char *list, *disc, *p;

    list = run_child(fn_list, path, NULL, fail, sizeof(fail));
    if (fail[0] || (p = line_of(list, "E", NULL)))
    {
        char e[256];

        field(e, sizeof(e), fail[0] ? NULL : p - 2, 1);
        printf("%s\t-\t-\t-\t-\t-\t-\t-\t-\t-\tlist: %s\t-\t-\n", path, fail[0] ? fail : e);
        free(list);
        return;
    }
    disc = run_child(fn_discover, path, NULL, dfail, sizeof(dfail));
    derr[0] = '\0';
    p = line_of(disc, "F", NULL);
    if (dfail[0])
        snprintf(derr, sizeof(derr), "discover: %s", dfail);
    else if (!p)
    {
        char *e = line_of(disc, "E", NULL);
        char t[256];

        snprintf(t, sizeof(t), "%.*s", e ? (int)strcspn(e, "\n") : 9, e ? e : "no answer");
        snprintf(derr, sizeof(derr), "discover: %s", t);
    }
    if (p)
    {
        field(factory, sizeof(factory), p, 0);
        field(providers, sizeof(providers), p, 1);
        field(locations, sizeof(locations), p, 2);
        field(exts, sizeof(exts), p, 3);
        field(unnamed, sizeof(unnamed), p, 4);
    }
    else
    {
        strcpy(factory, "?");
        strcpy(providers, "?");
        strcpy(locations, "?");
        strcpy(exts, "?");
        strcpy(unnamed, "?");
    }

    for (p = list; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
    {
        char id[1024], name[1024], count[32], fcount[32], ifail[64], pl[8], st[8];
        char *inst, *x, *c;

        if (strncmp(p, "I\t", 2) != 0)
            continue;
        field(id, sizeof(id), p, 1);
        field(name, sizeof(name), p, 2);
        c = line_of(disc, "C", id);
        if (c)
        {
            field(count, sizeof(count), c, 0);
            field(fcount, sizeof(fcount), c, 1);
        }
        else
        {
            strcpy(count, factory[0] == '0' ? "0" : "?");
            strcpy(fcount, count);
        }
        inst = run_child(fn_instance, path, id, ifail, sizeof(ifail));
        x = line_of(inst, "X", NULL);
        if (x)
        {
            field(pl, sizeof(pl), x, 0);
            field(st, sizeof(st), x, 1);
        }
        else
        {
            strcpy(pl, "?");
            strcpy(st, "?");
        }
        snprintf(err, sizeof(err), "%s", derr);
        if (ifail[0])
            join(err, sizeof(err), "instance: ", ifail);
        else if (!x)
        {
            char *e = line_of(inst, "E", NULL);
            char t[256];

            snprintf(t, sizeof(t), "%.*s", e ? (int)strcspn(e, "\n") : 9, e ? e : "no answer");
            join(err, sizeof(err), "instance: ", t);
        }
        printf("%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", path, id, name, factory, providers, locations, exts, count,
               pl, st, err, unnamed, fcount);
        fflush(stdout);
        free(inst);
    }
    free(list);
    free(disc);
}

static int has_suffix(const char *name, const char *suffix)
{
    const size_t n = strlen(name), m = strlen(suffix);

    return n > m && !strcmp(name + n - m, suffix);
}

/* a directory as omx-clap-scan walks it: its .clap files in name order, its directories walked, and a .clap directory
 * with no .clap file directly inside read as a bundle through its one object */
static void census_dir(const char *dir, uint32_t depth)
{
    struct dirent *e;
    struct stat st;
    char **names = NULL;
    size_t n = 0, cap = 0, i;
    unsigned here = 0;
    DIR *d;

    if (depth > CENSUS_DEPTH || !(d = opendir(dir)))
    {
        printf("%s\t-\t-\t-\t-\t-\t-\t-\t-\t-\tunreadable directory\t-\t-\n", dir);
        return;
    }
    while ((e = readdir(d)))
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (n == cap)
        {
            char **grown = realloc(names, (cap = cap ? cap * 2 : 64) * sizeof(*names));
            if (!grown)
                break;
            names = grown;
        }
        if (asprintf(&names[n], "%s/%s", dir, e->d_name) < 0)
            break;
        n++;
    }
    closedir(d);
    if (n)
        qsort(names, n, sizeof(*names), by_name);
    for (i = 0; i < n; i++)
    {
        if (stat(names[i], &st) == 0 && S_ISDIR(st.st_mode))
            census_dir(names[i], depth + 1);
        else if (stat(names[i], &st) == 0 && S_ISREG(st.st_mode) && has_suffix(names[i], ".clap"))
        {
            census_file(names[i]);
            here++;
        }
        free(names[i]);
    }
    free(names);
    if (here == 0 && has_suffix(dir, ".clap"))
    {
        char object[PATH_MAX], reason[256];
        const int k = omx_clap_host_bundle_object(dir, object, sizeof(object), reason, sizeof(reason));

        if (k == 1)
            census_file(object);
        else if (k > 1)
            printf("%s\t-\t-\t-\t-\t-\t-\t-\t-\t-\tbundle: %s\t-\t-\n", dir, reason);
    }
}

int main(int argc, char **argv)
{
    struct omx_clap_host_config config;
    int a = 1;

    if (a + 1 < argc && !strcmp(argv[a], "--timeout"))
    {
        g_timeout = atoi(argv[a + 1]);
        a += 2;
    }
    if (a >= argc || g_timeout <= 0)
    {
        fprintf(stderr, "usage: %s [--timeout SECONDS] FILE|DIRECTORY...\n", argv[0]);
        return 2;
    }
    omx_clap_host_config_default_sized(&config, sizeof(config));
    config.note_inputs = 1;
    config.lane_channels = 0;
    if (omx_clap_host_configure(&config) != 0)
    {
        fprintf(stderr, "can't configure the clap core\n");
        return 2;
    }
    printf("file\tplugin_id\tname\thas_discovery_factory\tproviders\tlocations\textensions\tpresets_counted\thas_preset_load\t"
           "has_state\terror\tfile_presets_unnamed\tpresets_factory\n");
    for (; a < argc; a++)
    {
        struct stat st;

        if (stat(argv[a], &st) == 0 && S_ISDIR(st.st_mode))
            census_dir(argv[a], 0);
        else
            census_file(argv[a]);
    }
    return 0;
}
