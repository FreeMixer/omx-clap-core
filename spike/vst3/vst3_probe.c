// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// vst3_probe: the VST3 feasibility spike of omx-clap-core#287, NOT FOR MERGE.
//
//   vst3_probe <bundle.vst3> [class name]     (default class "Compressor Stereo")
//
// Plain C11 against Steinberg's generated C API (vst3_c_api.h, BSD-3, steinbergmedia/vst3_c_api 93a116f, "VST3 SDK
// 3.8.1"); no C++ and no SDK library. It dlopens the module, reads its factory, creates the component and its edit
// controller, moves the component state to the controller through a memory IBStream, lists the buses and the
// parameters, round-trips the state, and processes one block of silence and one of noise, printing peak and RMS.
// Everything runs on one thread: the threading rules of a real host are the report's subject, not the spike's.
#include <dlfcn.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vst3_c_api.h"

#define RATE 48000.0
#define BLOCK 512

typedef Steinberg_tresult tresult;
static bool same_iid(const void *a, const Steinberg_TUID b) { return memcmp(a, b, sizeof(Steinberg_TUID)) == 0; }

// A static COM object: the refcount is never zero and release never frees.
static Steinberg_uint32 static_ref(void *self) { (void)self; return 1; }

// ---- the host context: IHostApplication -------------------------------------------------------------------------
static tresult host_query(void *self, const Steinberg_TUID iid, void **obj)
{
    if (same_iid(iid, Steinberg_FUnknown_iid) || same_iid(iid, Steinberg_Vst_IHostApplication_iid)) {
        *obj = self;
        return Steinberg_kResultOk;
    }
    *obj = NULL;
    return Steinberg_kNoInterface;
}
static tresult host_name(void *self, Steinberg_Vst_String128 name)
{
    (void)self;
    const char *s = "omx vst3_probe";
    size_t i = 0;
    for (; s[i] && i < 127; i++) name[i] = (Steinberg_Vst_TChar)s[i];
    name[i] = 0;
    return Steinberg_kResultOk;
}
// ---- IMessage + IAttributeList: the host must allocate the messages the two halves exchange --------------------
// One heap object carries both interfaces and one refcount. The attribute store is a small array (a spike).
enum { ATTR_INT, ATTR_FLOAT, ATTR_STRING, ATTR_BINARY };
typedef struct {
    char key[64];
    int type;
    int64_t i;
    double f;
    void *data; // a string (UTF-16, with its terminator) or a binary blob
    uint32_t size;
} attr;
typedef struct {
    Steinberg_Vst_IMessage msg;
    Steinberg_Vst_IAttributeList list;
    int refs;
    char id[128];
    attr a[32];
    int n;
} message;
#define MSG_OF_LIST(p) ((message *)((char *)(p) - offsetof(message, list)))

static int messages_made, messages_live;
static Steinberg_uint32 msg_add(void *self) { return (Steinberg_uint32)++((message *)self)->refs; }
static Steinberg_uint32 msg_release(void *self)
{
    message *m = self;
    int r = --m->refs;
    if (r == 0) {
        for (int k = 0; k < m->n; k++) free(m->a[k].data);
        free(m);
        messages_live--;
    }
    return (Steinberg_uint32)r;
}
static tresult msg_query(void *self, const Steinberg_TUID iid, void **obj)
{
    message *m = self;
    if (same_iid(iid, Steinberg_FUnknown_iid) || same_iid(iid, Steinberg_Vst_IMessage_iid)) *obj = &m->msg;
    else if (same_iid(iid, Steinberg_Vst_IAttributeList_iid)) *obj = &m->list;
    else {
        *obj = NULL;
        return Steinberg_kNoInterface;
    }
    m->refs++;
    return Steinberg_kResultOk;
}
static Steinberg_FIDString msg_get_id(void *self) { return ((message *)self)->id; }
static void msg_set_id(void *self, Steinberg_FIDString id)
{
    snprintf(((message *)self)->id, sizeof ((message *)self)->id, "%s", id ? id : "");
}
static struct Steinberg_Vst_IAttributeList *msg_attrs(void *self) { return &((message *)self)->list; }
static Steinberg_Vst_IMessageVtbl msg_vt = {msg_query, msg_add, msg_release, msg_get_id, msg_set_id, msg_attrs};

