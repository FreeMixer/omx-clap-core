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
* the default state.
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
#include <lv2/midi/midi.h>
#include <lv2/patch/patch.h>
#include <lv2/port-props/port-props.h>
#include <lv2/resize-port/resize-port.h>
#include <lv2/state/state.h>
#include <lv2/time/time.h>
#include <lv2/units/units.h>
#include <lv2/urid/urid.h>

#include "lv2_core_internal.h"
#include "lv2_ll_midi.h"


/*
************************************************************************************************************************
*           LOCAL DEFINES
************************************************************************************************************************
*/

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
    X(lilv_plugin_get_optional_features) X(lilv_node_as_int) X(lilv_node_is_uri) \
    X(lilv_port_is_a) X(lilv_port_get_symbol) X(lilv_port_get_name) X(lilv_port_get_range) X(lilv_port_get_value) \
    X(lilv_port_has_property) X(lilv_port_get_scale_points) X(lilv_scale_points_begin) X(lilv_scale_points_next) \
    X(lilv_port_supports_event) \
    X(lilv_scale_points_is_end) X(lilv_scale_points_get) X(lilv_scale_points_free) X(lilv_scale_point_get_label) \
    X(lilv_scale_point_get_value) X(lilv_nodes_begin) X(lilv_nodes_next) X(lilv_nodes_is_end) X(lilv_nodes_get) \
    X(lilv_nodes_contains) X(lilv_nodes_free) X(lilv_file_uri_parse) X(lilv_free) X(lilv_node_is_blank) X(lilv_nodes_size) \
    X(lilv_plugin_get_value) X(lilv_world_find_nodes) X(lilv_world_ask) \
    X(lilv_state_new_from_world) X(lilv_state_restore) X(lilv_state_free) X(lilv_state_new_from_instance) \
    X(lilv_state_to_string) X(lilv_state_new_from_string) X(lilv_world_load_resource) X(lilv_world_unload_resource)


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
    N_SUPPORTS, N_BUFFER_TYPE, N_SEQUENCE, N_PATCH_MESSAGE, N_RSZ_MIN, N_PATCH_WRITABLE, N_PATCH_PARAMETER,
    N_RDF_TYPE, N_RDFS_RANGE, N_RDFS_LABEL, N_SYMBOL, N_MINIMUM, N_MAXIMUM, N_DEFAULT,
    N_ATOM_FLOAT, N_ATOM_DOUBLE, N_ATOM_INT, N_ATOM_LONG, N_ATOM_BOOL, N_ATOM_STRING, N_ATOM_PATH, N_MIDI_EVENT, N_TIME_POSITION,
    N_IS_SIDECHAIN, N_PG_GROUP, N_PG_SIDECHAIN_OF, N_PG_MAIN_INPUT, N_PG_MAIN_OUTPUT, N_NAME, N_LABEL, N_PSET_PRESET, N_APPLIES_TO, N_UNIT,
    N_RANGE_STEPS, N_STATE_STATE, N_LL_MIDI, N_COUNT
};

