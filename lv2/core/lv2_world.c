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

/*
************************************************************************************************************************
*
* The bundle half of the LV2 host (see lv2_core.h): lilv, one bundle at a time, the ports, the refusals, the binary and
* the default state. Moved from the console's mix_lv2_host.c, its behaviour unchanged.
*
* lilv is NOT linked: the lilv headers give the prototypes, __typeof__ turns each into a pointer type (unevaluated, so no
* symbol reference reaches the linker), and the first bundle fills the table by dlsym from the library the
* configuration names. Main thread only; the state is one static world, unguarded, because only the main thread calls
* in.
*
************************************************************************************************************************
*/


/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/core/lv2.h>
#include <lv2/port-props/port-props.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>

#include "lv2_core_internal.h"


/*
************************************************************************************************************************
*           LOCAL DEFINES
************************************************************************************************************************
*/

#define LV2_EVENT_PORT                  "http://lv2plug.in/ns/ext/event#EventPort"
#define LV2_CORE_IS_SIDECHAIN           "http://lv2plug.in/ns/lv2core#isSideChain"
#define LV2_RDFS_LABEL                  "http://www.w3.org/2000/01/rdf-schema#label"

/* every lilv entry point this unit calls: one list, so a call the table lacks is a build error */
#define OMX_LILV_FNS(X) \
    X(lilv_world_new) X(lilv_world_free) X(lilv_new_uri) X(lilv_new_file_uri) X(lilv_node_free) \
    X(lilv_node_equals) X(lilv_node_as_uri) X(lilv_node_as_string) X(lilv_node_as_float) X(lilv_node_is_float) \
    X(lilv_node_is_int) X(lilv_world_load_bundle) X(lilv_world_unload_bundle) X(lilv_world_get_all_plugins) \
    X(lilv_plugins_get_by_uri) X(lilv_plugins_begin) X(lilv_plugins_next) X(lilv_plugins_is_end) X(lilv_plugins_get) \
    X(lilv_plugin_get_uri) X(lilv_plugin_get_name) X(lilv_plugin_get_bundle_uri) X(lilv_plugin_get_library_uri) \
    X(lilv_plugin_get_num_ports) X(lilv_plugin_get_port_by_index) X(lilv_plugin_get_required_features) \
    X(lilv_port_is_a) X(lilv_port_get_symbol) X(lilv_port_get_name) X(lilv_port_get_range) X(lilv_port_get_value) \
    X(lilv_port_has_property) X(lilv_port_get_scale_points) X(lilv_scale_points_begin) X(lilv_scale_points_next) \
    X(lilv_scale_points_is_end) X(lilv_scale_points_get) X(lilv_scale_points_free) X(lilv_scale_point_get_label) \
    X(lilv_scale_point_get_value) X(lilv_nodes_begin) X(lilv_nodes_next) X(lilv_nodes_is_end) X(lilv_nodes_get) \
    X(lilv_nodes_contains) X(lilv_nodes_free) X(lilv_file_uri_parse) X(lilv_free) \
    X(lilv_state_new_from_world) X(lilv_state_restore) X(lilv_state_free) X(lilv_plugin_get_value) \
    X(lilv_world_find_nodes)


/*
************************************************************************************************************************
*           LOCAL DATA TYPES
************************************************************************************************************************
*/

#define OMX_LILV_PTR(name) __typeof__(name) *name;
static struct
{
    OMX_LILV_FNS(OMX_LILV_PTR)
} L;
#undef OMX_LILV_PTR

/* the URIs the port reading asks lilv about, made once per world */
enum
{
    N_AUDIO, N_CONTROL, N_CV, N_ATOM, N_EVENT, N_INPUT, N_OUTPUT, N_DESIGNATION, N_ENABLED, N_LATENCY,
    N_REPORTS_LATENCY, N_INTEGER, N_TOGGLED, N_ENUMERATION, N_NOT_ON_GUI, N_TRIGGER, N_LOGARITHMIC,
    N_IS_SIDECHAIN, N_PG_GROUP, N_PG_SIDECHAIN_OF, N_PG_MAIN_INPUT, N_LABEL, N_COUNT
};