static tresult al_query(void *self, const Steinberg_TUID iid, void **obj) { return msg_query(MSG_OF_LIST(self), iid, obj); }
static Steinberg_uint32 al_add(void *self) { return msg_add(MSG_OF_LIST(self)); }
static Steinberg_uint32 al_release(void *self) { return msg_release(MSG_OF_LIST(self)); }
static attr *al_slot(void *self, const char *key, int type, bool make)
{
    message *m = MSG_OF_LIST(self);
    for (int k = 0; k < m->n; k++)
        if (!strcmp(m->a[k].key, key)) {
            if (!make && m->a[k].type != type) return NULL;
            if (make) {
                free(m->a[k].data);
                m->a[k].data = NULL;
                m->a[k].type = type;
            }
            return &m->a[k];
        }
    if (!make || m->n == 32) return NULL;
    attr *a = &m->a[m->n++];
    memset(a, 0, sizeof *a);
    snprintf(a->key, sizeof a->key, "%s", key);
    a->type = type;
    return a;
}
static tresult al_set_int(void *self, const char *id, Steinberg_int64 v)
{
    attr *a = al_slot(self, id, ATTR_INT, true);
    return a ? (a->i = v, Steinberg_kResultOk) : Steinberg_kResultFalse;
}
static tresult al_get_int(void *self, const char *id, Steinberg_int64 *v)
{
    attr *a = al_slot(self, id, ATTR_INT, false);
    return a ? (*v = a->i, Steinberg_kResultOk) : Steinberg_kResultFalse;
}
static tresult al_set_float(void *self, const char *id, double v)
{
    attr *a = al_slot(self, id, ATTR_FLOAT, true);
    return a ? (a->f = v, Steinberg_kResultOk) : Steinberg_kResultFalse;
}
static tresult al_get_float(void *self, const char *id, double *v)
{
    attr *a = al_slot(self, id, ATTR_FLOAT, false);
    return a ? (*v = a->f, Steinberg_kResultOk) : Steinberg_kResultFalse;
}
static tresult al_set_binary(void *self, const char *id, const void *d, Steinberg_uint32 n)
{
    attr *a = al_slot(self, id, ATTR_BINARY, true);
    if (!a || !(a->data = malloc(n ? n : 1))) return Steinberg_kResultFalse;
    memcpy(a->data, d, n);
    a->size = n;
    return Steinberg_kResultOk;
}
static tresult al_get_binary(void *self, const char *id, const void **d, Steinberg_uint32 *n)
{
    attr *a = al_slot(self, id, ATTR_BINARY, false);
    return a ? (*d = a->data, *n = a->size, Steinberg_kResultOk) : Steinberg_kResultFalse;
}
static tresult al_set_string(void *self, const char *id, const Steinberg_Vst_TChar *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    tresult r = al_set_binary(self, id, s, (n + 1) * sizeof *s);
    if (r == Steinberg_kResultOk) al_slot(self, id, ATTR_BINARY, false)->type = ATTR_STRING;
    return r;
}
static tresult al_get_string(void *self, const char *id, Steinberg_Vst_TChar *s, Steinberg_uint32 bytes)
{
    attr *a = al_slot(self, id, ATTR_STRING, false);
    if (!a || bytes < sizeof *s) return Steinberg_kResultFalse;
    uint32_t n = a->size < bytes ? a->size : bytes;
    memcpy(s, a->data, n);
    s[n / sizeof *s - 1] = 0;
    return Steinberg_kResultOk;
}
static Steinberg_Vst_IAttributeListVtbl al_vt = {al_query,     al_add,       al_release,    al_set_int,
                                                 al_get_int,   al_set_float, al_get_float,  al_set_string,
                                                 al_get_string, al_set_binary, al_get_binary};

