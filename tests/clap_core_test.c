// SPDX-License-Identifier: GPL-3.0-or-later
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
 * clap_core_test.c: the core over real `.clap` binaries (the fault fixtures, tests/fault_clap.c) and fakes this file
 * owns, no jack.
 *
 *   ./tests/clap_core_test <dir with fault-<n>.clap>
 *
 *  1. THE WITNESS: a link-time wrap is blind to a `.clap`'s own malloc, the negative control that keeps every "0
 *     allocations" here honest about what it measures.
 *  2. THE GUARD PAGE: a plugin that writes past its block dies of SIGSEGV in a forked child; the clean one exits 0.
 *  3. THE AUDIO ROLE ON A SPLIT: a plugin that asks is_audio_thread inside process(), one instance per lane, the lanes
 *     run by this thread and two workers that a predicate given to omx_clap_host_publish_role names; every answer on a
 *     thread that ran a lane is true, a thread outside reads false, and every thread reads false once unpublished.
 *  4. THE ROSTER is whole or refused: 257 parameters give 257 rows, a hole below count() refuses with
 *     clap.param-row-unreadable and no rows, and param_count and param_row refuse it too; the same plugin without the
 *     hole gives 4.
 *  5. THE TEMPO: omx_clap_host_run hands the plugin no transport with no tempo given or none published, a transport
 *     carrying the bpm and only HAS_TEMPO once one is, a change at the next block, none once withdrawn; the tempo
 *     fixture (fault mode 16) reads 120 bpm as a level of 0.12.
 *     The transport is written only inside a processing block: a block run on an idle stage leaves the instance's
 *     record untouched, and the warm-up after it is handed none.
 *  6. THE RESET: a re-engage after a steady bypass resets the plugin once, on the control thread holding the audio
 *     role; the RT thread running the instance never calls reset().
 */
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <math.h>

#include "../src/clap_host.h"

static int failures = 0;
#define CHECK(cond, ...)                                   \
  do {                                                     \
    if (!(cond)) {                                         \
      failures++;                                          \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
      fprintf(stderr, __VA_ARGS__);                        \
      fprintf(stderr, "\n");                               \
    }                                                      \
  } while (0)

/* ---- RT allocation witness ---- */
static volatile int in_rt = 0, rt_allocs = 0;
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);
void *__wrap_malloc(size_t n) { rt_allocs += in_rt; return __real_malloc(n); }
void *__wrap_calloc(size_t a, size_t b) { rt_allocs += in_rt; return __real_calloc(a, b); }
void *__wrap_realloc(void *p, size_t n) { rt_allocs += in_rt; return __real_realloc(p, n); }
void __wrap_free(void *p) { rt_allocs += in_rt; __real_free(p); }

static const char *g_fault_dir;

static void fault_path(char *out, size_t cap, int mode) { snprintf(out, cap, "%s/fault-%d.clap", g_fault_dir, mode); }
/* ---- 1. the witness ---- */
static void t_witness(void) {
  char path[512], why[OMX_CLAP_WHY_MAX];
  fault_path(path, sizeof path, 10);
  struct omx_clap_instance *in = NULL;
  CHECK(omx_clap_host_open(path, NULL, &in, why) == 0 && omx_clap_host_activate(in, 48000.0, 256, why) == 0, "the allocating plugin opens and activates (%s)", why);
  if (!in) return;
  omx_clap_host_publish(in, pthread_self());
  float l[256] = {0}, r[256] = {0};
  const int before = rt_allocs;
  in_rt = 1;
  omx_clap_host_run(in, l, r, 256);
  in_rt = 0;
  CHECK(rt_allocs == before, "the link-time wrap is BLIND to a .clap's own malloc (%d): an interposer is the measurement", rt_allocs - before);
  omx_clap_host_unpublish(in, 10, 1000);
  omx_clap_host_close(in);
}