static const char *const NODE_URIS[N_COUNT] =
{
    [N_AUDIO] = LV2_CORE__AudioPort,
    [N_CONTROL] = LV2_CORE__ControlPort,
    [N_CV] = LV2_CORE__CVPort,
    [N_ATOM] = LV2_ATOM__AtomPort,
    [N_EVENT] = LV2_EVENT_PORT,
    [N_INPUT] = LV2_CORE__InputPort,
    [N_OUTPUT] = LV2_CORE__OutputPort,
    [N_DESIGNATION] = LV2_CORE__designation,
    [N_ENABLED] = LV2_CORE__enabled,
    [N_LATENCY] = LV2_CORE__latency,
    [N_REPORTS_LATENCY] = LV2_CORE__reportsLatency,
    [N_INTEGER] = LV2_CORE__integer,
    [N_TOGGLED] = LV2_CORE__toggled,
    [N_ENUMERATION] = LV2_CORE__enumeration,
    [N_NOT_ON_GUI] = LV2_PORT_PROPS__notOnGUI,
    [N_TRIGGER] = LV2_PORT_PROPS__trigger,
    [N_LOGARITHMIC] = LV2_PORT_PROPS__logarithmic,
    [N_IS_SIDECHAIN] = LV2_CORE_IS_SIDECHAIN,
    [N_PG_GROUP] = LV2_PORT_GROUPS__group,
    [N_PG_SIDECHAIN_OF] = LV2_PORT_GROUPS__sideChainOf,
    [N_PG_MAIN_INPUT] = LV2_PORT_GROUPS__mainInput,
    [N_LABEL] = LV2_RDFS_LABEL,
};

/* one loaded bundle, reference-counted across the entries and plugins that use it */
struct lv2_bundle
{
    char *dir;
    LilvNode *node;
    unsigned refs;
    char **uris, **names;
    uint32_t count;
    struct lv2_bundle *next;
};


/*
************************************************************************************************************************
*           LOCAL GLOBAL VARIABLES
************************************************************************************************************************
*/

static struct
{
    void *so;
    LilvWorld *world;
    LilvNode *node[N_COUNT];
    struct lv2_bundle *bundles;
} g;


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS: LILV
************************************************************************************************************************
*/

void lv2_why_set(char why[LV2_CORE_WHY_MAX], const char *code)
{
    if (why)
        snprintf(why, LV2_CORE_WHY_MAX, "%s", code);
}

/* The lazy load: the library, every entry point, the world and its nodes; all or nothing, and a failure leaves nothing
 * mapped and nothing latched. */
static int lilv_load(void)
{
    const struct lv2_core_config *config = lv2_core_config();
    LilvWorld *world;
    int missing = 0, k;
    void *so;

    if (g.so)
        return 0;
    if (!config)
        return -1;
    so = dlopen(config->lilv_soname ? config->lilv_soname : LV2_CORE_LILV_SONAME, RTLD_NOW | RTLD_LOCAL);
    if (!so)
        return -1;
#define OMX_LILV_SYM(name) missing |= !(*(void **)&L.name = dlsym(so, #name));
    OMX_LILV_FNS(OMX_LILV_SYM)
#undef OMX_LILV_SYM
    world = missing ? NULL : L.lilv_world_new();
    if (!world)
    {
        memset(&L, 0, sizeof(L));
        dlclose(so);
        return -1;
    }
    g.so = so;
    g.world = world;
    for (k = 0; k < N_COUNT; k++)
        g.node[k] = L.lilv_new_uri(world, NODE_URIS[k]);
    return 0;
}

int lv2_core_lilv_loaded(void)
{
    return g.so != NULL;
}


/*
************************************************************************************************************************
*           GLOBAL FUNCTIONS: BUNDLES, ONE AT A TIME, NEVER LOAD_ALL
************************************************************************************************************************
*/

static void bundle_free(struct lv2_bundle *b)
{
    uint32_t i;

    for (i = 0; i < b->count; i++)
    {
        free(b->uris[i]);
        free(b->names[i]);
    }
    free(b->uris);
    free(b->names);
    if (b->node)
        L.lilv_node_free(b->node);
    free(b->dir);
    free(b);
}