static int host_refused; // a createInstance for anything but IMessage/IAttributeList: counted, refused
static tresult host_create(void *self, Steinberg_TUID cid, Steinberg_TUID iid, void **obj)
{
    (void)self;
    *obj = NULL;
    if (same_iid(cid, Steinberg_Vst_IMessage_iid) && same_iid(iid, Steinberg_Vst_IMessage_iid)) {
        message *m = calloc(1, sizeof *m);
        if (!m) return Steinberg_kResultFalse;
        m->msg.lpVtbl = &msg_vt;
        m->list.lpVtbl = &al_vt;
        m->refs = 1;
        messages_made++;
        messages_live++;
        *obj = &m->msg;
        return Steinberg_kResultOk;
    }
    host_refused++;
    return Steinberg_kNotImplemented;
}
static Steinberg_Vst_IHostApplicationVtbl host_vt = {host_query, static_ref, static_ref, host_name, host_create};
static Steinberg_Vst_IHostApplication host = {&host_vt};

// ---- a growable memory IBStream ---------------------------------------------------------------------------------
typedef struct {
    Steinberg_IBStream com;
    unsigned char *data;
    int64_t size, pos, cap;
} memstream;
static tresult ms_query(void *self, const Steinberg_TUID iid, void **obj)
{
    if (same_iid(iid, Steinberg_FUnknown_iid) || same_iid(iid, Steinberg_IBStream_iid)) {
        *obj = self;
        return Steinberg_kResultOk;
    }
    *obj = NULL;
    return Steinberg_kNoInterface;
}
static tresult ms_read(void *self, void *buf, Steinberg_int32 n, Steinberg_int32 *got)
{
    memstream *m = self;
    int64_t k = m->size - m->pos < n ? m->size - m->pos : n;
    if (k < 0) k = 0;
    memcpy(buf, m->data + m->pos, (size_t)k);
    m->pos += k;
    if (got) *got = (Steinberg_int32)k;
    return Steinberg_kResultOk;
}
static tresult ms_write(void *self, void *buf, Steinberg_int32 n, Steinberg_int32 *put)
{
    memstream *m = self;
    if (m->pos + n > m->cap) {
        int64_t cap = (m->pos + n) * 2;
        unsigned char *d = realloc(m->data, (size_t)cap);
        if (!d) return Steinberg_kResultFalse;
        m->data = d;
        m->cap = cap;
    }
    memcpy(m->data + m->pos, buf, (size_t)n);
    m->pos += n;
    if (m->pos > m->size) m->size = m->pos;
    if (put) *put = n;
    return Steinberg_kResultOk;
}
static tresult ms_seek(void *self, Steinberg_int64 pos, Steinberg_int32 mode, Steinberg_int64 *res)
{
    memstream *m = self;
    int64_t base = mode == Steinberg_IBStream_IStreamSeekMode_kIBSeekSet ? 0
                 : mode == Steinberg_IBStream_IStreamSeekMode_kIBSeekCur ? m->pos : m->size;
    if (base + pos < 0 || base + pos > m->size) return Steinberg_kInvalidArgument;
    m->pos = base + pos;
    if (res) *res = m->pos;
    return Steinberg_kResultOk;
}
static tresult ms_tell(void *self, Steinberg_int64 *pos)
{
    *pos = ((memstream *)self)->pos;
    return Steinberg_kResultOk;
}
static Steinberg_IBStreamVtbl ms_vt = {ms_query, static_ref, static_ref, ms_read, ms_write, ms_seek, ms_tell};