/* ---- 2. the guard page ---- */
/** In a child: host `mode`, run one block. The parent reads how the child ended. */
static int child_status(int mode) {
  char path[512], why[OMX_CLAP_WHY_MAX];
  fault_path(path, sizeof path, mode);
  const pid_t pid = fork();
  if (pid == 0) {
    /* a planted fault is the arm's proof, not a crash to keep: the child is not dumpable, so the
     * kernel writes no core for it (the status still reads SIGSEGV) */
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    struct omx_clap_instance *in = NULL;
    if (omx_clap_host_open(path, NULL, &in, why) != 0 || omx_clap_host_activate(in, 48000.0, 64, why) != 0) _exit(3);
    omx_clap_host_publish(in, pthread_self());
    float l[64] = {0}, r[64] = {0};
    omx_clap_host_run(in, l, r, 64);
    _exit(0);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  return status;
}

static void t_guard_page(void) {
  const int bad = child_status(9);
  CHECK(WIFSIGNALED(bad) && WTERMSIG(bad) == SIGSEGV, "a write past the block hits the guard page and faults there (status %d)", bad);
  /* The fault is the proof, not a crash to keep: no core is written for it */
  CHECK(!WCOREDUMP(bad), "the planted fault leaves no core dump behind (status %d)", bad);
  const int good = child_status(0);
  CHECK(WIFEXITED(good) && WEXITSTATUS(good) == 0, "the clean plugin in the same harness exits 0 (status %d)", good);
}

/* ---- 3. the audio role on a split ---- */

/* The answer the asking plugin last read on THIS thread, from inside its own process(); -1 none. */
static _Thread_local int g_asked = -1;

typedef struct {
  const clap_host_t *host;
  const clap_host_thread_check_t *tc;
} Asker;

static bool ask_init(const clap_plugin_t *p) {
  Asker *a = (Asker *)p->plugin_data;
  a->tc = (const clap_host_thread_check_t *)a->host->get_extension(a->host, CLAP_EXT_THREAD_CHECK);
  return a->tc != NULL;
}
static void ask_destroy(const clap_plugin_t *p) {
  free(p->plugin_data);
  free((void *)p);
}
static bool ask_activate(const clap_plugin_t *p, double sr, uint32_t a, uint32_t b) { (void)p, (void)sr, (void)a, (void)b; return true; }
static void ask_nop(const clap_plugin_t *p) { (void)p; }
static bool ask_start(const clap_plugin_t *p) { (void)p; return true; }
static clap_process_status ask_process(const clap_plugin_t *p, const clap_process_t *pr) {
  const Asker *a = (const Asker *)p->plugin_data;
  g_asked = a->tc->is_audio_thread(a->host) ? 1 : 0; /* the question, on the thread that processes */
  const clap_audio_buffer_t *ai = &pr->audio_inputs[0], *ao = &pr->audio_outputs[0];
  for (uint32_t c = 0; c < ao->channel_count; c++) memcpy(ao->data32[c], ai->data32[c], pr->frames_count * sizeof(float));
  return CLAP_PROCESS_CONTINUE;
}
static uint32_t ask_ports_count(const clap_plugin_t *p, bool is_input) { (void)p, (void)is_input; return 1; }
static bool ask_ports_get(const clap_plugin_t *p, uint32_t i, bool is_input, clap_audio_port_info_t *info) {
  (void)p, (void)is_input;
  if (i != 0) return false;
  memset(info, 0, sizeof *info);
  snprintf(info->name, sizeof info->name, "main");
  info->flags = CLAP_AUDIO_PORT_IS_MAIN;
  info->channel_count = 2;
  info->port_type = CLAP_PORT_STEREO;
  info->in_place_pair = CLAP_INVALID_ID;
  return true;
}
static const clap_plugin_audio_ports_t ASK_PORTS = {ask_ports_count, ask_ports_get};
static const void *ask_ext(const clap_plugin_t *p, const char *id) {
  (void)p;
  return strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0 ? &ASK_PORTS : NULL;
}
static const char *const ASK_FEATURES[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL};
static const clap_plugin_descriptor_t ASK_DESC = {CLAP_VERSION_INIT, "org.omx-clap-host.test.thread-asker", "thread asker", "omx-clap-host", "", "", "", "0", "",
                                                  ASK_FEATURES};
static uint32_t ask_count(const clap_plugin_factory_t *f) { (void)f; return 1; }
static const clap_plugin_descriptor_t *ask_desc(const clap_plugin_factory_t *f, uint32_t i) { (void)f; return i == 0 ? &ASK_DESC : NULL; }
static const clap_plugin_t *ask_create(const clap_plugin_factory_t *f, const clap_host_t *host, const char *id) {
  (void)f;
  if (strcmp(id, ASK_DESC.id) != 0) return NULL;
  Asker *a = calloc(1, sizeof *a);
  a->host = host;
  clap_plugin_t *p = calloc(1, sizeof *p);
  *p = (clap_plugin_t){&ASK_DESC, a, ask_init, ask_destroy, ask_activate, ask_nop, ask_start, ask_nop, ask_nop, ask_process, ask_ext, ask_nop};
  return p;
}
static const clap_plugin_factory_t ASK_FACTORY = {ask_count, ask_desc, ask_create};
static bool ask_entry_init(const char *path) { (void)path; return true; }
static void ask_entry_deinit(void) {}
static const void *ask_factory(const char *id) { return strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &ASK_FACTORY : NULL; }
static const clap_plugin_entry_t ASK_ENTRY = {CLAP_VERSION_INIT, ask_entry_init, ask_entry_deinit, ask_factory};

#define ROLE_LANES 6u
#define ROLE_W 3u
#define ROLE_BLOCK 64u
#define ROLE_CYCLES 200u

/* The split: this thread the driver (worker 0) and two workers, each recording its own thread id in its slot while its
 * loop runs. The predicate is what an engine hands omx_clap_host_publish_role: a live worker holds the role. */
struct role_split {
  pthread_t tid[ROLE_W];
  _Atomic int live[ROLE_W];
  _Atomic uint32_t cycle;    /* the driver bumps it; each worker runs the lanes of its share once per cycle */
  _Atomic uint32_t done[ROLE_W];
  _Atomic int quit;
  struct omx_clap_instance *in[ROLE_LANES];
  float buf[ROLE_LANES][2][ROLE_BLOCK];
  _Atomic uint32_t runs[ROLE_W], heard[ROLE_W], unasked;
};
static int split_role(void *ctx, pthread_t self) {
  struct role_split *S = ctx;
  for (uint32_t w = 1; w < ROLE_W; w++)
    if (atomic_load_explicit(&S->live[w], memory_order_acquire) && pthread_equal(S->tid[w], self)) return 1;
  return 0;
}
static void role_lane(struct role_split *S, uint32_t lane, uint32_t w) {
  g_asked = -1;
  omx_clap_host_run(S->in[lane], S->buf[lane][0], S->buf[lane][1], ROLE_BLOCK);
  if (g_asked < 0) atomic_fetch_add(&S->unasked, 1u);
  atomic_fetch_add(&S->runs[w], 1u);
  if (g_asked == 1) atomic_fetch_add(&S->heard[w], 1u);
}
struct worker_arg { struct role_split *S; uint32_t w; };
static void *role_worker(void *arg) {
  struct worker_arg *a = arg;
  struct role_split *S = a->S;
  const uint32_t w = a->w;
  S->tid[w] = pthread_self();
  atomic_store_explicit(&S->live[w], 1, memory_order_release);
  uint32_t seen = 0;
  while (!atomic_load(&S->quit)) {
    const uint32_t c = atomic_load_explicit(&S->cycle, memory_order_acquire);
    if (c == seen) { sched_yield(); continue; }
    seen = c;
    for (uint32_t l = w; l < ROLE_LANES; l += ROLE_W) role_lane(S, l, w);
    atomic_store_explicit(&S->done[w], c, memory_order_release);
  }
  atomic_store_explicit(&S->live[w], 0, memory_order_release);
  return NULL;
}
static void role_cycle(struct role_split *S) {
  const uint32_t c = atomic_fetch_add(&S->cycle, 1u) + 1u;
  for (uint32_t l = 0; l < ROLE_LANES; l += ROLE_W) role_lane(S, l, 0);
  for (uint32_t w = 1; w < ROLE_W; w++)
    while (atomic_load_explicit(&S->done[w], memory_order_acquire) != c) sched_yield();
}

static void *outsider_main(void *arg) {
  struct omx_clap_instance *in = arg;
  const clap_host_thread_check_t *tc = in->host.get_extension(&in->host, CLAP_EXT_THREAD_CHECK);
  return (void *)(uintptr_t)(tc->is_audio_thread(&in->host) ? 1u : 0u);
}
static int outsider_asks(struct omx_clap_instance *in) {
  pthread_t th;
  void *r = NULL;
  pthread_create(&th, NULL, outsider_main, in);
  pthread_join(th, &r);
  return (int)(uintptr_t)r;
}

static void t_audio_role_on_split(void) {
  static struct role_split S;
  char why[OMX_CLAP_WHY_MAX];
  for (uint32_t l = 0; l < ROLE_LANES; l++) {
    CHECK(omx_clap_host_open_entry(&ASK_ENTRY, ASK_DESC.id, &S.in[l], why) == 0, "open the asker for lane %u: %s", l, why);
    if (!S.in[l]) return;
    g_asked = -1;
    CHECK(omx_clap_host_activate(S.in[l], 48000.0, ROLE_BLOCK, why) == 0, "activate lane %u: %s", l, why);
    CHECK(g_asked == 1, "the warm-up on the control thread holds the audio role (read %d)", g_asked);
  }
  pthread_t th[ROLE_W];
  struct worker_arg args[ROLE_W];
  for (uint32_t w = 1; w < ROLE_W; w++) {
    args[w] = (struct worker_arg){&S, w};
    pthread_create(&th[w], NULL, role_worker, &args[w]);
    while (!atomic_load(&S.live[w])) sched_yield();
  }
  for (uint32_t l = 0; l < ROLE_LANES; l++) omx_clap_host_publish_role(S.in[l], pthread_self(), split_role, &S);
  CHECK(outsider_asks(S.in[0]) == 0, "a thread outside the split is not the audio thread");
  for (uint32_t c = 0; c < ROLE_CYCLES; c++) role_cycle(&S);
  printf("clap_core: audio role W=%u: driver %u/%u true, worker 1 %u/%u, worker 2 %u/%u, unasked %u\n", ROLE_W,
         atomic_load(&S.heard[0]), atomic_load(&S.runs[0]), atomic_load(&S.heard[1]), atomic_load(&S.runs[1]),
         atomic_load(&S.heard[2]), atomic_load(&S.runs[2]), atomic_load(&S.unasked));
  CHECK(atomic_load(&S.runs[1]) > 0 && atomic_load(&S.runs[2]) > 0, "both workers ran lanes");
  CHECK(atomic_load(&S.unasked) == 0, "every lane's plugin asked on its process thread (%u did not)", atomic_load(&S.unasked));
  for (uint32_t w = 0; w < ROLE_W; w++)
    CHECK(atomic_load(&S.heard[w]) == atomic_load(&S.runs[w]), "thread %u: the plugin read true on every lane it ran (%u of %u)", w,
          atomic_load(&S.heard[w]), atomic_load(&S.runs[w]));
  CHECK(outsider_asks(S.in[0]) == 0, "after the cycles, a thread outside the split still reads false");
  for (uint32_t l = 0; l < ROLE_LANES; l++) omx_clap_request_stop(&S.in[l]->stage);
  role_cycle(&S);
  for (uint32_t l = 0; l < ROLE_LANES; l++) CHECK(omx_clap_host_unpublish(S.in[l], 100, 2000000) == 0, "unpublish lane %u", l);
  CHECK(outsider_asks(S.in[0]) == 0, "unpublished: an outsider reads false");
  atomic_store(&S.quit, 1);
  for (uint32_t w = 1; w < ROLE_W; w++) pthread_join(th[w], NULL);
  for (uint32_t l = 0; l < ROLE_LANES; l++) {
    omx_clap_host_deactivate(S.in[l]);
    omx_clap_host_close(S.in[l]);
  }
}

/* ---- 4. the roster is whole or refused ---- */

/* The roster plugin: the asker's body with a `params` extension of `g_roster_n` parameters, and
 * `get_info` refusing index `g_roster_hole` (UINT32_MAX: none) — a plugin that declares a count
 * it will not answer. */
static uint32_t g_roster_n, g_roster_hole = UINT32_MAX;
static uint32_t roster_count(const clap_plugin_t *p) { (void)p; return g_roster_n; }
static bool roster_info(const clap_plugin_t *p, uint32_t i, clap_param_info_t *info) {
  (void)p;
  if (i >= g_roster_n || i == g_roster_hole) return false;
  memset(info, 0, sizeof *info);
  info->id = 1000u + i;
  snprintf(info->name, sizeof info->name, "p%u", i);
  info->min_value = 0.0;
  info->max_value = 1.0;
  info->default_value = 0.5;
  return true;
}
static bool roster_value(const clap_plugin_t *p, clap_id id, double *v) { (void)p, (void)id; *v = 0.5; return true; }
static bool roster_to_text(const clap_plugin_t *p, clap_id id, double v, char *t, uint32_t n) {
  (void)p, (void)id;
  snprintf(t, n, "%g", v);
  return true;
}
static bool roster_from_text(const clap_plugin_t *p, clap_id id, const char *t, double *v) { (void)p, (void)id; *v = atof(t); return true; }
static void roster_flush(const clap_plugin_t *p, const clap_input_events_t *in, const clap_output_events_t *out) { (void)p, (void)in, (void)out; }
static const clap_plugin_params_t ROSTER_PARAMS = {roster_count, roster_info, roster_value, roster_to_text, roster_from_text, roster_flush};
static const void *roster_ext(const clap_plugin_t *p, const char *id) {
  if (strcmp(id, CLAP_EXT_PARAMS) == 0) return &ROSTER_PARAMS;
  return ask_ext(p, id);
}
static const clap_plugin_descriptor_t ROSTER_DESC = {CLAP_VERSION_INIT, "org.omx-clap-host.test.roster", "roster", "omx-clap-host", "", "", "", "0", "",
                                                     ASK_FEATURES};
static const clap_plugin_descriptor_t *roster_desc(const clap_plugin_factory_t *f, uint32_t i) { (void)f; return i == 0 ? &ROSTER_DESC : NULL; }
static const clap_plugin_t *roster_create(const clap_plugin_factory_t *f, const clap_host_t *host, const char *id) {
  (void)f;
  if (strcmp(id, ROSTER_DESC.id) != 0) return NULL;
  Asker *a = calloc(1, sizeof *a);
  a->host = host;
  clap_plugin_t *p = calloc(1, sizeof *p);
  *p = (clap_plugin_t){&ROSTER_DESC, a, ask_init, ask_destroy, ask_activate, ask_nop, ask_start, ask_nop, ask_nop, ask_process, roster_ext, ask_nop};
  return p;
}
static const clap_plugin_factory_t ROSTER_FACTORY = {ask_count, roster_desc, roster_create};
static const void *roster_factory(const char *id) { return strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &ROSTER_FACTORY : NULL; }
static const clap_plugin_entry_t ROSTER_ENTRY = {CLAP_VERSION_INIT, ask_entry_init, ask_entry_deinit, roster_factory};

/* Open the roster plugin with `n` parameters and `hole`, read its roster: the count (or -1), the
 * refusal token, and the last row's id. */
static int roster_read(uint32_t n, uint32_t hole, const char **why, uint32_t *last_id) {
  g_roster_n = n;
  g_roster_hole = hole;
  char open_why[OMX_CLAP_WHY_MAX];
  struct omx_clap_instance *in = NULL;
  if (omx_clap_host_open_entry(&ROSTER_ENTRY, ROSTER_DESC.id, &in, open_why) != 0 || !in) {
    CHECK(0, "open the roster plugin (%u params): %s", n, open_why);
    return -2;
  }
  struct omx_clap_param_row *rows = NULL;
  *why = NULL;
  const int count = omx_clap_host_param_roster(in, &rows, why);
  struct omx_clap_param_row one;
  const uint32_t n_rows = omx_clap_host_param_count(in);
  const int row0 = omx_clap_host_param_row(in, 0, &one);
  if (count < 0) CHECK(n_rows == 0 && row0 == -1, "a refused roster counts 0 rows (%u) and refuses row 0 (%d)", n_rows, row0);
  else CHECK(n_rows == (uint32_t)count && row0 == 0, "a whole roster counts its %d rows (%u) and serves row 0", count, n_rows);
  *last_id = count > 0 && rows ? rows[count - 1].id : 0u;
  free(rows);
  omx_clap_host_close(in);
  return count;
}

static void t_roster_whole_or_refused(void) {
  const char *why = NULL;
  uint32_t last = 0;
  int got = roster_read(257u, UINT32_MAX, &why, &last);
  printf("clap_core: roster of 257 params -> %d rows, last id %u, refusal %s\n", got, last, why ? why : "none");
  CHECK(got == 257 && why == NULL, "a 257-parameter plugin serves all 257 rows, no cap (got %d, refusal %s)", got,
        why ? why : "none");
  CHECK(last == 1256u, "the 257th row is the plugin's last parameter (id %u, want 1256)", last);
  got = roster_read(4u, 2u, &why, &last);
  printf("clap_core: roster of 4 params, index 2 unreadable -> %d, refusal %s\n", got, why ? why : "none");
  CHECK(got == -1, "an index below count() that get_info will not answer refuses the read (got %d rows)", got);
  CHECK(why && strcmp(why, OMX_CLAP_PARAM_ROW_UNREADABLE) == 0, "the refusal is coded %s (got %s)", OMX_CLAP_PARAM_ROW_UNREADABLE,
        why ? why : "none");
  got = roster_read(4u, UINT32_MAX, &why, &last);
  CHECK(got == 4 && why == NULL, "positive control: the same plugin with no hole serves 4 rows (got %d)", got);
}

/* ---- 5. the tempo ---- */

/* The tempo fake: a stereo pass-through that records the transport its last process() was given. */
static struct {
  int saw_transport;   /* 1: a transport was passed, 0: NULL */
  double saw_tempo;    /* its tempo when it carried CLAP_TRANSPORT_HAS_TEMPO, else 0 */
  uint32_t saw_tflags;
  int saw_theader_ok;  /* size, space, type and time 0 as CLAP fixes them */
  uint32_t runs;
} g_tp;
static clap_process_status tp_process(const clap_plugin_t *p, const clap_process_t *pr) {
  (void)p;
  const clap_event_transport_t *t = pr->transport;
  g_tp.runs++;
  g_tp.saw_transport = t != NULL;
  g_tp.saw_tempo = t && (t->flags & CLAP_TRANSPORT_HAS_TEMPO) ? t->tempo : 0.0;
  g_tp.saw_tflags = t ? t->flags : 0u;
  g_tp.saw_theader_ok = t && t->header.size == sizeof *t && t->header.space_id == CLAP_CORE_EVENT_SPACE_ID &&
                        t->header.type == CLAP_EVENT_TRANSPORT && t->header.time == 0;
  const clap_audio_buffer_t *ai = &pr->audio_inputs[0], *ao = &pr->audio_outputs[0];
  for (uint32_t c = 0; c < ao->channel_count; c++) memcpy(ao->data32[c], ai->data32[c], pr->frames_count * sizeof(float));
  return CLAP_PROCESS_CONTINUE;
}
static const clap_plugin_descriptor_t TP_DESC = {CLAP_VERSION_INIT, "org.omx-clap-host.test.transport", "transport reader", "omx-clap-host", "", "", "", "0", "",
                                                 ASK_FEATURES};
static bool tp_init(const clap_plugin_t *p) { (void)p; return true; }
static void tp_destroy(const clap_plugin_t *p) { free((void *)p); }
static const clap_plugin_descriptor_t *tp_desc(const clap_plugin_factory_t *f, uint32_t i) { (void)f; return i == 0 ? &TP_DESC : NULL; }
static const clap_plugin_t *tp_create(const clap_plugin_factory_t *f, const clap_host_t *host, const char *id) {
  (void)f, (void)host;
  if (strcmp(id, TP_DESC.id) != 0) return NULL;
  clap_plugin_t *p = calloc(1, sizeof *p);
  *p = (clap_plugin_t){&TP_DESC, NULL, tp_init, tp_destroy, ask_activate, ask_nop, ask_start, ask_nop, ask_nop, tp_process, ask_ext, ask_nop};
  return p;
}
static const clap_plugin_factory_t TP_FACTORY = {ask_count, tp_desc, tp_create};
static const void *tp_factory(const char *id) { return strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &TP_FACTORY : NULL; }
static const clap_plugin_entry_t TP_ENTRY = {CLAP_VERSION_INIT, ask_entry_init, ask_entry_deinit, tp_factory};

static void tp_run(struct omx_clap_instance *in) {
  float l[64], r[64];
  for (int i = 0; i < 64; i++) l[i] = r[i] = 0.1f;
  in_rt = 1;
  omx_clap_host_run(in, l, r, 64);
  in_rt = 0;
}

static void t_tempo(void) {
  char why[OMX_CLAP_WHY_MAX];
  struct omx_clap_instance *in = NULL;
  CHECK(omx_clap_host_open_entry(&TP_ENTRY, TP_DESC.id, &in, why) == 0 && omx_clap_host_activate(in, 48000.0, 64, why) == 0, "the transport reader opens (%s)", why);
  if (!in) return;
  omx_clap_host_publish(in, pthread_self());
  const uint32_t runs0 = atomic_load(&in->stage.h.runs), calls0 = g_tp.runs; /* past the warm-up's own calls */
  tp_run(in);
  CHECK(!g_tp.saw_transport, "an instance given no tempo passes transport NULL");
  struct omx_clap_tempo word;
  memset(&word, 0, sizeof word);
  omx_clap_host_set_tempo(in, &word);
  tp_run(in);
  CHECK(!g_tp.saw_transport, "a word holding no tempo passes transport NULL, never an invented one");
  omx_clap_tempo_publish(&word, 120.0);
  tp_run(in);
  CHECK(g_tp.saw_transport, "a published tempo reaches the plugin as a transport");
  CHECK(g_tp.saw_tempo == 120.0, "the plugin reads the bpm: 120 (%g)", g_tp.saw_tempo);
  CHECK(g_tp.saw_tflags == CLAP_TRANSPORT_HAS_TEMPO, "the flags are HAS_TEMPO and nothing else (0x%x)", g_tp.saw_tflags);
  CHECK(g_tp.saw_theader_ok, "the transport's header is CLAP_EVENT_TRANSPORT, its size, time 0");
  omx_clap_tempo_publish(&word, 93.5);
  tp_run(in);
  CHECK(g_tp.saw_tempo == 93.5, "a bpm change arrives at the next block: 93.5 (%g)", g_tp.saw_tempo);
  omx_clap_tempo_publish(&word, NAN);
  tp_run(in);
  CHECK(!g_tp.saw_transport, "a non-finite bpm withdraws the tempo: transport NULL");
  CHECK(omx_clap_tempo_read(&word) == 0.0, "and the word reads 0");
  CHECK(atomic_load(&in->stage.h.runs) - runs0 == 5 && g_tp.runs - calls0 == 5, "every block ran the plugin (%u)", g_tp.runs - calls0);
  omx_clap_request_stop(&in->stage);
  tp_run(in);
  CHECK(omx_clap_host_unpublish(in, 10, 1000) == 0, "unpublished");
  omx_clap_host_close(in);

  /* the fixture that reports its tempo as a level */
  char path[512];
  fault_path(path, sizeof path, 16);
  in = NULL;
  CHECK(omx_clap_host_open(path, NULL, &in, why) == 0 && omx_clap_host_activate(in, 48000.0, 256, why) == 0, "the tempo fixture opens (%s)", why);
  if (!in) return;
  omx_clap_host_set_tempo(in, &word);
  omx_clap_host_publish(in, pthread_self());
  float l[256], r[256];
  float level = -1.0f;
  for (int b = 0; b < 4; b++) { /* past the fade-in */
    for (int i = 0; i < 256; i++) l[i] = r[i] = 1.0f;
    omx_clap_tempo_publish(&word, 120.0);
    omx_clap_host_run(in, l, r, 256);
    level = l[255];
  }
  CHECK(fabsf(level - 0.12f) < 1e-6f, "120 bpm reaches the fixture: level %g, want 0.12", (double)level);
  omx_clap_tempo_publish(&word, 0.0);
  for (int i = 0; i < 256; i++) l[i] = r[i] = 1.0f;
  omx_clap_host_run(in, l, r, 256);
  CHECK(l[255] == 0.0f, "withdrawn: the fixture is silent (%g)", (double)l[255]);
  omx_clap_request_stop(&in->stage);
  omx_clap_host_run(in, l, r, 256);
  CHECK(omx_clap_host_unpublish(in, 10, 1000) == 0, "unpublished");
  omx_clap_host_close(in);
}

/* The transport is the processing block's: an idle stage's block writes nothing of the instance, so a warm-up that
 * follows it is handed no transport. */
static void t_transport_in_the_cycle(void) {
  char why[OMX_CLAP_WHY_MAX];
  struct omx_clap_instance *in = NULL;
  CHECK(omx_clap_host_open_entry(&TP_ENTRY, TP_DESC.id, &in, why) == 0 && omx_clap_host_activate(in, 48000.0, 64, why) == 0, "the transport reader opens again (%s)", why);
  if (!in) return;
  struct omx_clap_tempo word;
  memset(&word, 0, sizeof word);
  omx_clap_tempo_publish(&word, 120.0);
  omx_clap_host_set_tempo(in, &word);
  clap_event_transport_t before;
  memcpy(&before, &in->transport, sizeof before);
  const uint32_t calls0 = g_tp.runs;
  tp_run(in); /* not published: the stage is idle and the block passes dry */
  CHECK(g_tp.runs == calls0, "an idle stage's block never reaches the plugin");
  CHECK(memcmp(&before, &in->transport, sizeof before) == 0, "and leaves the instance's transport record untouched");
  CHECK(in->stage.proc.transport == NULL, "and hands the stage no transport");
  CHECK(omx_clap_prime(&in->stage, 64) == 0, "a warm-up after it runs clean");
  CHECK(g_tp.runs > calls0 && !g_tp.saw_transport, "and the warm-up's process() is handed no transport");
  omx_clap_host_publish(in, pthread_self());
  tp_run(in);
  CHECK(g_tp.saw_transport && g_tp.saw_tempo == 120.0, "published, the block carries the tempo (%g)", g_tp.saw_tempo);
  CHECK(in->stage.proc.transport == NULL, "and withdraws it when the block returns");
  omx_clap_request_stop(&in->stage);
  tp_run(in);
  CHECK(omx_clap_host_unpublish(in, 10, 1000) == 0, "unpublished");
  omx_clap_host_close(in);
}

/* ---- 6. the reset is the control thread's ---- */

/* The reset fake: a -20 dB pad that counts its reset() calls by the thread that made them. */
static pthread_t g_rs_rt;
static _Atomic uint32_t g_rs_on_rt, g_rs_off_rt, g_rs_process;
static void rs_reset(const clap_plugin_t *p) {
  (void)p;
  if (pthread_equal(pthread_self(), g_rs_rt)) atomic_fetch_add(&g_rs_on_rt, 1u);
  else atomic_fetch_add(&g_rs_off_rt, 1u);
}
static clap_process_status rs_process(const clap_plugin_t *p, const clap_process_t *pr) {
  (void)p;
  atomic_fetch_add(&g_rs_process, 1u);
  const clap_audio_buffer_t *ai = &pr->audio_inputs[0], *ao = &pr->audio_outputs[0];
  for (uint32_t c = 0; c < ao->channel_count; c++)
    for (uint32_t i = 0; i < pr->frames_count; i++) ao->data32[c][i] = 0.1f * ai->data32[c][i];
  return CLAP_PROCESS_CONTINUE;
}
static const clap_plugin_descriptor_t RS_DESC = {CLAP_VERSION_INIT, "org.omx-clap-host.test.reset-counter", "reset counter", "omx-clap-host", "", "", "", "0", "",
                                                 ASK_FEATURES};
static const clap_plugin_descriptor_t *rs_desc(const clap_plugin_factory_t *f, uint32_t i) { (void)f; return i == 0 ? &RS_DESC : NULL; }
static const clap_plugin_t *rs_create(const clap_plugin_factory_t *f, const clap_host_t *host, const char *id) {
  (void)f, (void)host;
  if (strcmp(id, RS_DESC.id) != 0) return NULL;
  clap_plugin_t *p = calloc(1, sizeof *p);
  *p = (clap_plugin_t){&RS_DESC, NULL, tp_init, tp_destroy, ask_activate, ask_nop, ask_start, ask_nop, rs_reset, rs_process, ask_ext, ask_nop};
  return p;
}
static const clap_plugin_factory_t RS_FACTORY = {ask_count, rs_desc, rs_create};
static const void *rs_factory(const char *id) { return strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &RS_FACTORY : NULL; }
static const clap_plugin_entry_t RS_ENTRY = {CLAP_VERSION_INIT, ask_entry_init, ask_entry_deinit, rs_factory};

static struct {
  struct omx_clap_instance *in;
  _Atomic int go, quit;
  _Atomic uint32_t blocks;
  float last;
} g_rs;
static void *rs_rt(void *arg) {
  (void)arg;
  float l[64], r[64];
  while (!atomic_load(&g_rs.go)) sched_yield();
  while (!atomic_load(&g_rs.quit)) {
    for (int i = 0; i < 64; i++) l[i] = r[i] = 0.5f;
    omx_clap_host_run(g_rs.in, l, r, 64);
    g_rs.last = l[32];
    atomic_fetch_add(&g_rs.blocks, 1u);
    usleep(200);
  }
  return NULL;
}
static void rs_wait(uint32_t n) {
  const uint32_t to = atomic_load(&g_rs.blocks) + n;
  while (atomic_load(&g_rs.blocks) < to) usleep(100);
}

static void t_reset_off_rt(void) {
  char why[OMX_CLAP_WHY_MAX];
  CHECK(omx_clap_host_open_entry(&RS_ENTRY, RS_DESC.id, &g_rs.in, why) == 0 && omx_clap_host_activate(g_rs.in, 48000.0, 64, why) == 0, "the reset counter opens (%s)", why);
  if (!g_rs.in) return;
  pthread_t rt;
  pthread_create(&rt, NULL, rs_rt, NULL);
  g_rs_rt = rt;
  const uint32_t off0 = atomic_load(&g_rs_off_rt); /* the warm-up's own reset, on this thread */
  omx_clap_host_publish(g_rs.in, rt);
  atomic_store(&g_rs.go, 1);
  rs_wait(20);
  omx_clap_host_bypass(g_rs.in, 1);
  rs_wait(20); /* the crossfade, then a steady bypass: the plugin idles */
  const uint32_t idle_from = atomic_load(&g_rs_process);
  rs_wait(10);
  CHECK(atomic_load(&g_rs_process) == idle_from, "the steady bypass never calls process()");
  CHECK(atomic_load(&g_rs_off_rt) == off0 && atomic_load(&g_rs_on_rt) == 0, "no reset while bypassed");
  omx_clap_host_bypass(g_rs.in, 0);
  CHECK(atomic_load(&g_rs_off_rt) - off0 == 1, "the re-engage reset the plugin once, on the control thread (%u)", atomic_load(&g_rs_off_rt) - off0);
  CHECK(atomic_load(&g_rs.in->stage.resets) == 1, "the stage counted it (%u)", atomic_load(&g_rs.in->stage.resets));
  rs_wait(40);
  CHECK(atomic_load(&g_rs_on_rt) == 0, "the RT thread called reset() %u times across %u blocks", atomic_load(&g_rs_on_rt), atomic_load(&g_rs.blocks));
  CHECK(atomic_load(&g_rs_off_rt) - off0 == 1, "and nobody reset it again");
  CHECK(atomic_load(&g_rs_process) > idle_from && g_rs.last == 0.05f, "processing again after the re-engage (%g)", (double)g_rs.last);
  omx_clap_host_bypass(g_rs.in, 0);
  CHECK(atomic_load(&g_rs_off_rt) - off0 == 1, "an off that was already off resets nothing");
  omx_clap_request_stop(&g_rs.in->stage);
  rs_wait(2);
  atomic_store(&g_rs.quit, 1);
  pthread_join(rt, NULL);
  CHECK(omx_clap_host_unpublish(g_rs.in, 10, 1000) == 0, "unpublished");
  omx_clap_host_close(g_rs.in);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <fault dir>\n", argv[0]);
    return 2;
  }
  g_fault_dir = argv[1];
  t_witness();
  t_guard_page();
  t_audio_role_on_split();
  t_roster_whole_or_refused();
  t_tempo();
  t_transport_in_the_cycle();
  t_reset_off_rt();
  CHECK(rt_allocs == 0, "no allocation inside omx_clap_host_run across the run (%d)", rt_allocs);
  if (failures) {
    fprintf(stderr, "clap_core: %d failure(s)\n", failures);
    return 1;
  }
  printf("clap_core: all passed (rt allocations: %d)\n", rt_allocs);
  return 0;
}