/* the plugins whose bundle is THIS one: another loaded bundle's are not listed */
static int bundle_list(struct lv2_bundle *b)
{
    const LilvPlugins *all = L.lilv_world_get_all_plugins(g.world);
    uint32_t n = 0;
    LilvIter *it;

    for (it = L.lilv_plugins_begin(all); !L.lilv_plugins_is_end(all, it); it = L.lilv_plugins_next(all, it))
        n += L.lilv_node_equals(L.lilv_plugin_get_bundle_uri(L.lilv_plugins_get(all, it)), b->node) ? 1u : 0u;
    if (n == 0)
        return -1;
    b->uris = calloc(n, sizeof(*b->uris));
    b->names = calloc(n, sizeof(*b->names));
    if (!b->uris || !b->names)
        return -1;
    for (it = L.lilv_plugins_begin(all); !L.lilv_plugins_is_end(all, it); it = L.lilv_plugins_next(all, it))
    {
        const LilvPlugin *pl = L.lilv_plugins_get(all, it);
        LilvNode *name;

        if (!L.lilv_node_equals(L.lilv_plugin_get_bundle_uri(pl), b->node) || b->count == n)
            continue;
        name = L.lilv_plugin_get_name(pl);
        b->uris[b->count] = strdup(L.lilv_node_as_uri(L.lilv_plugin_get_uri(pl)));
        b->names[b->count] = strdup(name ? L.lilv_node_as_string(name) : "");
        if (name)
            L.lilv_node_free(name);
        if (!b->uris[b->count] || !b->names[b->count])
            return -1;
        b->count++;
    }
    return 0;
}

struct lv2_bundle *lv2_bundle_ref(const char *path, char why[LV2_CORE_WHY_MAX])
{
    struct lv2_bundle *b;
    size_t n;
    char *dir;

    if (!path || path[0] != '/' || lilv_load() != 0)
    {
        lv2_why_set(why, LV2_CODE_NO_REALISATION);
        return NULL;
    }
    n = strlen(path);
    dir = malloc(n + 2);
    if (!dir)
    {
        lv2_why_set(why, LV2_CODE_NO_REALISATION);
        return NULL;
    }
    memcpy(dir, path, n + 1);
    if (dir[n - 1] != '/')
    {
        dir[n] = '/';
        dir[n + 1] = '\0';
    }
    for (b = g.bundles; b; b = b->next)
    {
        if (strcmp(b->dir, dir) == 0)
        {
            free(dir);
            b->refs++;
            return b;
        }
    }
    b = calloc(1, sizeof(*b));
    if (!b)
    {
        free(dir);
        lv2_why_set(why, LV2_CODE_NO_REALISATION);
        return NULL;
    }
    b->dir = dir;
    b->node = L.lilv_new_file_uri(g.world, NULL, dir);
    if (!b->node)
    {
        bundle_free(b);
        lv2_why_set(why, LV2_CODE_NO_REALISATION);
        return NULL;
    }
    L.lilv_world_load_bundle(g.world, b->node);
    if (bundle_list(b) != 0)
    {
        L.lilv_world_unload_bundle(g.world, b->node);
        bundle_free(b);
        lv2_why_set(why, LV2_CODE_NO_REALISATION);
        return NULL;
    }
    b->refs = 1;
    b->next = g.bundles;
    g.bundles = b;
    lv2_why_set(why, "");
    return b;
}

void lv2_bundle_unref(struct lv2_bundle *b)
{
    struct lv2_bundle **link;

    if (!b || --b->refs > 0)
        return;
    L.lilv_world_unload_bundle(g.world, b->node);
    for (link = &g.bundles; *link; link = &(*link)->next)
    {
        if (*link == b)
        {
            *link = b->next;
            break;
        }
    }
    bundle_free(b);
}

uint32_t lv2_bundle_count(const struct lv2_bundle *b)
{
    return b ? b->count : 0;
}

const char *lv2_bundle_uri(const struct lv2_bundle *b, uint32_t index)
{
    return b && index < b->count ? b->uris[index] : NULL;
}

const char *lv2_bundle_name(const struct lv2_bundle *b, uint32_t index)
{
    return b && index < b->count ? b->names[index] : NULL;
}


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS: THE PORTS
************************************************************************************************************************
*/

static int port_designated(const LilvPlugin *pl, const LilvPort *port, int designation)
{
    LilvNodes *d = L.lilv_port_get_value(pl, port, g.node[N_DESIGNATION]);
    const int yes = d && L.lilv_nodes_contains(d, g.node[designation]);

    L.lilv_nodes_free(d);
    return yes;
}