// ---- empty parameter changes and event list for process() -------------------------------------------------------
static tresult pc_query(void *self, const Steinberg_TUID iid, void **obj)
{
    if (same_iid(iid, Steinberg_FUnknown_iid) || same_iid(iid, Steinberg_Vst_IParameterChanges_iid)) {
        *obj = self;
        return Steinberg_kResultOk;
    }
    *obj = NULL;
    return Steinberg_kNoInterface;
}
static Steinberg_int32 pc_count(void *self) { (void)self; return 0; }
static struct Steinberg_Vst_IParamValueQueue *pc_get(void *self, Steinberg_int32 i) { (void)self; (void)i; return NULL; }
static struct Steinberg_Vst_IParamValueQueue *pc_add(void *self, const Steinberg_Vst_ParamID *id, Steinberg_int32 *i)
{
    (void)self; (void)id; (void)i;
    return NULL;
}
static Steinberg_Vst_IParameterChangesVtbl pc_vt = {pc_query, static_ref, static_ref, pc_count, pc_get, pc_add};
static Steinberg_Vst_IParameterChanges pc_in = {&pc_vt}, pc_out = {&pc_vt};

static tresult ev_query(void *self, const Steinberg_TUID iid, void **obj)
{
    if (same_iid(iid, Steinberg_FUnknown_iid) || same_iid(iid, Steinberg_Vst_IEventList_iid)) {
        *obj = self;
        return Steinberg_kResultOk;
    }
    *obj = NULL;
    return Steinberg_kNoInterface;
}
static Steinberg_int32 ev_count(void *self) { (void)self; return 0; }
static tresult ev_get(void *self, Steinberg_int32 i, struct Steinberg_Vst_Event *e)
{
    (void)self; (void)i; (void)e;
    return Steinberg_kInvalidArgument;
}
static tresult ev_add(void *self, struct Steinberg_Vst_Event *e) { (void)self; (void)e; return Steinberg_kResultFalse; }
static Steinberg_Vst_IEventListVtbl ev_vt = {ev_query, static_ref, static_ref, ev_count, ev_get, ev_add};
static Steinberg_Vst_IEventList ev_in = {&ev_vt}, ev_out = {&ev_vt};

// ---- helpers ------------------------------------------------------------------------------------------------------
static const char *ascii(const Steinberg_Vst_TChar *s)
{
    static char out[4][129];
    static int k;
    char *o = out[k++ & 3];
    int i = 0;
    for (; s[i] && i < 128; i++) o[i] = s[i] < 128 ? (char)s[i] : '?';
    o[i] = 0;
    return o;
}
#define CALL(obj, m, ...) ((obj)->lpVtbl->m((obj), ##__VA_ARGS__))
#define CHECK(what, r)                                                                                               \
    do {                                                                                                             \
        tresult r_ = (r);                                                                                            \
        printf("%-34s %s (%d)\n", what, r_ == Steinberg_kResultOk ? "ok" : "FAILED", (int)r_);                       \
        if (r_ != Steinberg_kResultOk) return 1;                                                                     \
    } while (0)

static void level(const char *what, float **ch, int nch, int n)
{
    for (int c = 0; c < nch; c++) {
        double pk = 0, ss = 0;
        for (int i = 0; i < n; i++) {
            double v = fabs(ch[c][i]);
            pk = v > pk ? v : pk;
            ss += v * v;
        }
        double rms = sqrt(ss / n);
        printf("  %-6s ch%d peak %8.2f dBFS  rms %8.2f dBFS\n", what, c, pk > 0 ? 20 * log10(pk) : -INFINITY,
               rms > 0 ? 20 * log10(rms) : -INFINITY);
    }
}