static const char *const NODE_URIS[N_COUNT] =
{
    [N_AUDIO] = LV2_CORE__AudioPort,
    [N_CONTROL] = LV2_CORE__ControlPort,
    [N_CV] = LV2_CORE__CVPort,
    [N_ATOM] = LV2_ATOM__AtomPort,
    [N_EVENT] = LV2_EVENT__EventPort,
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
    [N_SUPPORTS] = LV2_ATOM__supports,
    [N_BUFFER_TYPE] = LV2_ATOM__bufferType,
    [N_SEQUENCE] = LV2_ATOM__Sequence,
    [N_PATCH_MESSAGE] = LV2_PATCH__Message,
    [N_RSZ_MIN] = LV2_RESIZE_PORT__minimumSize,
    [N_PATCH_WRITABLE] = LV2_PATCH__writable,
    [N_PATCH_PARAMETER] = LV2_CORE_PREFIX "Parameter",
    [N_RDF_TYPE] = "http://www.w3.org/1999/02/22-rdf-syntax-ns#type",
    [N_RDFS_RANGE] = "http://www.w3.org/2000/01/rdf-schema#range",
    [N_RDFS_LABEL] = "http://www.w3.org/2000/01/rdf-schema#label",
    [N_SYMBOL] = LV2_CORE__symbol,
    [N_MINIMUM] = LV2_CORE__minimum,
    [N_MAXIMUM] = LV2_CORE__maximum,
    [N_DEFAULT] = LV2_CORE__default,
    [N_ATOM_FLOAT] = LV2_ATOM__Float,
    [N_ATOM_DOUBLE] = LV2_ATOM__Double,
    [N_ATOM_INT] = LV2_ATOM__Int,
    [N_ATOM_LONG] = LV2_ATOM__Long,
    [N_ATOM_BOOL] = LV2_ATOM__Bool,
    [N_ATOM_STRING] = LV2_ATOM__String,
    [N_ATOM_PATH] = LV2_ATOM__Path,
    [N_MIDI_EVENT] = LV2_MIDI__MidiEvent,
    [N_TIME_POSITION] = LV2_TIME__Position,
    [N_IS_SIDECHAIN] = LV2_CORE_IS_SIDECHAIN,
    [N_PG_GROUP] = LV2_PORT_GROUPS__group,
    [N_PG_SIDECHAIN_OF] = LV2_PORT_GROUPS__sideChainOf,
    [N_PG_MAIN_INPUT] = LV2_PORT_GROUPS__mainInput,
    [N_PG_MAIN_OUTPUT] = LV2_PORT_GROUPS__mainOutput,
    [N_NAME] = LV2_CORE__name,
    [N_LABEL] = LV2_RDFS_LABEL,
    [N_PSET_PRESET] = "http://lv2plug.in/ns/ext/presets#Preset",
    [N_APPLIES_TO] = LV2_CORE__appliesTo,
    [N_UNIT] = LV2_UNITS__unit,
    [N_RANGE_STEPS] = LV2_PORT_PROPS__rangeSteps,
    [N_STATE_STATE] = LV2_STATE__state,
    [N_LL_MIDI] = OMX_LV2_LL_MIDI_PORT_URI,
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
    /* The bundle stays loaded when its last reference goes, and the next reference to its directory takes it as it is.
     * lilv hands a bundle's statements to sord, whose node table is a zix hash: zix 0.8.0 marks every removal with a
     * tombstone and does not reclaim them in a small table, so a world that unloads and reloads bundles ends in a
     * zix_hash_find that probes a full table for ever. A process uses a handful of bundles and reads each once. */
    if (b && b->refs > 0)
        b->refs--;
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

/* the range and the default: lv2:minimum and lv2:maximum as declared, an absent side -1e9 or 1e9 (the bounds a patch
 * parameter gets when it declares none: LV2 leaves such a port unbounded, CLAP needs a finite pair, and a pair of
 * [0,0] loses every write, #254); the default is lv2:default, else lv2:minimum, else 0, held inside the range */
static void port_range(const LilvPlugin *pl, const LilvPort *port, struct lv2_control *c)
{
    LilvNode *def = NULL, *min = NULL, *max = NULL;
    int has_def, has_min, has_max;

    L.lilv_port_get_range(pl, port, &def, &min, &max);
    c->def = node_float(def, &has_def);
    c->min = node_float(min, &has_min);
    c->max = node_float(max, &has_max);
    if (!has_min)
        c->min = -1e9f;
    if (!has_max)
        c->max = 1e9f;
    if (c->max < c->min)
        c->max = c->min;
    if (!has_def)
        c->def = has_min ? c->min : 0.0f;
    if (c->def < c->min)
        c->def = c->min;
    if (c->def > c->max)
        c->def = c->max;
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

/* units:unit when it names one of LV2's own units: its local name ("hz"), duplicated; NULL for none, a unit of another
 * vocabulary, or one the plugin defines itself (the CLAP face maps the name to the console's code) */
static char *port_unit(const LilvPlugin *pl, const LilvPort *port)
{
    LilvNodes *v = L.lilv_port_get_value(pl, port, g.node[N_UNIT]);
    LilvIter *it = v ? L.lilv_nodes_begin(v) : NULL;
    const LilvNode *n = it && !L.lilv_nodes_is_end(v, it) ? L.lilv_nodes_get(v, it) : NULL;
    const char *uri = n && L.lilv_node_is_uri(n) ? L.lilv_node_as_uri(n) : NULL;
    char *name = NULL;

    if (uri && strncmp(uri, LV2_UNITS_PREFIX, strlen(LV2_UNITS_PREFIX)) == 0 && uri[strlen(LV2_UNITS_PREFIX)])
        name = strdup(uri + strlen(LV2_UNITS_PREFIX));
    if (v)
        L.lilv_nodes_free(v);
    return name;
}

/* pprops:rangeSteps: the positions a control has from its minimum to its maximum, 2 or more; 0 for none */
static uint32_t port_steps(const LilvPlugin *pl, const LilvPort *port)
{
    LilvNodes *v = L.lilv_port_get_value(pl, port, g.node[N_RANGE_STEPS]);
    LilvIter *it = v ? L.lilv_nodes_begin(v) : NULL;
    const LilvNode *n = it && !L.lilv_nodes_is_end(v, it) ? L.lilv_nodes_get(v, it) : NULL;
    int steps = n && L.lilv_node_is_int(n) ? L.lilv_node_as_int(n) : 0;

    if (v)
        L.lilv_nodes_free(v);
    return steps >= 2 ? (uint32_t)steps : 0u;
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

static uint32_t nodes_count(const LilvNodes *nodes)
{
    uint32_t n = 0;
    LilvIter *it;

    for (it = nodes ? L.lilv_nodes_begin(nodes) : NULL; nodes && !L.lilv_nodes_is_end(nodes, it); it = L.lilv_nodes_next(nodes, it))
        n++;
    return n;
}

/* The first value of `predicate` on `subject`, as a number: 0 when it has none. */
static int world_float(const LilvNode *subject, int predicate, float *v)
{
    LilvNodes *nodes = L.lilv_world_find_nodes(g.world, subject, g.node[predicate], NULL);
    LilvIter *it = nodes ? L.lilv_nodes_begin(nodes) : NULL;
    int ok = 0;

    if (nodes && !L.lilv_nodes_is_end(nodes, it))
        *v = node_float(L.lilv_nodes_get(nodes, it), &ok);
    L.lilv_nodes_free(nodes);
    return ok;
}

/* The first value of `predicate` on `subject`, as a new string, or NULL. */
static char *world_string(const LilvNode *subject, int predicate)
{
    LilvNodes *nodes = L.lilv_world_find_nodes(g.world, subject, g.node[predicate], NULL);
    LilvIter *it = nodes ? L.lilv_nodes_begin(nodes) : NULL;
    char *s = NULL;

    if (nodes && !L.lilv_nodes_is_end(nodes, it) && L.lilv_node_as_string(L.lilv_nodes_get(nodes, it)))
        s = strdup(L.lilv_node_as_string(L.lilv_nodes_get(nodes, it)));
    L.lilv_nodes_free(nodes);
    return s;
}

/* the largest of `min` and the first rsz:minimumSize in `rsz` */
static uint32_t rsz_max(const LilvNodes *rsz, uint32_t min)
{
    LilvIter *it = rsz ? L.lilv_nodes_begin(rsz) : NULL;

    if (rsz && !L.lilv_nodes_is_end(rsz, it))
    {
        int ok = 0;
        const float v = node_float(L.lilv_nodes_get(rsz, it), &ok);

        if (ok && v > (float)min)
            return (uint32_t)v;
    }
    return min;
}

/* An atom port that is neither MIDI nor the time port, read after both. An input is a sequence (atom:bufferType
 * atom:Sequence, or none named): the first that supports patch:Message is the patch input, whatever else it lists (a
 * time:Position it also takes is forged into it); any other, up to LV2_CORE_ATOM_IN_PORTS, is fed an empty sequence at
 * every block, which the LV2 atom rules let a host do with a port it sends nothing to (LSP's in_ui, eq10q's, a GUI's
 * control port). An input that is no sequence, or one more, is refused with LV2_CODE_ATOM_IN: never the MIDI refusal,
 * since it takes no MIDI. An output is the patch output when it supports patch:Message and nothing else, and the first
 * one; any other is a notify output, up to LV2_CORE_NOTIFY_PORTS, given a buffer and discarded; one more is refused with
 * the MIDI refusal. */
static const char *atom_port(struct lv2_plugin *p, const LilvPlugin *pl, const LilvPort *port, int input, uint32_t index)
{
    LilvNodes *sup = L.lilv_port_get_value(pl, port, g.node[N_SUPPORTS]);
    LilvNodes *type = L.lilv_port_get_value(pl, port, g.node[N_BUFFER_TYPE]);
    LilvNodes *rsz = L.lilv_port_get_value(pl, port, g.node[N_RSZ_MIN]);
    const int patch = sup && L.lilv_nodes_contains(sup, g.node[N_PATCH_MESSAGE]);
    const int patch_only = patch && nodes_count(sup) == 1;
    const int sequence = !type || nodes_count(type) == 0 || L.lilv_nodes_contains(type, g.node[N_SEQUENCE]);
    const int one_size = !rsz || nodes_count(rsz) <= 1;
    const char *refused = input ? LV2_CODE_ATOM_IN : LV2_CODE_MIDI_IN;

    if (input && sequence && one_size && patch && p->patch_in < 0)
    {
        refused = NULL;
        p->patch_in = (int32_t)index;
        p->patch_time = L.lilv_nodes_contains(sup, g.node[N_TIME_POSITION]) ? 1 : 0;
        p->atom_min = rsz_max(rsz, p->atom_min);
    }
    else if (input && sequence && one_size && p->n_atom_in < LV2_CORE_ATOM_IN_PORTS)
    {
        refused = NULL;
        p->atom_in[p->n_atom_in++] = index;
        p->atom_in_min = rsz_max(rsz, p->atom_in_min);
    }
    else if (!input && patch_only && sequence && p->patch_out < 0 && one_size)
    {
        refused = NULL;
        p->patch_out = (int32_t)index;
        p->atom_min = rsz_max(rsz, p->atom_min);
    }
    else if (!input && p->n_notify < LV2_CORE_NOTIFY_PORTS && one_size)
    {
        refused = NULL;
        p->notify_out[p->n_notify++] = index;
        p->notify_min = rsz_max(rsz, p->notify_min);
    }
    L.lilv_nodes_free(sup);
    L.lilv_nodes_free(type);
    L.lilv_nodes_free(rsz);
    return refused;
}

/* The symbol the URI gives when the property has no lv2:symbol: the text after the last '#', else after the
 * last '/', every character outside [A-Za-z0-9_] as '_', and a leading digit prefixed with '_'. */
static char *symbol_of_uri(const char *uri)
{
    const char *tail = strrchr(uri, '#');
    const char *slash = strrchr(uri, '/');
    size_t n, i;
    char *s;

    if (!tail)
        tail = slash ? slash + 1 : uri;
    else
        tail++;
    n = strlen(tail);
    s = malloc(n + 2u);
    if (!s)
        return NULL;
    for (i = 0; i < n; i++)
    {
        const char c = tail[i];
        s[i] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ? c : '_';
    }
    s[n] = '\0';
    if (n == 0 || (s[0] >= '0' && s[0] <= '9'))
    {
        memmove(s + 1, s, n + 1u);
        s[0] = '_';
    }
    return s;
}

/* A patch:writable property is a numeric lv2:Parameter when it is typed so and its range is one of the numeric atoms;
 * the type it takes is the one of that range. 0 for anything else (a string or path property, or no range). */
static int patch_type_of(const LilvNode *prop, enum lv2_patch_type *type)
{
    static const struct { int node; enum lv2_patch_type type; } map[] =
    {
        { N_ATOM_FLOAT, LV2_PATCH_FLOAT }, { N_ATOM_DOUBLE, LV2_PATCH_DOUBLE }, { N_ATOM_INT, LV2_PATCH_INT },
        { N_ATOM_LONG, LV2_PATCH_LONG }, { N_ATOM_BOOL, LV2_PATCH_BOOL },
    };
    LilvNodes *range;
    size_t i;
    int found = 0;

    if (!L.lilv_world_ask(g.world, prop, g.node[N_RDF_TYPE], g.node[N_PATCH_PARAMETER]))
        return 0;
    range = L.lilv_world_find_nodes(g.world, prop, g.node[N_RDFS_RANGE], NULL);
    for (i = 0; range && !found && i < sizeof(map) / sizeof(map[0]); i++)
        if (L.lilv_nodes_contains(range, g.node[map[i].node]))
        {
            *type = map[i].type;
            found = 1;
        }
    L.lilv_nodes_free(range);
    return found;
}

/* A patch:writable property is a text property when it is an lv2:Parameter whose range is atom:String or atom:Path:
 * 1 with *is_path set, 0 for anything else. */
static int text_type_of(const LilvNode *prop, int *is_path)
{
    LilvNodes *range;
    int found = 0;

    if (!L.lilv_world_ask(g.world, prop, g.node[N_RDF_TYPE], g.node[N_PATCH_PARAMETER]))
        return 0;
    range = L.lilv_world_find_nodes(g.world, prop, g.node[N_RDFS_RANGE], NULL);
    if (range && L.lilv_nodes_contains(range, g.node[N_ATOM_PATH]))
        found = 1, *is_path = 1;
    else if (range && L.lilv_nodes_contains(range, g.node[N_ATOM_STRING]))
        found = 1, *is_path = 0;
    L.lilv_nodes_free(range);
    return found;
}

/* two patch:writable nodes by their text, for the order the ids are given in */
static int node_text_cmp(const void *a, const void *b)
{
    const char *x = L.lilv_node_as_string(*(const LilvNode *const *)a);
    const char *y = L.lilv_node_as_string(*(const LilvNode *const *)b);

    return strcmp(x ? x : "", y ? y : "");
}

/* The numeric patch:writable properties of the plugin, and its text ones, in the order of their URIs: patch:writable is
 * a set, and lilv lists it in no stable order, so the order is the URIs' own. A numeric property's id is the port count
 * plus its index in that order, counting every entry, so a string property does not move the ids of the numeric ones,
 * and the same plugin gets the same ids in every instance and every process. Versions before this one gave them in
 * lilv's order, so a plugin with two or more such properties may have had other ids there. */
static const char *read_patches(struct lv2_plugin *p, const LilvPlugin *pl)
{
    LilvNodes *writable = L.lilv_plugin_get_value(pl, g.node[N_PATCH_WRITABLE]);
    const uint32_t n = nodes_count(writable);
    const LilvNode **order = calloc(n ? n : 1u, sizeof(*order));
    uint32_t i = 0, k = 0;
    LilvIter *it;

    p->patches = calloc(n ? n : 1u, sizeof(*p->patches));
    p->texts = calloc(n ? n : 1u, sizeof(*p->texts));
    if (!p->patches || !p->texts || !order)
    {
        free(order);
        L.lilv_nodes_free(writable);
        return LV2_CODE_NO_REALISATION;
    }
    for (it = writable ? L.lilv_nodes_begin(writable) : NULL; writable && !L.lilv_nodes_is_end(writable, it); it = L.lilv_nodes_next(writable, it))
        order[i++] = L.lilv_nodes_get(writable, it);
    qsort(order, i, sizeof(*order), node_text_cmp);
    for (i = 0; i < n; i++)
    {
        const LilvNode *prop = order[i];
        const char *uri = prop ? L.lilv_node_as_uri(prop) : NULL;
        struct lv2_patch *pp = &p->patches[k];
        enum lv2_patch_type type = LV2_PATCH_FLOAT;     // set by patch_type_of whenever it is read
        float min = -1e9f, max = 1e9f, def;
        int is_path = 0;

        if (uri && text_type_of(prop, &is_path))
        {
            struct lv2_patch_text *t = &p->texts[p->n_texts++];

            t->uri = strdup(uri);
            t->is_path = is_path;
            t->name = world_string(prop, N_RDFS_LABEL);
            if (!t->name)
                t->name = strdup(uri);
            if (!t->uri || !t->name)
            {
                free(order);
                L.lilv_nodes_free(writable);
                return LV2_CODE_NO_REALISATION;     // counted, so lv2_plugin_close frees what it holds
            }
            continue;
        }
        if (!uri || !patch_type_of(prop, &type))
            continue;
        pp->id = p->n_ports + i;
        pp->uri = strdup(uri);
        pp->type = type;
        pp->symbol = world_string(prop, N_SYMBOL);
        if (!pp->symbol)
            pp->symbol = symbol_of_uri(uri);
        pp->name = world_string(prop, N_RDFS_LABEL);
        if (!pp->name)
            pp->name = strdup(pp->symbol ? pp->symbol : uri);
        if (type == LV2_PATCH_BOOL)
        {
            min = 0.0f;
            max = 1.0f;
            pp->props = LV2_PROP_TOGGLED;
        }
        else if (type == LV2_PATCH_INT || type == LV2_PATCH_LONG)
            pp->props = LV2_PROP_INTEGER;
        world_float(prop, N_MINIMUM, &min);
        world_float(prop, N_MAXIMUM, &max);
        def = min;
        world_float(prop, N_DEFAULT, &def);
        pp->min = min;
        pp->max = max;
        pp->def = def;
        p->n_patches = k + 1u;
        if (!pp->uri || !pp->symbol || !pp->name)
        {
            free(order);
            L.lilv_nodes_free(writable);
            return LV2_CODE_NO_REALISATION;     // the entry is counted, so lv2_plugin_close frees what it holds
        }
        k++;
    }
    free(order);
    L.lilv_nodes_free(writable);
    p->n_patches = k;
    return NULL;
}

/* An atom input the transport is forged into: a sequence whose atom:supports names time:Position and not patch:Message */
static int port_takes_time(const LilvPlugin *pl, const LilvPort *port)
{
    LilvNodes *s = L.lilv_port_get_value(pl, port, g.node[N_SUPPORTS]);
    LilvNodes *type = L.lilv_port_get_value(pl, port, g.node[N_BUFFER_TYPE]);
    const int sequence = !type || nodes_count(type) == 0 || L.lilv_nodes_contains(type, g.node[N_SEQUENCE]);
    const int takes = s && sequence && L.lilv_nodes_contains(s, g.node[N_TIME_POSITION])
                      && !L.lilv_nodes_contains(s, g.node[N_PATCH_MESSAGE]);

    L.lilv_nodes_free(s);
    L.lilv_nodes_free(type);
    return takes;
}

/* a node's string, duplicated, from the first of `nodes`; NULL when there is none. The nodes stay the caller's. */
static char *first_string(LilvNodes *nodes)
{
    LilvIter *it = nodes ? L.lilv_nodes_begin(nodes) : NULL;

    return it && !L.lilv_nodes_is_end(nodes, it) ? strdup(L.lilv_node_as_string(L.lilv_nodes_get(nodes, it))) : NULL;
}

/* whether a port is an audio port of the side `is_input` names */
static int audio_of(const LilvPlugin *pl, uint32_t i, int is_input)
{
    const LilvPort *port = L.lilv_plugin_get_port_by_index(pl, i);

    return L.lilv_port_is_a(pl, port, g.node[N_AUDIO]) && L.lilv_port_is_a(pl, port, g.node[is_input ? N_INPUT : N_OUTPUT]);
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

/* the name of an audio port: its group's lv2:name, else the group's rdfs:label, else the first member's lv2:name, else
 * its symbol; duplicated. The group is read from the member's own pg:group node, so a blank-node group names it too. */
static char *port_name(const LilvPlugin *pl, int grouped, uint32_t first)
{
    const LilvPort *port = L.lilv_plugin_get_port_by_index(pl, first);
    LilvNodes *grp = grouped ? L.lilv_port_get_value(pl, port, g.node[N_PG_GROUP]) : NULL;
    LilvIter *it = grp ? L.lilv_nodes_begin(grp) : NULL;
    char *s = NULL;

    if (it && !L.lilv_nodes_is_end(grp, it))
    {
        s = world_string(L.lilv_nodes_get(grp, it), N_NAME);
        if (!s)
            s = world_string(L.lilv_nodes_get(grp, it), N_LABEL);
    }
    L.lilv_nodes_free(grp);
    if (!s)
        s = node_dup(L.lilv_port_get_name(pl, port), L.lilv_node_as_string(L.lilv_port_get_symbol(pl, port)));
    return s;
}

/* the kind a port's channel count gives it: one is mono, two stereo, more unknown */
static uint32_t kind_of_count(uint32_t n)
{
    return n == 1 ? LV2_PORT_MONO : n == 2 ? LV2_PORT_STEREO : LV2_PORT_UNKNOWN;
}

/* a member's lv2:designation, as a code: a CLAP surround id (CLAP_SURROUND_FL 0 ... CLAP_SURROUND_SR 10, named by number:
 * the LV2 host holds no CLAP), LV2_CORE_MAP_SIDE for a mid-side group's side, ACN k as DES_ACN + k */
#define DES_ACN                         0x40u
#define DES_NONE                        0xffu
#define PG                              LV2_PORT_GROUPS_PREFIX
enum { FL = 0, FR = 1, FC = 2, LFE = 3, BL = 4, BR = 5, FLC = 6, FRC = 7, BC = 8, SL = 9, SR = 10 };
#define A(k)                            (DES_ACN + (k))

static const struct
{
    const char *uri;
    uint8_t code;
} DESIGNATIONS[] =
{
    { PG "left", FL }, { PG "right", FR }, { PG "center", FC }, { PG "lowFrequencyEffects", LFE }, { PG "rearLeft", BL },
    { PG "rearRight", BR }, { PG "centerLeft", FLC }, { PG "centerRight", FRC }, { PG "rearCenter", BC },
    { PG "sideLeft", SL }, { PG "sideRight", SR }, { PG "side", LV2_CORE_MAP_SIDE },
    { PG "ACN0", A(0) }, { PG "ACN1", A(1) }, { PG "ACN2", A(2) }, { PG "ACN3", A(3) }, { PG "ACN4", A(4) },
    { PG "ACN5", A(5) }, { PG "ACN6", A(6) }, { PG "ACN7", A(7) }, { PG "ACN8", A(8) }, { PG "ACN9", A(9) },
    { PG "ACN10", A(10) }, { PG "ACN11", A(11) }, { PG "ACN12", A(12) }, { PG "ACN13", A(13) }, { PG "ACN14", A(14) },
    { PG "ACN15", A(15) },
};

/* the group types port-groups.ttl declares elements for: the kind each gives, and its elements' designations in the
 * ttl's pg:element order (which is the channel order and the map's) */
static const struct
{
    const char *uri;
    uint32_t kind;
    uint32_t n;
    uint8_t order[16];
} GROUP_TYPES[] =
{
    { PG "MonoGroup", LV2_PORT_MONO, 1, { FC } },
    { PG "StereoGroup", LV2_PORT_STEREO, 2, { FL, FR } },
    { PG "MidSideGroup", LV2_PORT_STEREO, 2, { FC, LV2_CORE_MAP_SIDE } },
    { PG "ThreePointZeroGroup", LV2_PORT_SURROUND, 3, { FL, FR, BC } },
    { PG "FourPointZeroGroup", LV2_PORT_SURROUND, 4, { FL, FC, FR, BC } },
    { PG "FivePointZeroGroup", LV2_PORT_SURROUND, 5, { FL, FC, FR, BL, BR } },
    { PG "FivePointOneGroup", LV2_PORT_SURROUND, 6, { FL, FC, FR, BL, BR, LFE } },
    { PG "SixPointOneGroup", LV2_PORT_SURROUND, 7, { FL, FC, FR, SL, SR, BC, LFE } },
    { PG "SevenPointOneGroup", LV2_PORT_SURROUND, 8, { FL, FC, FR, SL, SR, BL, BR, LFE } },
    { PG "SevenPointOneWideGroup", LV2_PORT_SURROUND, 8, { FL, FLC, FC, FRC, FR, BL, BR, LFE } },
    // full-order ambisonic streams: ACN 0 to (order+1)^2 - 1, in order
    { PG "AmbisonicBH1P1Group", LV2_PORT_AMBISONIC, 4, { A(0), A(1), A(2), A(3) } },
    { PG "AmbisonicBH2P2Group", LV2_PORT_AMBISONIC, 9, { A(0), A(1), A(2), A(3), A(4), A(5), A(6), A(7), A(8) } },
    { PG "AmbisonicBH3P3Group", LV2_PORT_AMBISONIC, 16,
      { A(0), A(1), A(2), A(3), A(4), A(5), A(6), A(7), A(8), A(9), A(10), A(11), A(12), A(13), A(14), A(15) } },
    // mixed-order streams, which a CLAP ambisonic port cannot express: discrete channels, their ACNs in the map
    { PG "AmbisonicBH1P0Group", LV2_PORT_UNKNOWN, 3, { A(0), A(1), A(3) } },
    { PG "AmbisonicBH2P0Group", LV2_PORT_UNKNOWN, 5, { A(0), A(1), A(3), A(4), A(8) } },
    { PG "AmbisonicBH2P1Group", LV2_PORT_UNKNOWN, 6, { A(0), A(1), A(2), A(3), A(4), A(8) } },
    { PG "AmbisonicBH3P0Group", LV2_PORT_UNKNOWN, 7, { A(0), A(1), A(3), A(4), A(8), A(9), A(15) } },
    { PG "AmbisonicBH3P1Group", LV2_PORT_UNKNOWN, 8, { A(0), A(1), A(2), A(3), A(4), A(8), A(9), A(15) } },
    { PG "AmbisonicBH3P2Group", LV2_PORT_UNKNOWN, 11, { A(0), A(1), A(2), A(3), A(4), A(5), A(6), A(7), A(8), A(9), A(15) } },
};

#undef A
#undef PG

/* The kind a group type gives its port, and its elements' designations in order: 1, or 0 for a type the table lacks
 * (pg:Group, pg:InputGroup, pg:OutputGroup, pg:DiscreteGroup, another vocabulary's). */
static int group_kind(const char *type_uri, uint32_t *kind, const uint8_t **order, uint32_t *n)
{
    size_t t;

    for (t = 0; type_uri && t < sizeof(GROUP_TYPES) / sizeof(GROUP_TYPES[0]); t++)
    {
        if (strcmp(type_uri, GROUP_TYPES[t].uri) == 0)
        {
            *kind = GROUP_TYPES[t].kind;
            *order = GROUP_TYPES[t].order;
            *n = GROUP_TYPES[t].n;
            return 1;
        }
    }
    return 0;
}

/* a port's lv2:designation as a code of DESIGNATIONS, DES_NONE without one the table names */
static uint8_t designation_of(const LilvPlugin *pl, uint32_t index)
{
    LilvNodes *d = L.lilv_port_get_value(pl, L.lilv_plugin_get_port_by_index(pl, index), g.node[N_DESIGNATION]);
    char *uri = first_string(d);
    uint8_t code = DES_NONE;
    size_t k;

    for (k = 0; uri && k < sizeof(DESIGNATIONS) / sizeof(DESIGNATIONS[0]); k++)
        if (strcmp(uri, DESIGNATIONS[k].uri) == 0)
            code = DESIGNATIONS[k].code;
    L.lilv_nodes_free(d);
    free(uri);
    return code;
}

/* A group's port in its type's element order: the kind and the map from the table when the group's rdf:type is in it
 * and its members carry the type's designations, one each; otherwise the port stays in port index order with the kind
 * its count gives. A mixed-order ambisonic group is ordered and mapped but of no kind. */
static void group_order(struct lv2_audio_port *a, const LilvPlugin *pl)
{
    const LilvPort *first = L.lilv_plugin_get_port_by_index(pl, a->port[0]);
    LilvNodes *grp = L.lilv_port_get_value(pl, first, g.node[N_PG_GROUP]);
    LilvIter *it = grp ? L.lilv_nodes_begin(grp) : NULL;
    LilvNodes *types = it && !L.lilv_nodes_is_end(grp, it)
                       ? L.lilv_world_find_nodes(g.world, L.lilv_nodes_get(grp, it), g.node[N_RDF_TYPE], NULL) : NULL;
    uint32_t kind = LV2_PORT_UNKNOWN, n = 0, port[LV2_CORE_PORT_CHANNELS], c, e;
    const uint8_t *order = NULL;
    int typed = 0, ok;

    for (it = types ? L.lilv_nodes_begin(types) : NULL; types && !typed && !L.lilv_nodes_is_end(types, it); it = L.lilv_nodes_next(types, it))
        typed = group_kind(L.lilv_node_as_string(L.lilv_nodes_get(types, it)), &kind, &order, &n);
    L.lilv_nodes_free(types);
    L.lilv_nodes_free(grp);
    ok = typed && n == a->n;
    for (e = 0; ok && e < n; e++)
    {
        // the member designated as element e: exactly one
        uint32_t found = 0;

        for (c = 0; c < a->n; c++)
            if (designation_of(pl, a->port[c]) == order[e])
                port[e] = a->port[c], found++;
        ok = found == 1;
    }
    if (!ok)
        return;
    memcpy(a->port, port, n * sizeof(port[0]));
    for (e = 0; e < n; e++)
        a->map[e] = order[e] >= DES_ACN && order[e] < DES_ACN + 16u ? (uint8_t)(order[e] - DES_ACN) : order[e];
    a->kind = kind;
}

/* one audio port of a side, as the port-group rules read it */
struct side_port
{
    uint32_t index;                 // the plugin's port index
    char *group;                    // pg:group, NULL: ungrouped
    int side_chain;                 // lv2:isSideChain, or pg:sideChainOf on the port or on its group (rule 4)
    int main;                       // a member of the main port
};

/* The width of a side's main port, the one rule 3 compares with: the members of its main group when it has one by rule 1
 * or 2 (the declared group, when a port of the side carries it, else the first group), else its ungrouped audio ports.
 * Side chains, and every group but the main one, are never counted. */
static uint32_t side_main_width(const LilvPlugin *pl, uint32_t n, int is_input)
{
    LilvNodes *mg = L.lilv_plugin_get_value(pl, g.node[is_input ? N_PG_MAIN_INPUT : N_PG_MAIN_OUTPUT]);
    char *declared = first_string(mg), *first = NULL;
    uint32_t i, plain = 0, in_declared = 0, in_first = 0;

    L.lilv_nodes_free(mg);
    for (i = 0; i < n; i++)
    {
        const LilvPort *port = L.lilv_plugin_get_port_by_index(pl, i);
        char *group;

        if (!audio_of(pl, i, is_input) || L.lilv_port_has_property(pl, port, g.node[N_IS_SIDECHAIN])
            || port_has(pl, port, N_PG_SIDECHAIN_OF) || group_has(pl, port, N_PG_SIDECHAIN_OF))
            continue;
        group = port_group(pl, port);
        if (!group)
            plain++;
        else
        {
            in_declared += declared && strcmp(group, declared) == 0;
            if (!first)
                first = strdup(group);
            in_first += first && strcmp(group, first) == 0;
        }
        free(group);
    }
    free(declared);
    free(first);
    return in_declared ? in_declared : first ? in_first : plain;
}

/* One port of the description: its channels (the members, at most LV2_CORE_PORT_CHANNELS of them: one more is too wide),
 * its name, its kind and its map. A group is ordered by its type (group_order); an ungrouped port, and a group the
 * table leaves of no kind, keeps port index order. A port of one or two channels of no kind is mono or stereo by its
 * count, the CLAP port types for those widths. */
static const char *describe(struct lv2_audio_port *a, const LilvPlugin *pl, const char *group, const uint32_t *members, uint32_t n)
{
    if (n > LV2_CORE_PORT_CHANNELS)
        return LV2_CODE_PORT_TOO_WIDE;
    memcpy(a->port, members, n * sizeof(*members));
    a->n = n;
    a->kind = kind_of_count(n);
    if (group)
        group_order(a, pl);
    a->name = port_name(pl, group != NULL, members[0]);
    return a->name ? NULL : LV2_CODE_NO_REALISATION;
}

/*
 * One side's audio ports, by the port-group rules: side chains (lv2:isSideChain, pg:sideChainOf on the port or on its
 * group) are auxiliary whatever else applies (rule 4); of the rest,
 *  1. a main group declared (pg:mainInput, pg:mainOutput) that has members on this side is the main port;
 *  2. else the group of the first grouped port is;
 *  3. else, no group at all: up to two ports are the main port, more than two as many as the other side's main port is
 *     wide (`other`: its main group's members by rule 1 or 2, else its ungrouped ports; side chains and auxiliary groups
 *     never count) are one main port of that width, and otherwise the first two are the main port.
 * Every other audio port is auxiliary: the members of one pg:group one port, an ungrouped port one mono port, in the
 * order of their first member, up to LV2_CORE_AUX_PORTS of them with the side's CV ports (one more is too many ports).
 * A port of more than LV2_CORE_PORT_CHANNELS channels is too wide. Fills p->main_in and p->aux_in, or p->main_out and
 * p->aux_out; returns the refusal, or NULL.
 */
static const char *read_side(struct lv2_plugin *p, const LilvPlugin *pl, int is_input)
{
    const uint32_t n = L.lilv_plugin_get_num_ports(pl), other = side_main_width(pl, n, !is_input);
    struct lv2_audio_port *main = is_input ? &p->main_in : &p->main_out;
    struct lv2_audio_port *aux = is_input ? p->aux_in : p->aux_out;
    uint32_t *n_aux = is_input ? &p->n_aux_in : &p->n_aux_out;
    const uint32_t n_cv = is_input ? p->n_cv_in : p->n_cv_out;
    struct side_port *sp = calloc(n ? n : 1u, sizeof(*sp));
    uint32_t *members = calloc(n ? n : 1u, sizeof(*members));
    LilvNodes *mg = L.lilv_plugin_get_value(pl, g.node[is_input ? N_PG_MAIN_INPUT : N_PG_MAIN_OUTPUT]);
    char *declared = first_string(mg);
    const char *main_group = NULL, *refusal = NULL;
    uint32_t ns = 0, i, j, plain = 0, grouped = 0, nm;

    L.lilv_nodes_free(mg);
    if (!sp || !members)
        refusal = LV2_CODE_NO_REALISATION;
    for (i = 0; i < n && !refusal; i++)
    {
        const LilvPort *port;

        if (!audio_of(pl, i, is_input))
            continue;
        port = L.lilv_plugin_get_port_by_index(pl, i);
        sp[ns].index = i;
        sp[ns].group = port_group(pl, port);
        sp[ns].side_chain = L.lilv_port_has_property(pl, port, g.node[N_IS_SIDECHAIN]) || port_has(pl, port, N_PG_SIDECHAIN_OF)
                            || group_has(pl, port, N_PG_SIDECHAIN_OF);
        if (!sp[ns].side_chain)
        {
            plain++;
            grouped += sp[ns].group != NULL;
            // rule 1: the declared main group, when a port of this side carries it
            if (declared && sp[ns].group && strcmp(sp[ns].group, declared) == 0)
                main_group = declared;
        }
        ns++;
    }
    // rule 2: the first group in port order
    for (i = 0; i < ns && !refusal && !main_group && grouped; i++)
        if (!sp[i].side_chain && sp[i].group)
            main_group = sp[i].group;
    // the main port's members: the main group's, or by rule 3 among ungrouped ports
    for (i = 0, nm = 0; i < ns && !refusal; i++)
    {
        if (sp[i].side_chain)
            continue;
        if (main_group)
            sp[i].main = sp[i].group && strcmp(sp[i].group, main_group) == 0;
        else
            sp[i].main = plain <= 2 || plain == other || nm < 2;
        if (sp[i].main)
            members[nm++] = sp[i].index;
    }
    if (!refusal && nm)
        refusal = describe(main, pl, main_group, members, nm);
    // the auxiliary ports, in the order of their first member. A member of the main group that carries lv2:isSideChain is
    // one of them (rule 4), named after the main group it is declared in
    for (i = 0; i < ns && !refusal; i++)
    {
        uint32_t m = 0;

        if (sp[i].main || sp[i].index == UINT32_MAX)
            continue;
        for (j = i; j < ns; j++)
        {
            if (sp[j].main || sp[j].index == UINT32_MAX)
                continue;
            if (j == i || (sp[i].group && sp[j].group && strcmp(sp[i].group, sp[j].group) == 0))
            {
                members[m++] = sp[j].index;
                if (j != i)
                    sp[j].index = UINT32_MAX;   // taken by this port
            }
        }
        if (*n_aux + n_cv >= LV2_CORE_AUX_PORTS)
            refusal = LV2_CODE_TOO_MANY_PORTS;
        else
            refusal = describe(&aux[(*n_aux)++], pl, sp[i].group, members, m);
    }
    for (i = 0; sp && i < ns; i++)
        free(sp[i].group);
    free(sp);
    free(members);
    free(declared);
    return refusal;
}

/* Read every port, or name the refusal, before any binary, with the codes of lv2_core.h. */
static const char *read_ports(struct lv2_plugin *p, const LilvPlugin *pl)
{
    const uint32_t n = L.lilv_plugin_get_num_ports(pl);
    uint32_t nout = 0, i;
    const char *refusal;
    int has_bypass = 0;

    p->controls = calloc(n ? n : 1u, sizeof(*p->controls));
    if (!p->controls)
        return LV2_CODE_NO_REALISATION;
    p->latency_port = -1;
    p->patch_in = p->patch_out = -1;
    p->midi_in_port = -1;
    p->midi_out_port = -1;
    p->time_port = -1;
    p->n_ports = n;
    for (i = 0; i < n; i++)
    {
        const LilvPort *port = L.lilv_plugin_get_port_by_index(pl, i);
        const int input = L.lilv_port_is_a(pl, port, g.node[N_INPUT]);
        struct lv2_control *c;
        const char *symbol;

        if (L.lilv_port_is_a(pl, port, g.node[N_LL_MIDI]))
        {
            // an llext:MidiPort (ll-plugins' MIDI port, the event extension's predecessor: zynadd's MIDI input) is a
            // MIDI port as the atom one is, over an LV2_MIDI buffer (lv2_ll_midi.h), and the only MIDI port in its
            // direction: beside any other MIDI port, in either index order (a second llext:MidiPort, or an atom or
            // ev:EventPort MIDI port, which the arms below refuse when the first was an ll port), the plugin is one more
            // MIDI port than the adapter feeds (#284)
            if (input ? p->midi_in_port >= 0 : p->midi_out_port >= 0)
                return LV2_CODE_MIDI_IN;
            if (input)
            {
                p->midi_in_port = (int32_t)i;
                p->midi_in_ll = 1;
            }
            else
            {
                p->midi_out_port = (int32_t)i;
                p->midi_out_ll = 1;
            }
            continue;
        }
        if (L.lilv_port_is_a(pl, port, g.node[N_EVENT]))
        {
            // an ev:EventPort (the event extension, atom's predecessor: ll-plugins' rudolf556 and control2midi) that
            // supports midi:MidiEvent is a MIDI port as the atom one is, over an LV2_Event_Buffer: the first each way
            // is the note path's, one after it is fed an empty buffer every block or offered one and never read. One
            // that supports no midi:MidiEvent carries events no buffer here writes or reads
            uint32_t *count = input ? &p->n_event_in : &p->n_event_out;

            if (!L.lilv_port_supports_event(pl, port, g.node[N_MIDI_EVENT]))
                return LV2_CODE_MIDI_IN;
            if (input ? p->midi_in_port < 0 : p->midi_out_port < 0)
            {
                if (input)
                {
                    p->midi_in_port = (int32_t)i;
                    p->midi_in_event = 1;
                }
                else
                {
                    p->midi_out_port = (int32_t)i;
                    p->midi_out_event = 1;
                }
                continue;
            }
            if (input ? p->midi_in_ll : p->midi_out_ll)       // an llext:MidiPort is the only MIDI port its way
                return LV2_CODE_MIDI_IN;
            if (*count >= (input ? LV2_CORE_ATOM_IN_PORTS : LV2_CORE_NOTIFY_PORTS))
                return LV2_CODE_MIDI_IN;
            (input ? p->event_in : p->event_out)[(*count)++] = i;
            continue;
        }
        if (L.lilv_port_is_a(pl, port, g.node[N_ATOM]) && L.lilv_port_supports_event(pl, port, g.node[N_MIDI_EVENT]))
        {
            // a MIDI port is an atom:Sequence that supports midi:MidiEvent, one input and one output at most. One that
            // also takes patch:Message is the patch port too: one sequence the note path and the parameter path both
            // fill, in frame order, or both readers read (the lsp samplers, midimap, midifile); a second patch port
            // beside it is one more than the mapping carries
            // a MIDI port after the first: an input is fed an empty sequence every block, an output is offered a buffer
            // and drained, as the other atom ports are (carla's midijoin, 16 inputs; midisplit, 16 outputs); the note path
            // is the first port's in index order
            if (input ? p->midi_in_port >= 0 : p->midi_out_port >= 0)
            {
                LilvNodes *rsz;

                if (input ? p->midi_in_ll : p->midi_out_ll)   // an llext:MidiPort is the only MIDI port its way
                    return LV2_CODE_MIDI_IN;
                rsz = L.lilv_port_get_value(pl, port, g.node[N_RSZ_MIN]);

                if (input ? p->n_atom_in >= LV2_CORE_ATOM_IN_PORTS : p->n_notify >= LV2_CORE_NOTIFY_PORTS)
                {
                    L.lilv_nodes_free(rsz);
                    return LV2_CODE_MIDI_IN;
                }
                if (input)
                {
                    p->atom_in[p->n_atom_in++] = i;
                    p->atom_in_min = rsz_max(rsz, p->atom_in_min);
                }
                else
                {
                    p->notify_out[p->n_notify++] = i;
                    p->notify_min = rsz_max(rsz, p->notify_min);
                }
                L.lilv_nodes_free(rsz);
                continue;
            }
            if (L.lilv_port_supports_event(pl, port, g.node[N_PATCH_MESSAGE]))
            {
                LilvNodes *rsz = L.lilv_port_get_value(pl, port, g.node[N_RSZ_MIN]);

                if (input ? p->patch_in >= 0 : p->patch_out >= 0)
                {
                    L.lilv_nodes_free(rsz);
                    return input ? LV2_CODE_ATOM_IN : LV2_CODE_MIDI_IN;
                }
                if (input)
                {
                    p->patch_in = (int32_t)i;
                    p->patch_in_shared = 1;
                    p->patch_time = L.lilv_port_supports_event(pl, port, g.node[N_TIME_POSITION]) ? 1 : 0;
                }
                else
                {
                    p->patch_out = (int32_t)i;
                    p->patch_out_shared = 1;
                }
                p->atom_min = rsz_max(rsz, p->atom_min);
                L.lilv_nodes_free(rsz);
            }
            else
            {
                LilvNodes *rsz = L.lilv_port_get_value(pl, port, g.node[N_RSZ_MIN]);

                if (input)
                    p->midi_time = L.lilv_port_supports_event(pl, port, g.node[N_TIME_POSITION]) ? 1 : 0;
                p->midi_min = rsz_max(rsz, p->midi_min);
                L.lilv_nodes_free(rsz);
            }
            if (input)
                p->midi_in_port = (int32_t)i;
            else
                p->midi_out_port = (int32_t)i;
            continue;
        }
        if (L.lilv_port_is_a(pl, port, g.node[N_ATOM]) && input && p->time_port < 0 && port_takes_time(pl, port))
        {
            // the transport's port: a sequence input that takes time:Position and no patch:Message (§6.6); whatever else it
            // lists (atom:Sequence, the plugin's own objects) it is sent nothing of
            p->time_port = (int32_t)i;
            continue;
        }
        if (L.lilv_port_is_a(pl, port, g.node[N_ATOM]))
        {
            const char *refused = atom_port(p, pl, port, input, i);

            if (refused)
                return refused;
            continue;
        }
        if (L.lilv_port_is_a(pl, port, g.node[N_CV]))
        {
            // a CV port is a mono audio port of its own, each way, counted with the side's auxiliary ports against their
            // limit (read_side adds the auxiliary ports to this count); CV ports alone past it are refused here
            uint32_t *count = input ? &p->n_cv_in : &p->n_cv_out;
            char **name = input ? p->cv_in_name : p->cv_out_name;
            const char *sym = L.lilv_node_as_string(L.lilv_port_get_symbol(pl, port));

            if (*count >= LV2_CORE_AUX_PORTS)
                return LV2_CODE_TOO_MANY_PORTS;
            name[*count] = node_dup(L.lilv_port_get_name(pl, port), sym ? sym : "");
            if (!name[*count])
                return LV2_CODE_NO_REALISATION;
            (input ? p->cv_in : p->cv_out)[(*count)++] = i;
            continue;
        }
        if (L.lilv_port_is_a(pl, port, g.node[N_AUDIO]))
        {
            nout += !input;
            continue;
        }
        if (!L.lilv_port_is_a(pl, port, g.node[N_CONTROL]))
            return LV2_CODE_NO_REALISATION;
        symbol = L.lilv_node_as_string(L.lilv_port_get_symbol(pl, port));
        if (!input && p->latency_port < 0
            && (port_designated(pl, port, N_LATENCY) || L.lilv_port_has_property(pl, port, g.node[N_REPORTS_LATENCY])))
        {
            struct lv2_control range;

            p->latency_port = (int32_t)i;
            port_range(pl, port, &range);
            p->latency_def = range.def;
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
        c->unit = port_unit(pl, port);
        c->range_steps = port_steps(pl, port);
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
    refusal = read_side(p, pl, 1);
    if (!refusal)
        refusal = read_side(p, pl, 0);
    if (refusal)
        return refusal;
    p->in_legs = p->main_in.n;
    p->legs = p->main_out.n;
    if (nout == 0)
    {
        // no audio output. A MIDI tool (a MIDI output), an analyser (audio inputs), a control-only plugin (control ports
        // alone) or a MIDI sink (a MIDI input and control ports) is hosted with the strip's audio passing through it
        // untouched, the host's placement, never the plugin's own audio (#183, #253); its control outputs read after
        // every block. A plugin with no port at all has nothing the host feeds or reads
        if (n == 0)
            return LV2_CODE_NO_AUDIO_OUTPUT;
        p->passthrough = 1;
        p->legs = p->in_legs;
    }
    else if (p->main_out.n == 0)
    {
        return LV2_CODE_NO_AUDIO_OUTPUT;    // every output a side chain: no main output to carry
    }
    if (p->patch_in >= 0)
        return read_patches(p, pl);     // a property is writable only through an input port
    return NULL;
}

/* the first of a port's roles, in lv2_port_role's order: a control is the next of controls[*k], and takes it */
static uint16_t port_role(const struct lv2_plugin *p, uint32_t i, uint32_t *k, uint32_t *leg, uint32_t *c)
{
    *leg = 0; /* the main legs and the patch/MIDI/time ports return early: their leg is 0, never unset */
    for (*c = 0; *c < p->main_in.n; (*c)++)
        if (p->main_in.port[*c] == i)
            return LV2_ROLE_MAIN_IN;
    for (*c = 0; *c < p->main_out.n; (*c)++)
        if (p->main_out.port[*c] == i)
            return LV2_ROLE_MAIN_OUT;
    *c = 0;
    if (p->patch_in >= 0 && !p->patch_in_shared && (uint32_t)p->patch_in == i)
        return LV2_ROLE_PATCH_IN;
    if (p->patch_out >= 0 && !p->patch_out_shared && (uint32_t)p->patch_out == i)
        return LV2_ROLE_PATCH_OUT;
    if (p->midi_in_port >= 0 && (uint32_t)p->midi_in_port == i)
        return LV2_ROLE_MIDI_IN;
    if (p->midi_out_port >= 0 && (uint32_t)p->midi_out_port == i)
        return LV2_ROLE_MIDI_OUT;
    if (p->time_port >= 0 && (uint32_t)p->time_port == i)
        return LV2_ROLE_TIME;
    for (*leg = 0; *leg < p->n_notify; (*leg)++)
        if (p->notify_out[*leg] == i)
            return LV2_ROLE_NOTIFY;
    for (*leg = 0; *leg < p->n_atom_in; (*leg)++)
        if (p->atom_in[*leg] == i)
            return LV2_ROLE_ATOM_IN;
    for (*leg = 0; *leg < p->n_event_in; (*leg)++)
        if (p->event_in[*leg] == i)
            return LV2_ROLE_EVENT_IN;
    for (*leg = 0; *leg < p->n_event_out; (*leg)++)
        if (p->event_out[*leg] == i)
            return LV2_ROLE_EVENT_OUT;
    for (*leg = 0; *leg < p->n_aux_in; (*leg)++)
        for (*c = 0; *c < p->aux_in[*leg].n; (*c)++)
            if (p->aux_in[*leg].port[*c] == i)
                return LV2_ROLE_AUX_IN;
    for (*leg = 0; *leg < p->n_aux_out; (*leg)++)
        for (*c = 0; *c < p->aux_out[*leg].n; (*c)++)
            if (p->aux_out[*leg].port[*c] == i)
                return LV2_ROLE_AUX_OUT;
    *c = 0;
    for (*leg = 0; *leg < p->n_cv_in; (*leg)++)
        if (p->cv_in[*leg] == i)
            return LV2_ROLE_CV_IN;
    for (*leg = 0; *leg < p->n_cv_out; (*leg)++)
        if (p->cv_out[*leg] == i)
            return LV2_ROLE_CV_OUT;
    *leg = 0;
    if (p->latency_port >= 0 && (uint32_t)p->latency_port == i)
        return LV2_ROLE_LATENCY;
    if (*k < p->n_controls && p->controls[*k].port == i)
    {
        *leg = (*k)++;
        return LV2_ROLE_CONTROL;
    }
    return LV2_ROLE_NONE;
}

/* every port's role, by its index: what an instance connects it to */
static const char *read_roles(struct lv2_plugin *p)
{
    uint32_t i, k, leg, c;

    p->roles = calloc(p->n_ports ? p->n_ports : 1u, sizeof(*p->roles));
    if (!p->roles)
        return LV2_CODE_NO_REALISATION;
    for (i = 0, k = 0; i < p->n_ports; i++)
    {
        p->roles[i].role = port_role(p, i, &k, &leg, &c);
        p->roles[i].leg = leg;
        p->roles[i].channel = (uint16_t)c;
    }
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
    // state:threadSafeRestore, required or optional: the plugin's restore() may run beside run(), so a load while active
    // needs no restart
    for (k = 0; k < n; k++)
        p->safe_restore |= strcmp(p->required[k], LV2_STATE__threadSafeRestore) == 0;
    req = L.lilv_plugin_get_optional_features(pl);
    for (it = req ? L.lilv_nodes_begin(req) : NULL; req && !L.lilv_nodes_is_end(req, it); it = L.lilv_nodes_next(req, it))
        p->safe_restore |= strcmp(L.lilv_node_as_uri(L.lilv_nodes_get(req, it)), LV2_STATE__threadSafeRestore) == 0;
    if (req)
        L.lilv_nodes_free(req);
    for (k = 0; k < n && !refused && lv2_core_feature_options()->strict; k++)
    {
        // state:threadSafeRestore is the plugin's own declaration, not a feature to hand over
        if (strcmp(p->required[k], LV2_STATE__threadSafeRestore) == 0)
            continue;
        if (!lv2_feature_offered_within(lv2_core_feature_options(), lv2_features_configured(), p->required[k]))
            refused = LV2_CODE_FEATURES_MISSING;
    }
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
        free(p->controls[k].unit);
    }
    free(p->controls);
    for (k = 0; p->patches && k < p->n_patches; k++)
    {
        free(p->patches[k].uri);
        free(p->patches[k].symbol);
        free(p->patches[k].name);
    }
    free(p->patches);
    for (k = 0; p->texts && k < p->n_texts; k++)
    {
        free(p->texts[k].uri);
        free(p->texts[k].name);
    }
    free(p->texts);
    free(p->roles);
    free(p->main_in.name);
    free(p->main_out.name);
    for (i = 0; i < LV2_CORE_AUX_PORTS; i++)    // every slot: a refused open has named some ports, not set the counts
    {
        if (p->aux_in)
            free(p->aux_in[i].name);
        if (p->aux_out)
            free(p->aux_out[i].name);
    }
    free(p->aux_in);
    free(p->aux_out);
    for (i = 0; i < LV2_CORE_AUX_PORTS; i++)
    {
        free(p->cv_in_name[i]);
        free(p->cv_out_name[i]);
    }
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
    p->aux_in = calloc(LV2_CORE_AUX_PORTS, sizeof(*p->aux_in));
    p->aux_out = calloc(LV2_CORE_AUX_PORTS, sizeof(*p->aux_out));
    if (!p->uri || !p->bundle_path || !p->uri_node || !p->aux_in || !p->aux_out)
    {
        lv2_plugin_close(p);
        lv2_why_set(why, LV2_CODE_NO_REALISATION);
        return NULL;
    }
    // the plugin must be THIS bundle's: a URI another open bundle holds is not in this one
    pl = L.lilv_plugins_get_by_uri(L.lilv_world_get_all_plugins(g.world), p->uri_node);
    p->lilv_plugin = (void *)pl;
    refusal = !pl || !L.lilv_node_equals(L.lilv_plugin_get_bundle_uri(pl), bundle->node) ? LV2_CODE_NO_REALISATION : NULL;
    if (!refusal)
        refusal = read_ports(p, pl);
    if (!refusal)
        refusal = read_roles(p);
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

/* the process's table as a map, for a plugin that was handed none */
static LV2_URID table_map(LV2_URID_Map_Handle h, const char *uri)
{
    (void)h;
    return lv2_urid_lookup(uri);
}

/* state:loadDefaultState over lilv-state, through the plugin's own URID map, or the process's table, which is the same
 * mapping, when the feature options withheld it: 0, or -1 without a world. The default state is the plugin's state:state;
 * a plugin whose description has none is restored nothing, as under mod-host: lilv would make an empty state of it, and
 * a plugin that resets what a restore does not name (calf Wavetable's modulation matrix) would lose its own defaults */
int lv2_plugin_default_state(const struct lv2_plugin *p, LV2_Handle handle, const LV2_Feature *const *features)
{
    static LV2_URID_Map table = { NULL, table_map };
    LV2_URID_Map *map = &table;
    LilvInstance instance;
    LilvState *state;
    uint32_t i;

    for (i = 0; features && features[i]; i++)
        if (strcmp(features[i]->URI, LV2_URID__map) == 0)
            map = features[i]->data;
    if (!g.world)
        return -1;
    if (!L.lilv_world_ask(g.world, p->uri_node, g.node[N_STATE_STATE], NULL))
        return 0;
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

/*
************************************************************************************************************************
*           GLOBAL FUNCTIONS: THE STATE
************************************************************************************************************************
*/

/* what lilv reads and writes port values through: the plugin's input controls, by symbol, as floats */
struct state_io
{
    const struct lv2_plugin *p;
    float *controls;
    LV2_URID float_type;
};

/* the value lilv saves for a port: an input control's, the bypass and the outputs are not in the state */
static const void *state_get(const char *symbol, void *user, uint32_t *size, uint32_t *type)
{
    const struct state_io *io = user;
    uint32_t k;

    for (k = 0; k < io->p->n_controls; k++)
        if (io->p->controls[k].kind == LV2_CONTROL_INPUT && strcmp(io->p->controls[k].symbol, symbol) == 0)
        {
            *size = sizeof(float);
            *type = io->float_type;
            return &io->controls[k];
        }
    return NULL;
}

/* a port value the state carries, written to its input control: any other is ignored */
static void state_set(const char *symbol, void *user, const void *value, uint32_t size, uint32_t type)
{
    const struct state_io *io = user;
    uint32_t k;

    if (size != sizeof(float) || type != io->float_type)
        return;
    for (k = 0; k < io->p->n_controls; k++)
        if (io->p->controls[k].kind == LV2_CONTROL_INPUT && strcmp(io->p->controls[k].symbol, symbol) == 0)
            memcpy(&io->controls[k], value, sizeof(float));
}

char *lv2_plugin_state_text(const struct lv2_plugin *p, LV2_Handle handle, const float *controls,
                            const LV2_Feature *const *features, LV2_URID_Map *map, LV2_URID_Unmap *unmap,
                            const char *scratch_dir, const char *copy_dir)
{
    struct state_io io;
    LilvInstance instance;
    LilvState *state;
    char *text, *out;

    if (!p || !p->lilv_plugin || !g.world || !handle || !map || !unmap)
        return NULL;
    instance.lv2_descriptor = p->desc;
    instance.lv2_handle = handle;
    instance.pimpl = NULL;
    io.p = p;
    io.controls = (float *)controls;
    io.float_type = map->map(map->handle, LV2_ATOM__Float);
    // the files the plugin made under its directory are copied out of it, so the state names what it held at the save
    state = L.lilv_state_new_from_instance(p->lilv_plugin, &instance, map, scratch_dir, scratch_dir ? copy_dir : NULL, NULL,
                                           NULL, state_get, &io, 0, features);
    if (!state)
        return NULL;
    // lilv writes a state with a subject, and the state of this plugin is the plugin's own
    text = L.lilv_state_to_string(g.world, map, unmap, state, p->uri, NULL);
    L.lilv_state_free(state);
    if (!text)
        return NULL;
    out = strdup(text);
    L.lilv_free(text);
    return out;
}

int lv2_plugin_state_restore(const struct lv2_plugin *p, LV2_Handle handle, float *controls, const char *text,
                             const LV2_Feature *const *features, LV2_URID_Map *map)
{
    struct state_io io;
    LilvInstance instance;
    LilvState *state;

    if (!p || !g.world || !text || !map)
        return -1;
    state = L.lilv_state_new_from_string(g.world, map, text);
    if (!state)
        return -1;
    instance.lv2_descriptor = p->desc;
    instance.lv2_handle = handle;
    instance.pimpl = NULL;
    io.p = p;
    io.controls = controls;
    io.float_type = map->map(map->handle, LV2_ATOM__Float);
    L.lilv_state_restore(state, &instance, state_set, &io, 0, features);
    L.lilv_state_free(state);
    return 0;
}

char *lv2_plugin_preset_text(const struct lv2_plugin *p, const char *uri, LV2_URID_Map *map, LV2_URID_Unmap *unmap)
{
    static const char APPLIES_TO[] = "http://lv2plug.in/ns/lv2core#appliesTo";
    LilvNode *node, *applies;
    LilvNodes *nodes;
    LilvState *state;
    char *text, *out;
    int ours;

    if (!p || !p->uri_node || !g.world || !uri || !map || !unmap)
        return NULL;
    node = L.lilv_new_uri(g.world, uri);
    applies = L.lilv_new_uri(g.world, APPLIES_TO);
    if (!node || !applies)
    {
        if (node)
            L.lilv_node_free(node);
        if (applies)
            L.lilv_node_free(applies);
        return NULL;
    }
    // the preset must apply to THIS plugin: a preset of the bundle for another plugin is not this plugin's to load
    nodes = L.lilv_world_find_nodes(g.world, node, applies, NULL);
    ours = nodes && L.lilv_nodes_contains(nodes, p->uri_node);
    if (nodes)
        L.lilv_nodes_free(nodes);
    L.lilv_node_free(applies);
    if (!ours)
    {
        L.lilv_node_free(node);
        return NULL;
    }
    // lilv reads a preset's statements on demand: the resource is loaded for the state, then unloaded
    L.lilv_world_load_resource(g.world, node);
    state = L.lilv_state_new_from_world(g.world, map, node);
    L.lilv_world_unload_resource(g.world, node);
    L.lilv_node_free(node);
    if (!state)
        return NULL;
    text = L.lilv_state_to_string(g.world, map, unmap, state, uri, NULL);
    L.lilv_state_free(state);
    if (!text)
        return NULL;
    out = strdup(text);
    L.lilv_free(text);
    return out;
}

int lv2_plugin_state_check(const char *text, LV2_URID_Map *map)
{
    LilvState *state;

    if (!g.world || !text || !map)
        return -1;
    state = L.lilv_state_new_from_string(g.world, map, text);
    if (!state)
        return -1;
    L.lilv_state_free(state);
    return 0;
}

/* the presets of one plugin of the bundle: every pset:Preset that lv2:appliesTo it, in the world's order, with its
 * rdfs:label (the preset's resource is loaded for the label, as for its state, then unloaded; a preset with no label
 * is named by its URI) */
static int plugin_presets(const struct lv2_bundle *b, uint32_t index, lv2_preset_fn fn, void *ctx, int *stopped)
{
    LilvNode *plugin = L.lilv_new_uri(g.world, b->uris[index]);
    LilvNodes *presets;
    LilvIter *it;
    int listed = 0;

    if (!plugin)
        return 0;
    presets = L.lilv_world_find_nodes(g.world, NULL, g.node[N_APPLIES_TO], plugin);
    for (it = presets ? L.lilv_nodes_begin(presets) : NULL; it && !L.lilv_nodes_is_end(presets, it) && !*stopped;
         it = L.lilv_nodes_next(presets, it))
    {
        const LilvNode *preset = L.lilv_nodes_get(presets, it);
        LilvNodes *labels;
        char *label;

        if (!L.lilv_world_ask(g.world, preset, g.node[N_RDF_TYPE], g.node[N_PSET_PRESET]))
            continue;
        L.lilv_world_load_resource(g.world, preset);
        labels = L.lilv_world_find_nodes(g.world, preset, g.node[N_RDFS_LABEL], NULL);
        label = first_string(labels);
        if (labels)
            L.lilv_nodes_free(labels);
        L.lilv_world_unload_resource(g.world, preset);
        listed++;
        if (fn && fn(ctx, b->uris[index], L.lilv_node_as_uri(preset), label ? label : L.lilv_node_as_uri(preset)) != 0)
            *stopped = 1;
        free(label);
    }
    if (presets)
        L.lilv_nodes_free(presets);
    L.lilv_node_free(plugin);
    return listed;
}

int lv2_bundle_presets(const struct lv2_bundle *b, lv2_preset_fn fn, void *ctx)
{
    uint32_t i;
    int listed = 0, stopped = 0;

    if (!b || !g.world)
        return -1;
    for (i = 0; i < b->count && !stopped; i++)
        listed += plugin_presets(b, i, fn, ctx, &stopped);
    return listed;
}


/*
************************************************************************************************************************
*           GLOBAL FUNCTIONS: THE FIELD DUMP, FROM THE WORLD ALONE
************************************************************************************************************************
*/

/* a resource's label: its lv2:name, rdfs:label or foaf:name, the first it states; NULL when it states none */
static char *resource_label(const LilvNode *node)
{
    static const char *const by[] = { LV2_CORE__name, LV2_RDFS_LABEL, "http://xmlns.com/foaf/0.1/name" };
    char *label = NULL;
    uint32_t i;

    for (i = 0; i < sizeof(by) / sizeof(by[0]) && !label; i++)
    {
        LilvNode *predicate = L.lilv_new_uri(g.world, by[i]);
        LilvNodes *values = predicate ? L.lilv_world_find_nodes(g.world, node, predicate, NULL) : NULL;

        if (values && L.lilv_nodes_size(values) > 0)
            label = strdup(L.lilv_node_as_string(L.lilv_nodes_get(values, L.lilv_nodes_begin(values))));
        L.lilv_nodes_free(values);
        if (predicate)
            L.lilv_node_free(predicate);
    }
    return label;
}

/* a number literal as the dump writes it, %.17g */
static void number_text(const LilvNode *node, char *buf, size_t size)
{
    snprintf(buf, size, "%.17g", strtod(L.lilv_node_as_string(node), NULL));
}

/* one value of a field, by its kind; a resource that is no URI is passed by its label, and left out when it has none */
static void field_value(const struct lv2_field_ask *ask, int32_t port, uint32_t k, const LilvNode *v, lv2_field_fn fn, void *ctx)
{
    char number[64];

    if (L.lilv_node_is_uri(v) && !ask->by_label)
        fn(ctx, port, k, LV2_FIELD_URI, L.lilv_node_as_uri(v));
    else if (L.lilv_node_is_uri(v) || L.lilv_node_is_blank(v))
    {
        char *label = resource_label(v);

        if (label)
            fn(ctx, port, k, LV2_FIELD_TEXT, label);
        free(label);
    }
    else if (L.lilv_node_is_float(v) || L.lilv_node_is_int(v))
    {
        number_text(v, number, sizeof(number));
        fn(ctx, port, k, LV2_FIELD_NUMBER, number);
    }
    else
        fn(ctx, port, k, LV2_FIELD_TEXT, L.lilv_node_as_string(v));
}

/* a port's scale points, each "<value> <label>" */
static void scale_points(const LilvPlugin *pl, const LilvPort *port, int32_t index, uint32_t k, lv2_field_fn fn, void *ctx)
{
    LilvScalePoints *points = L.lilv_port_get_scale_points(pl, port);
    LilvIter *it;

    for (it = L.lilv_scale_points_begin(points); points && !L.lilv_scale_points_is_end(points, it);
         it = L.lilv_scale_points_next(points, it))
    {
        const LilvScalePoint *sp = L.lilv_scale_points_get(points, it);
        char number[64], *text;

        number_text(L.lilv_scale_point_get_value(sp), number, sizeof(number));
        if (asprintf(&text, "%s %s", number, L.lilv_node_as_string(L.lilv_scale_point_get_label(sp))) < 0)
            continue;
        fn(ctx, index, k, LV2_FIELD_TEXT, text);
        free(text);
    }
    L.lilv_scale_points_free(points);
}

int lv2_bundle_fields(struct lv2_bundle *bundle, const char *uri, const struct lv2_field_ask *asks, uint32_t count,
                      lv2_field_fn fn, void *ctx, char why[LV2_CORE_WHY_MAX])
{
    LilvNode *uri_node, **predicates;
    const LilvPlugin *pl;
    uint32_t k, i, ports;

    if (!bundle || !uri || !g.world || !fn)
    {
        lv2_why_set(why, LV2_CODE_NO_REALISATION);
        return -1;
    }
    uri_node = L.lilv_new_uri(g.world, uri);
    pl = uri_node ? L.lilv_plugins_get_by_uri(L.lilv_world_get_all_plugins(g.world), uri_node) : NULL;
    if (uri_node)
        L.lilv_node_free(uri_node);
    // the plugin must be THIS bundle's, as lv2_plugin_open has it
    if (!pl || !L.lilv_node_equals(L.lilv_plugin_get_bundle_uri(pl), bundle->node))
    {
        lv2_why_set(why, LV2_CODE_NO_REALISATION);
        return -1;
    }
    predicates = calloc(count ? count : 1, sizeof(*predicates));
    if (!predicates)
    {
        lv2_why_set(why, LV2_CODE_NO_REALISATION);
        return -1;
    }
    for (k = 0; k < count; k++)
        predicates[k] = L.lilv_new_uri(g.world, asks[k].uri);

    for (k = 0; k < count; k++)
    {
        LilvNodes *values = predicates[k] ? L.lilv_plugin_get_value(pl, predicates[k]) : NULL;
        LilvIter *it;

        for (it = L.lilv_nodes_begin(values); values && !L.lilv_nodes_is_end(values, it); it = L.lilv_nodes_next(values, it))
            field_value(&asks[k], -1, k, L.lilv_nodes_get(values, it), fn, ctx);
        L.lilv_nodes_free(values);
    }
    ports = L.lilv_plugin_get_num_ports(pl);
    for (i = 0; i < ports; i++)
    {
        const LilvPort *port = L.lilv_plugin_get_port_by_index(pl, i);

        for (k = 0; port && k < count; k++)
        {
            LilvNodes *values;
            LilvIter *it;

            if (strcmp(asks[k].uri, LV2_CORE__scalePoint) == 0)
            {
                scale_points(pl, port, (int32_t)i, k, fn, ctx);
                continue;
            }
            values = predicates[k] ? L.lilv_port_get_value(pl, port, predicates[k]) : NULL;
            for (it = L.lilv_nodes_begin(values); values && !L.lilv_nodes_is_end(values, it); it = L.lilv_nodes_next(values, it))
                field_value(&asks[k], (int32_t)i, k, L.lilv_nodes_get(values, it), fn, ctx);
            L.lilv_nodes_free(values);
        }
    }
    for (k = 0; k < count; k++)
        if (predicates[k])
            L.lilv_node_free(predicates[k]);
    free(predicates);
    lv2_why_set(why, "");
    return 0;
}