static float node_float(const LilvNode *node, int *ok)
{
    *ok = node && (L.lilv_node_is_float(node) || L.lilv_node_is_int(node));
    return *ok ? L.lilv_node_as_float(node) : 0.0f;
}

/* the range, and the default as the console's port_default reads it: lv2:default, else lv2:minimum, else 0 */
static void port_range(const LilvPlugin *pl, const LilvPort *port, struct lv2_control *c)
{
    LilvNode *def = NULL, *min = NULL, *max = NULL;
    int has_def, has_min, has_max;

    L.lilv_port_get_range(pl, port, &def, &min, &max);
    c->def = node_float(def, &has_def);
    c->min = node_float(min, &has_min);
    c->max = node_float(max, &has_max);
    if (!has_def)
        c->def = c->min;
    if (!has_max)
        c->max = c->min > c->def ? c->min : c->def;
    L.lilv_node_free(def);
    L.lilv_node_free(min);
    L.lilv_node_free(max);
}

static uint32_t port_props(const LilvPlugin *pl, const LilvPort *port)
{
    static const struct { int node; uint32_t prop; } map[] =
    {
        { N_INTEGER, LV2_PROP_INTEGER }, { N_TOGGLED, LV2_PROP_TOGGLED }, { N_ENUMERATION, LV2_PROP_ENUMERATION },
        { N_NOT_ON_GUI, LV2_PROP_NOT_ON_GUI }, { N_TRIGGER, LV2_PROP_TRIGGER }, { N_LOGARITHMIC, LV2_PROP_LOGARITHMIC },
    };
    uint32_t props = 0;
    size_t i;

    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (L.lilv_port_has_property(pl, port, g.node[map[i].node]))
            props |= map[i].prop;
    return props;
}

/* the scale points, in value order; a complete enumeration has one on every integer from min to max */
static int port_points(const LilvPlugin *pl, const LilvPort *port, struct lv2_control *c)
{
    LilvScalePoints *points = L.lilv_port_get_scale_points(pl, port);
    uint32_t n = 0, i, j;
    LilvIter *it;

    if (!points)
        return 0;
    for (it = L.lilv_scale_points_begin(points); !L.lilv_scale_points_is_end(points, it); it = L.lilv_scale_points_next(points, it))
        n++;
    c->points = calloc(n ? n : 1u, sizeof(*c->points));
    if (!c->points)
    {
        L.lilv_scale_points_free(points);
        return -1;
    }
    for (it = L.lilv_scale_points_begin(points); !L.lilv_scale_points_is_end(points, it); it = L.lilv_scale_points_next(points, it))
    {
        const LilvScalePoint *p = L.lilv_scale_points_get(points, it);
        const LilvNode *label = L.lilv_scale_point_get_label(p);
        int ok;
        struct lv2_scale_point sp;

        sp.value = node_float(L.lilv_scale_point_get_value(p), &ok);
        if (!ok)
            continue;
        sp.label = strdup(label ? L.lilv_node_as_string(label) : "");
        if (!sp.label)
            break;
        for (j = c->n_points; j > 0 && c->points[j - 1].value > sp.value; j--)
            c->points[j] = c->points[j - 1];
        c->points[j] = sp;
        c->n_points++;
    }
    L.lilv_scale_points_free(points);
    if ((c->props & LV2_PROP_ENUMERATION) && c->min == floorf(c->min) && c->max == floorf(c->max) && c->max >= c->min)
    {
        float v = c->min;

        for (i = 0; i < c->n_points && v <= c->max; i++)
            if (c->points[i].value == v)
                v += 1.0f;
        if (v > c->max)
            c->props |= LV2_PROP_ENUM_COMPLETE;
    }
    return 0;
}

static char *node_dup(LilvNode *node, const char *fallback)
{
    char *s = strdup(node ? L.lilv_node_as_string(node) : fallback);

    if (node)
        L.lilv_node_free(node);
    return s;
}

/* a node's string, duplicated, from the first of `nodes`; NULL when there is none. The nodes stay the caller's. */
static char *first_string(LilvNodes *nodes)
{
    LilvIter *it = nodes ? L.lilv_nodes_begin(nodes) : NULL;

    return it && !L.lilv_nodes_is_end(nodes, it) ? strdup(L.lilv_node_as_string(L.lilv_nodes_get(nodes, it))) : NULL;
}