static void list_buses(Steinberg_Vst_IComponent *comp)
{
    static const char *media[] = {"audio", "event"}, *dir[] = {"in", "out"};
    for (int m = 0; m < 2; m++)
        for (int d = 0; d < 2; d++) {
            Steinberg_int32 n = CALL(comp, getBusCount, m, d);
            for (Steinberg_int32 i = 0; i < n; i++) {
                struct Steinberg_Vst_BusInfo b;
                if (CALL(comp, getBusInfo, m, d, i, &b) != Steinberg_kResultOk) continue;
                printf("  bus %s %s %d: \"%s\" channels %d type %s flags 0x%x\n", media[m], dir[d], (int)i,
                       ascii(b.name), (int)b.channelCount, b.busType == Steinberg_Vst_BusTypes_kMain ? "main" : "aux",
                       (unsigned)b.flags);
            }
        }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: vst3_probe <bundle.vst3> [class name]\n");
        return 2;
    }
    const char *want = argc > 2 ? argv[2] : "Compressor Stereo";
    char so[4096];
    const char *base = strrchr(argv[1], '/') ? strrchr(argv[1], '/') + 1 : argv[1];
    snprintf(so, sizeof so, "%s/Contents/x86_64-linux/%.*s.so", argv[1], (int)(strlen(base) - 5), base);

    // 1. the module: dlopen, ModuleEntry (Linux), GetPluginFactory
    void *lib = dlopen(so, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    bool (*module_entry)(void *) = (bool (*)(void *))dlsym(lib, "ModuleEntry");
    bool (*module_exit)(void) = (bool (*)(void))dlsym(lib, "ModuleExit");
    Steinberg_IPluginFactory *(*get_factory)(void) = (Steinberg_IPluginFactory * (*)(void)) dlsym(lib, "GetPluginFactory");
    if (!module_entry || !module_exit || !get_factory) {
        fprintf(stderr, "missing ModuleEntry/ModuleExit/GetPluginFactory\n");
        return 1;
    }
    printf("%-34s %s\n", "ModuleEntry", module_entry(lib) ? "ok" : "FAILED");
    Steinberg_IPluginFactory *fac = get_factory();
    struct Steinberg_PFactoryInfo fi;
    CHECK("IPluginFactory::getFactoryInfo", CALL(fac, getFactoryInfo, &fi));
    Steinberg_int32 nclass = CALL(fac, countClasses);
    printf("factory vendor \"%s\", %d classes\n", fi.vendor, (int)nclass);

    // 2. the class: an "Audio Module Class" by name
    struct Steinberg_PClassInfo ci, found;
    int naudio = 0, nctrl = 0, hit = 0;
    for (Steinberg_int32 i = 0; i < nclass; i++) {
        if (CALL(fac, getClassInfo, i, &ci) != Steinberg_kResultOk) continue;
        if (!strcmp(ci.category, "Audio Module Class")) {
            naudio++;
            if (!hit && !strcmp(ci.name, want)) found = ci, hit = 1;
        } else if (!strcmp(ci.category, "Component Controller Class")) nctrl++;
    }
    printf("  %d audio module classes, %d controller classes\n", naudio, nctrl);
    if (!hit) {
        fprintf(stderr, "no audio module class \"%s\"\n", want);
        return 1;
    }
    printf("class \"%s\"\n", found.name);

    // 3. the component and its controller
    Steinberg_Vst_IComponent *comp = NULL;
    CHECK("createInstance(IComponent)", CALL(fac, createInstance, (const char *)found.cid,
                                             (const char *)Steinberg_Vst_IComponent_iid, (void **)&comp));
    CHECK("IComponent::initialize", CALL(comp, initialize, (struct Steinberg_FUnknown *)&host));
    Steinberg_TUID ctrl_cid;
    Steinberg_Vst_IEditController *ctrl = NULL;
    bool separate = CALL(comp, getControllerClassId, ctrl_cid) == Steinberg_kResultOk;
    if (separate)
        CHECK("createInstance(IEditController)", CALL(fac, createInstance, (const char *)ctrl_cid,
                                                      (const char *)Steinberg_Vst_IEditController_iid, (void **)&ctrl));
    else
        CHECK("query IEditController (single)",
              CALL(comp, queryInterface, Steinberg_Vst_IEditController_iid, (void **)&ctrl));
    if (separate) CHECK("IEditController::initialize", CALL(ctrl, initialize, (struct Steinberg_FUnknown *)&host));

    // the two halves talk through IConnectionPoint (no proxy here: one thread)
    Steinberg_Vst_IConnectionPoint *cp_comp = NULL, *cp_ctrl = NULL;
    if (CALL(comp, queryInterface, Steinberg_Vst_IConnectionPoint_iid, (void **)&cp_comp) == Steinberg_kResultOk &&
        CALL(ctrl, queryInterface, Steinberg_Vst_IConnectionPoint_iid, (void **)&cp_ctrl) == Steinberg_kResultOk) {
        CHECK("IConnectionPoint::connect comp", CALL(cp_comp, connect, cp_ctrl));
        CHECK("IConnectionPoint::connect ctrl", CALL(cp_ctrl, connect, cp_comp));
    }

    // 4. state: component -> controller (the round trip comes after setupProcessing, below)
    memstream st = {{&ms_vt}, NULL, 0, 0, 0};
    CHECK("IComponent::getState", CALL(comp, getState, &st.com));
    printf("  component state %lld bytes\n", (long long)st.size);
    st.pos = 0;
    CHECK("IEditController::setComponentState", CALL(ctrl, setComponentState, &st.com));

    // 5. buses and parameters
    list_buses(comp);
    Steinberg_int32 np = CALL(ctrl, getParameterCount);
    int ro = 0, bypass = 0;
    for (Steinberg_int32 i = 0; i < np; i++) {
        struct Steinberg_Vst_ParameterInfo pi;
        if (CALL(ctrl, getParameterInfo, i, &pi) != Steinberg_kResultOk) continue;
        ro += !!(pi.flags & Steinberg_Vst_ParameterInfo_ParameterFlags_kIsReadOnly);
        bypass += !!(pi.flags & Steinberg_Vst_ParameterInfo_ParameterFlags_kIsBypass);
        if (i < 8) {
            Steinberg_Vst_String128 txt = {0};
            CALL(ctrl, getParamStringByValue, pi.id, pi.defaultNormalizedValue, txt);
            printf("  param %3d id %10u \"%s\" default %.4f (\"%s\" %s) steps %d flags 0x%x\n", (int)i,
                   (unsigned)pi.id, ascii(pi.title), pi.defaultNormalizedValue, ascii(txt), ascii(pi.units),
                   (int)pi.stepCount, (unsigned)pi.flags);
        }
    }
    printf("  %d parameters (%d read-only, %d bypass)\n", (int)np, ro, bypass);

    // 6. processing: stereo in/out, 32-bit, realtime, 48 kHz, 512 frames
    Steinberg_Vst_IAudioProcessor *proc = NULL;
    CHECK("query IAudioProcessor", CALL(comp, queryInterface, Steinberg_Vst_IAudioProcessor_iid, (void **)&proc));
    Steinberg_Vst_SpeakerArrangement in_arr = Steinberg_Vst_SpeakerArr_kStereo, out_arr = Steinberg_Vst_SpeakerArr_kStereo;
    printf("%-34s %d\n", "setBusArrangements(stereo)", (int)CALL(proc, setBusArrangements, &in_arr, 1, &out_arr, 1));
    CHECK("canProcessSampleSize(32)", CALL(proc, canProcessSampleSize, Steinberg_Vst_SymbolicSampleSizes_kSample32));
    struct Steinberg_Vst_ProcessSetup setup = {Steinberg_Vst_ProcessModes_kRealtime,
                                               Steinberg_Vst_SymbolicSampleSizes_kSample32, BLOCK, RATE};
    CHECK("setupProcessing(48000, 512)", CALL(proc, setupProcessing, &setup));
    // the state round trip waits for setupProcessing: LSP 1.2.29 divides by its rate inside IComponent::setState
    // (SIGFPE at lsp-plugins.so+0x3b55e6 when the rate is still 0), so a host restores only after setupProcessing
    st.pos = 0;
    CHECK("IComponent::setState (restore)", CALL(comp, setState, &st.com));
    memstream st2 = {{&ms_vt}, NULL, 0, 0, 0};
    CHECK("IComponent::getState (again)", CALL(comp, getState, &st2.com));
    printf("  state round trip: %lld bytes, %s\n", (long long)st2.size,
           st2.size == st.size && !memcmp(st.data, st2.data, (size_t)st.size) ? "identical" : "DIFFERENT");

    CHECK("activateBus(audio in 0)", CALL(comp, activateBus, 0, 0, 0, 1));
    CHECK("activateBus(audio out 0)", CALL(comp, activateBus, 0, 1, 0, 1));
    CHECK("IComponent::setActive(1)", CALL(comp, setActive, 1));
    printf("  latency %u samples, tail %u samples\n", (unsigned)CALL(proc, getLatencySamples),
           (unsigned)CALL(proc, getTailSamples));
    tresult sp = CALL(proc, setProcessing, 1); // kNotImplemented is allowed by the spec
    printf("%-34s %d\n", "setProcessing(1)", (int)sp);

    static float inL[BLOCK], inR[BLOCK], outL[BLOCK], outR[BLOCK];
    float *ins[2] = {inL, inR}, *outs[2] = {outL, outR};
    struct Steinberg_Vst_AudioBusBuffers ib = {2, 0, {.Steinberg_Vst_AudioBusBuffers_channelBuffers32 = ins}};
    struct Steinberg_Vst_AudioBusBuffers ob = {2, 0, {.Steinberg_Vst_AudioBusBuffers_channelBuffers32 = outs}};
    struct Steinberg_Vst_ProcessData pd = {Steinberg_Vst_ProcessModes_kRealtime,
                                           Steinberg_Vst_SymbolicSampleSizes_kSample32,
                                           BLOCK, 1, 1, &ib, &ob, &pc_in, &pc_out, &ev_in, &ev_out, NULL};

    memset(outL, 0x7f, sizeof outL); // garbage, so a plugin that never writes shows
    memset(outR, 0x7f, sizeof outR);
    ib.silenceFlags = 3;
    CHECK("process(silence)", CALL(proc, process, &pd));
    level("out", outs, 2, BLOCK);
    printf("  output silenceFlags 0x%llx\n", (unsigned long long)ob.silenceFlags);

    uint32_t seed = 1;
    for (int b = 0; b < 8; b++) { // eight blocks of white noise at -6 dBFS peak, the last one measured
        for (int i = 0; i < BLOCK; i++) {
            seed = seed * 1664525u + 1013904223u;
            inL[i] = ((float)(seed >> 8) / 8388608.0f - 1.0f) * 0.5f;
            seed = seed * 1664525u + 1013904223u;
            inR[i] = ((float)(seed >> 8) / 8388608.0f - 1.0f) * 0.5f;
        }
        ib.silenceFlags = 0;
        if (CALL(proc, process, &pd) != Steinberg_kResultOk) {
            printf("process(noise) block %d FAILED\n", b);
            return 1;
        }
    }
    printf("%-34s ok (8 blocks)\n", "process(noise)");
    level("in", ins, 2, BLOCK);
    level("out", outs, 2, BLOCK);

    // 7. teardown in the reverse order
    CALL(proc, setProcessing, 0);
    CALL(comp, setActive, 0);
    if (cp_comp && cp_ctrl) {
        CALL(cp_comp, disconnect, cp_ctrl);
        CALL(cp_ctrl, disconnect, cp_comp);
        CALL(cp_comp, release);
        CALL(cp_ctrl, release);
    }
    CALL(proc, release);
    if (separate) {
        CALL(ctrl, terminate);
        CALL(ctrl, release);
    } else CALL(ctrl, release);
    CALL(comp, terminate);
    CALL(comp, release);
    CALL(fac, release);
    printf("%-34s %s\n", "ModuleExit", module_exit() ? "ok" : "FAILED");
    printf("host IMessage objects made %d, still alive %d; other createInstance requests refused %d\n", messages_made,
           messages_live, host_refused);
    free(st.data);
    free(st2.data);
    dlclose(lib);
    return 0;
}