/* whether a port is an audio input */
static int audio_input(const LilvPlugin *pl, uint32_t i)
{
    const LilvPort *port = L.lilv_plugin_get_port_by_index(pl, i);

    return L.lilv_port_is_a(pl, port, g.node[N_AUDIO]) && L.lilv_port_is_a(pl, port, g.node[N_INPUT]);
}

/* the group of a port, as its string, duplicated; NULL for an ungrouped port */
static char *port_group(const LilvPlugin *pl, const LilvPort *port)
{
    LilvNodes *grp = L.lilv_port_get_value(pl, port, g.node[N_PG_GROUP]);
    char *s = first_string(grp);

    L.lilv_nodes_free(grp);
    return s;
}

/* whether a port carries `pred` with a value */
static int port_has(const LilvPlugin *pl, const LilvPort *port, int pred)
{
    LilvNodes *vals = L.lilv_port_get_value(pl, port, g.node[pred]);
    const int yes = vals && !L.lilv_nodes_is_end(vals, L.lilv_nodes_begin(vals));

    L.lilv_nodes_free(vals);
    return yes;
}

/* whether the group of a port carries `pred` with a value: the group is a node of the bundle, so the world is asked */
static int group_has(const LilvPlugin *pl, const LilvPort *port, int pred)
{
    LilvNodes *grp = L.lilv_port_get_value(pl, port, g.node[N_PG_GROUP]);
    LilvIter *it = grp ? L.lilv_nodes_begin(grp) : NULL;
    int yes = 0;

    if (it && !L.lilv_nodes_is_end(grp, it))
    {
        LilvNodes *vals = L.lilv_world_find_nodes(g.world, L.lilv_nodes_get(grp, it), g.node[pred], NULL);

        yes = vals && !L.lilv_nodes_is_end(vals, L.lilv_nodes_begin(vals));
        L.lilv_nodes_free(vals);
    }
    L.lilv_nodes_free(grp);
    return yes;
}

/* the name of a side chain: the label of its port's group, else the port's own name, else its symbol; duplicated */
static char *side_name(const LilvPlugin *pl, const LilvPort *port)
{
    LilvNodes *grp = L.lilv_port_get_value(pl, port, g.node[N_PG_GROUP]);
    LilvIter *it = grp ? L.lilv_nodes_begin(grp) : NULL;
    char *s = NULL;

    if (it && !L.lilv_nodes_is_end(grp, it))
    {
        LilvNodes *labels = L.lilv_world_find_nodes(g.world, L.lilv_nodes_get(grp, it), g.node[N_LABEL], NULL);

        s = first_string(labels);
        L.lilv_nodes_free(labels);
    }
    L.lilv_nodes_free(grp);
    if (!s)
        s = node_dup(L.lilv_port_get_name(pl, port), L.lilv_node_as_string(L.lilv_port_get_symbol(pl, port)));
    return s;
}

/* The audio inputs, sorted. The main input is every input that is not a side chain, or with pg:mainInput the members of
 * that group; more than two, or an input outside the group, is refused as extra. A side chain is an input with
 * lv2:isSideChain, or with pg:sideChainOf on the port or on its group. A CLAP input is made of the members of one
 * pg:group, or of one ungrouped port, in the order of their first member, one or two channels each. Fills p->legs,
 * p->in_ports and the side chains; returns the refusal, or NULL. */
static const char *read_inputs(struct lv2_plugin *p, const LilvPlugin *pl, uint32_t n)
{
    char **group = calloc(n ? n : 1u, sizeof(*group));
    unsigned char *side = calloc(n ? n : 1u, sizeof(*side));
    uint32_t *owner = calloc(n ? n : 1u, sizeof(*owner));
    uint32_t *first = calloc(LV2_CORE_SIDE_LEGS, sizeof(*first));
    uint32_t *fill = calloc(LV2_CORE_SIDE_LEGS, sizeof(*fill));
    uint32_t base[LV2_CORE_SIDE_LEGS] = { 0 };
    const char *key[LV2_CORE_SIDE_LEGS] = { NULL };
    LilvNodes *mg = L.lilv_plugin_get_value(pl, g.node[N_PG_MAIN_INPUT]);
    char *main_group = first_string(mg);
    uint32_t i, k, nmain = 0, legs = 0, ncl = 0;
    const char *refusal = NULL;

    L.lilv_nodes_free(mg);
    if (!group || !side || !owner || !first || !fill)
        refusal = LV2_CODE_NO_REALISATION;
    for (i = 0; i < n && !refusal; i++)
    {
        const LilvPort *port;
        int is_side;

        owner[i] = UINT32_MAX;
        if (!audio_input(pl, i))
            continue;
        port = L.lilv_plugin_get_port_by_index(pl, i);
        group[i] = port_group(pl, port);
        is_side = L.lilv_port_has_property(pl, port, g.node[N_IS_SIDECHAIN]) || port_has(pl, port, N_PG_SIDECHAIN_OF)
            || group_has(pl, port, N_PG_SIDECHAIN_OF);
        side[i] = (unsigned char)(is_side != 0);
        if (is_side)
            continue;
        if (main_group && !(group[i] && strcmp(group[i], main_group) == 0))
        {
            refusal = LV2_CODE_EXTRA_INPUTS;
            break;
        }
        if (nmain < 2)
            p->in_ports[nmain] = i;
        nmain++;
    }
    if (!refusal && nmain == 0)
        refusal = LV2_CODE_NO_AUDIO_INPUT;
    if (!refusal && nmain > 2)
        refusal = LV2_CODE_EXTRA_INPUTS;
    for (i = 0; i < n && !refusal; i++)
    {
        if (!side[i])
            continue;
        for (k = 0; k < ncl && !(group[i] && key[k] && strcmp(key[k], group[i]) == 0); k++)
            ;
        if (k == ncl)
        {
            if (ncl == LV2_CORE_SIDE_LEGS)
            {
                refusal = LV2_CODE_EXTRA_INPUTS;
                break;
            }
            key[ncl] = group[i];
            first[ncl] = i;
            ncl++;
        }
        owner[i] = k;
        p->side_channels[k]++;
        legs++;
    }
    for (k = 0; k < ncl && !refusal; k++)
        if (p->side_channels[k] > 2)
            refusal = LV2_CODE_WIDER_THAN_STRIP;
    if (!refusal && legs > LV2_CORE_SIDE_LEGS)
        refusal = LV2_CODE_EXTRA_INPUTS;
    for (k = 0; k < ncl && !refusal; k++)
    {
        base[k] = k ? base[k - 1] + p->side_channels[k - 1] : 0;
        p->side_base[k] = base[k];
        p->side_name[k] = side_name(pl, L.lilv_plugin_get_port_by_index(pl, first[k]));
        if (!p->side_name[k])
            refusal = LV2_CODE_NO_REALISATION;
    }
    for (i = 0; i < n && !refusal; i++)
    {
        if (!side[i])
            continue;
        k = owner[i];
        p->side_in[base[k] + fill[k]++] = i;
    }
    if (!refusal)
    {
        p->n_side = ncl;
        p->n_side_legs = legs;
        p->legs = nmain;
    }
    for (i = 0; i < n && group; i++)
        free(group[i]);
    free(group);
    free(side);
    free(owner);
    free(first);
    free(fill);
    free(main_group);
    return refusal;
}

/* Read every port, or name the refusal, before any binary: the console's read_ports, its codes word for word. */
static const char *read_ports(struct lv2_plugin *p, const LilvPlugin *pl)
{
    const uint32_t n = L.lilv_plugin_get_num_ports(pl);
    uint32_t nout = 0, out[3] = { 0 }, i;
    const char *refusal;
    int has_bypass = 0;

    p->controls = calloc(n ? n : 1u, sizeof(*p->controls));
    if (!p->controls)
        return LV2_CODE_NO_REALISATION;
    p->latency_port = -1;
    p->n_ports = n;
    for (i = 0; i < n; i++)
    {
        const LilvPort *port = L.lilv_plugin_get_port_by_index(pl, i);
        const int input = L.lilv_port_is_a(pl, port, g.node[N_INPUT]);
        struct lv2_control *c;
        const char *symbol;

        if (L.lilv_port_is_a(pl, port, g.node[N_ATOM]) || L.lilv_port_is_a(pl, port, g.node[N_EVENT]))
            return LV2_CODE_MIDI_IN;
        if (L.lilv_port_is_a(pl, port, g.node[N_CV]))
            return LV2_CODE_CV_PORTS;
        if (L.lilv_port_is_a(pl, port, g.node[N_AUDIO]))
        {
            if (!input)
            {
                if (nout < 3)
                    out[nout] = i;
                nout++;
            }
            continue;
        }
        if (!L.lilv_port_is_a(pl, port, g.node[N_CONTROL]))
            return LV2_CODE_NO_REALISATION;
        symbol = L.lilv_node_as_string(L.lilv_port_get_symbol(pl, port));
        if (!input && p->latency_port < 0
            && (port_designated(pl, port, N_LATENCY) || L.lilv_port_has_property(pl, port, g.node[N_REPORTS_LATENCY])))
        {
            p->latency_port = (int32_t)i;
            continue;
        }
        c = &p->controls[p->n_controls++];
        c->port = i;
        c->symbol = strdup(symbol ? symbol : "");
        c->name = node_dup(L.lilv_port_get_name(pl, port), c->symbol ? c->symbol : "");
        if (!c->symbol || !c->name)
            return LV2_CODE_NO_REALISATION;
        port_range(pl, port, c);
        c->props = port_props(pl, port);
        if (!input)
        {
            c->kind = LV2_CONTROL_OUTPUT;
        }
        else if (!has_bypass && port_designated(pl, port, N_ENABLED))
        {
            c->kind = LV2_CONTROL_BYPASS;
            c->def = 1.0f;      // held at the value that keeps the plugin processing
            has_bypass = 1;
        }
        else if (!has_bypass && strcmp(c->symbol, "bypass") == 0)
        {
            c->kind = LV2_CONTROL_BYPASS;
            c->def = 0.0f;
            has_bypass = 1;
        }
        else
        {
            c->kind = LV2_CONTROL_INPUT;
        }
        if (port_points(pl, port, c) != 0)
            return LV2_CODE_NO_REALISATION;
    }
    refusal = read_inputs(p, pl, n);
    if (refusal)
        return refusal;
    if (nout == 0)
        return LV2_CODE_NO_AUDIO_OUTPUT;
    if (nout != p->legs)
        return LV2_CODE_WIDER_THAN_STRIP;
    memcpy(p->out_ports, out, sizeof(p->out_ports));
    return NULL;
}

/* lv2:requiredFeature, and the refusal of one outside the configured list */
static const char *read_required(struct lv2_plugin *p, const LilvPlugin *pl)
{
    LilvNodes *req = L.lilv_plugin_get_required_features(pl);
    uint32_t n = 0, k = 0;
    const char *refused = NULL;
    LilvIter *it;

    for (it = req ? L.lilv_nodes_begin(req) : NULL; req && !L.lilv_nodes_is_end(req, it); it = L.lilv_nodes_next(req, it))
        n++;
    p->required = calloc(n + 1u, sizeof(*p->required));
    for (it = req ? L.lilv_nodes_begin(req) : NULL; p->required && req && !L.lilv_nodes_is_end(req, it); it = L.lilv_nodes_next(req, it))
    {
        p->required[k] = strdup(L.lilv_node_as_uri(L.lilv_nodes_get(req, it)));
        if (!p->required[k])
            break;
        k++;
    }
    L.lilv_nodes_free(req);
    if (!p->required || k != n)
        return LV2_CODE_NO_REALISATION;
    for (k = 0; k < n && !refused; k++)
        if (!lv2_feature_configured(p->required[k]))
            refused = LV2_CODE_FEATURES_MISSING;
    return refused;
}


/*
************************************************************************************************************************
*           GLOBAL FUNCTIONS: THE PLUGIN
************************************************************************************************************************
*/

void lv2_plugin_close(struct lv2_plugin *p)
{
    uint32_t i, k;

    if (!p)
        return;
    if (p->binary)
        dlclose(p->binary);
    if (p->uri_node)
        L.lilv_node_free(p->uri_node);
    for (i = 0; p->required && p->required[i]; i++)
        free(p->required[i]);
    free(p->required);
    for (k = 0; p->controls && k < p->n_controls; k++)
    {
        for (i = 0; i < p->controls[k].n_points; i++)
            free(p->controls[k].points[i].label);
        free(p->controls[k].points);
        free(p->controls[k].symbol);
        free(p->controls[k].name);
    }
    free(p->controls);
    for (i = 0; i < p->n_side; i++)
        free(p->side_name[i]);
    free(p->uri);
    free(p->bundle_path);
    lv2_bundle_unref(p->bundle);
    free(p);
}

struct lv2_plugin *lv2_plugin_open(struct lv2_bundle *bundle, const char *uri, char why[LV2_CORE_WHY_MAX])
{
    struct lv2_plugin *p;
    const LilvPlugin *pl;
    const char *refusal;

    if (!bundle || !uri || !g.world)
    {
        lv2_why_set(why, LV2_CODE_NO_REALISATION);
        return NULL;
    }
    p = calloc(1, sizeof(*p));
    if (!p)
    {
        lv2_why_set(why, LV2_CODE_NO_REALISATION);
        return NULL;
    }
    bundle->refs++;
    p->bundle = bundle;
    p->uri = strdup(uri);
    p->bundle_path = strdup(bundle->dir);
    p->uri_node = L.lilv_new_uri(g.world, uri);
    if (!p->uri || !p->bundle_path || !p->uri_node)
    {
        lv2_plugin_close(p);
        lv2_why_set(why, LV2_CODE_NO_REALISATION);
        return NULL;
    }
    // the plugin must be THIS bundle's: a URI another open bundle holds is not in this one
    pl = L.lilv_plugins_get_by_uri(L.lilv_world_get_all_plugins(g.world), p->uri_node);
    refusal = !pl || !L.lilv_node_equals(L.lilv_plugin_get_bundle_uri(pl), bundle->node) ? LV2_CODE_NO_REALISATION : NULL;
    if (!refusal)
        refusal = read_ports(p, pl);
    if (!refusal)
        refusal = read_required(p, pl);
    if (refusal)
    {
        lv2_plugin_close(p);
        lv2_why_set(why, refusal);
        return NULL;
    }
    lv2_why_set(why, "");
    return p;
}

/* the binary, RTLD_NOW so the first RT run() faults in no code page, walked to the URI */
int lv2_plugin_load(struct lv2_plugin *p, char why[LV2_CORE_WHY_MAX])
{
    const LilvPlugin *pl = L.lilv_plugins_get_by_uri(L.lilv_world_get_all_plugins(g.world), p->uri_node);
    LV2_Descriptor_Function entry;
    char *path;
    uint32_t i;

    if (p->desc)
        return 0;
    path = pl ? L.lilv_file_uri_parse(L.lilv_node_as_uri(L.lilv_plugin_get_library_uri(pl)), NULL) : NULL;
    if (!path)
    {
        lv2_why_set(why, LV2_CODE_NO_REALISATION);
        return -1;
    }
    p->binary = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    L.lilv_free(path);
    if (!p->binary)
    {
        lv2_why_set(why, LV2_CODE_NO_REALISATION);
        return -1;
    }
    *(void **)&entry = dlsym(p->binary, "lv2_descriptor");
    for (i = 0; entry && !p->desc; i++)
    {
        const LV2_Descriptor *d = entry(i);

        if (!d)
            break;
        if (strcmp(d->URI, p->uri) == 0)
            p->desc = d;
    }
    if (!p->desc)
    {
        lv2_why_set(why, LV2_CODE_NO_REALISATION);
        return -1;
    }
    return 0;
}


/*
************************************************************************************************************************
*           GLOBAL FUNCTIONS: THE DEFAULT STATE
************************************************************************************************************************
*/

/* state:loadDefaultState over lilv-state, through the plugin's own URID map: 0, or -1 without a map */
int lv2_plugin_default_state(const struct lv2_plugin *p, LV2_Handle handle, const LV2_Feature *const *features)
{
    LV2_URID_Map *map = NULL;
    LilvInstance instance;
    LilvState *state;
    uint32_t i;

    for (i = 0; features && features[i]; i++)
        if (strcmp(features[i]->URI, LV2_URID__map) == 0)
            map = features[i]->data;
    if (!map || !g.world)
        return -1;      // the state's keys must be mapped by the plugin's own map
    state = L.lilv_state_new_from_world(g.world, map, p->uri_node);
    if (!state)
        return 0;       // no default state: nothing to restore
    instance.lv2_descriptor = p->desc;
    instance.lv2_handle = handle;
    instance.pimpl = NULL;
    L.lilv_state_restore(state, &instance, NULL, NULL, 0, features);
    L.lilv_state_free(state);
    return 0;
}
