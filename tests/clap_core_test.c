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
 *     hole gives 4. A plugin claiming 10000000 parameters is served OMX_CLAP_PARAM_COUNT_MAX rows.
 *  5. THE TEMPO: omx_clap_host_run hands the plugin no transport with no tempo given or none published, a transport
 *     carrying the bpm and only HAS_TEMPO once one is, a change at the next block, none once withdrawn; the tempo
 *     fixture (fault mode 16) reads 120 bpm as a level of 0.12.
 *     The transport is written only inside a processing block: a block run on an idle stage leaves the instance's
 *     record untouched, and the warm-up after it is handed none.
 *  6. THE RESET: a re-engage after a steady bypass resets the plugin once, on the control thread holding the audio
 *     role; the RT thread running the instance never calls reset(). An off that was already off holds nothing: every
 *     block stays the plugin's output.
 *  7. THE ABI: configure accepts a consumer of this header (ABI 16, the oldest until the first release of
 *     libomx-clap-core.so.1) and refuses 0, ABI 13 (before the instance's surround and ambisonic extensions), ABI 12
 *     (before the stage's wide bounce), the last header of libomx-clap-core.so.0 (ABI 11) and a newer one.
 *  8. THE SIDE CHAIN: a plugin with a stereo auxiliary input is admitted, reads silence until the caller binds a pair,
 *     reads the pair while it runs, reads silence again once unbound, keeps the binding over a deactivate and activate;
 *     binding while it runs, a bad port or a stereo port with one channel is refused; one auxiliary input beyond the
 *     core's count, one of 65 channels or one of none is refused; one of three channels is admitted.
 *  9. THE RESCAN AND THE WARM-UP: the warm-up's own deactivate raises no report; a rescan made in activate, or one
 *     made before it, is reported after it; request_layout's read of the description answers one.
 * 10. THE NOTE OUTPUT: an effect with a note output that pushes 300 note ons, a note expression and a NOTE_END on every
 *     block: its note output port is read at open, the warm-up's 64 blocks put nothing in the ring, and each live block
 *     of omx_clap_host_run keeps 256 for omx_clap_host_notes_out at the block's frames, counts 44 dropped and the
 *     expression unmapped, and never forwards the NOTE_END, with no allocation on either side under the witness.
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
#include "../src/clap_meters.h"
#include "../src/hosted_stage_limits.h"

/* hosted_stage_limits.h and clap_host_limits.h are two renders of HOSTED_STAGE_LIMITS (data/hosting.json): every
 * number the core reads has one value under both names, so a host reading OMX_HOSTED_STAGE_* reads the core's own */
_Static_assert(OMX_HOSTED_STAGE_WARMUP_BLOCKS == CLAP_HOST_WARMUP_BLOCKS, "warm-up blocks");
_Static_assert(OMX_HOSTED_STAGE_WARMUP_LEVEL_DBFS == CLAP_HOST_WARMUP_LEVEL_DBFS, "warm-up level");
_Static_assert(OMX_HOSTED_STAGE_NON_FINITE_STRIKES == CLAP_HOST_NON_FINITE_STRIKES, "strikes");
_Static_assert(OMX_HOSTED_STAGE_CLAMP_DBFS == CLAP_HOST_CLAMP_DBFS, "clamp");
_Static_assert(OMX_HOSTED_STAGE_PARAM_QUEUE_DEPTH == CLAP_HOST_PARAM_QUEUE_DEPTH, "queue depth");
_Static_assert(OMX_HOSTED_STAGE_EVENTS_PER_BLOCK == CLAP_HOST_EVENTS_PER_BLOCK, "events per block");
_Static_assert(OMX_HOSTED_STAGE_UNPUBLISH_POLL_US == CLAP_HOST_UNPUBLISH_POLL_US, "unpublish poll");
_Static_assert(OMX_HOSTED_STAGE_UNPUBLISH_TIMEOUT_US == CLAP_HOST_UNPUBLISH_TIMEOUT_US, "unpublish timeout");
_Static_assert(OMX_HOSTED_STAGE_NOTES_PER_BLOCK == CLAP_HOST_NOTES_PER_BLOCK, "notes per block");
_Static_assert(OMX_HOSTED_STAGE_PORT_CHANNELS == CLAP_HOST_PORT_CHANNELS, "port width");
_Static_assert(OMX_HOSTED_STAGE_AUX_OUTPUTS == CLAP_HOST_AUX_OUTPUTS, "auxiliary outputs");
_Static_assert(OMX_HOSTED_STAGE_AUX_INPUTS == CLAP_HOST_AUX_INPUTS, "auxiliary inputs");
_Static_assert(OMX_HOSTED_STAGE_STATE_MAX_BYTES == CLAP_HOST_STATE_MAX_BYTES, "state bytes");
_Static_assert(OMX_HOSTED_STAGE_LOG_BYTES == CLAP_HOST_LOG_BYTES, "log bytes");
_Static_assert(OMX_HOSTED_STAGE_ROLE_POLL_US == CLAP_HOST_ROLE_POLL_US, "role poll");
_Static_assert(OMX_HOSTED_STAGE_ROLE_TIMEOUT_US == CLAP_HOST_ROLE_TIMEOUT_US, "role timeout");
_Static_assert(OMX_HOSTED_STAGE_PARAM_COUNT_MAX == CLAP_HOST_PARAM_COUNT_MAX, "parameter count cap");
_Static_assert(OMX_HOSTED_STAGE_LATENCY_HOLD_MS == CLAP_HOST_LATENCY_HOLD_MS, "latency hold");

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
/* ---- 7. the ABI ---- */
static void t_abi(void) {
  struct omx_clap_host_config config;
  omx_clap_host_config_default(&config);
  CHECK(config.abi == 16u && OMX_CLAP_CORE_ABI_OLDEST == 16u, "this header is ABI 16, the oldest accepted 16 (%u)", config.abi);
  /* ABI 6 appends the note ids to the instance: every field an older header reaches keeps its offset */
  CHECK(offsetof(struct omx_clap_instance, note_ids) >= offsetof(struct omx_clap_instance, contract_why) + sizeof ((struct omx_clap_instance *)0)->contract_why,
        "the note ids follow every field of ABI 5 (%zu)", offsetof(struct omx_clap_instance, note_ids));
  /* ABI 7 appends the note output after them */
  CHECK(offsetof(struct omx_clap_instance, note_outputs) >= offsetof(struct omx_clap_instance, note_ids) + sizeof(struct omx_clap_note_ids) &&
            offsetof(struct omx_clap_instance, note_out) > offsetof(struct omx_clap_instance, note_outputs),
        "the note output follows the note ids (%zu)", offsetof(struct omx_clap_instance, note_out));
  /* ABI 8 appends the voice info after the note output */
  CHECK(offsetof(struct omx_clap_instance, voice_info_read) >= offsetof(struct omx_clap_instance, note_out) + sizeof(struct omx_clap_note_out) &&
            offsetof(struct omx_clap_instance, voice_flags) > offsetof(struct omx_clap_instance, voice_capacity) &&
            offsetof(struct omx_clap_instance, voice_capacity) > offsetof(struct omx_clap_instance, voice_count) &&
            offsetof(struct omx_clap_instance, voice_count) > offsetof(struct omx_clap_instance, voice_info_read),
        "the voice info follows the note output (%zu)", offsetof(struct omx_clap_instance, voice_flags));
  /* ABI 9 appends the MPE zones after the voice info */
  CHECK(offsetof(struct omx_clap_instance, mpe_zones) >= offsetof(struct omx_clap_instance, voice_flags) + sizeof(uint64_t),
        "the MPE zones follow the voice info (%zu)", offsetof(struct omx_clap_instance, mpe_zones));
  /* ABI 10 appends what the plugin tells the host's note-ports, note-name and voice-info after the MPE zones */
  CHECK(offsetof(struct omx_clap_instance, note_ports_rescan_flags) >= offsetof(struct omx_clap_instance, mpe_zones) + sizeof(struct omx_clap_mpe_zones) &&
            offsetof(struct omx_clap_instance, note_ports_rescanned) > offsetof(struct omx_clap_instance, note_ports_rescan_flags) &&
            offsetof(struct omx_clap_instance, note_ports_stale) > offsetof(struct omx_clap_instance, note_ports_rescanned) &&
            offsetof(struct omx_clap_instance, note_names_changed) > offsetof(struct omx_clap_instance, note_ports_stale) &&
            offsetof(struct omx_clap_instance, voice_info_changed) > offsetof(struct omx_clap_instance, note_names_changed),
        "the host's note details follow the MPE zones (%zu)", offsetof(struct omx_clap_instance, voice_info_changed));
  /* ABI 11 appends the full host's timers, descriptors, pool, tail and sleep after them */
  CHECK(offsetof(struct omx_clap_instance, timer_support) > offsetof(struct omx_clap_instance, voice_info_changed) &&
            offsetof(struct omx_clap_instance, timers) > offsetof(struct omx_clap_instance, tail) &&
            offsetof(struct omx_clap_instance, fds) > offsetof(struct omx_clap_instance, timer_next_id) &&
            offsetof(struct omx_clap_instance, sleep) > offsetof(struct omx_clap_instance, tail_changed),
        "the full host's fields follow the host's note details (%zu)", offsetof(struct omx_clap_instance, sleep));
  /* ABI 14 appends the layout extensions, the reported ports rescan and the layout requests' extensions */
  CHECK(offsetof(struct omx_clap_instance, surround) >= offsetof(struct omx_clap_instance, sleep) + sizeof(struct omx_clap_sleep) &&
            offsetof(struct omx_clap_instance, ambisonic) > offsetof(struct omx_clap_instance, surround) &&
            offsetof(struct omx_clap_instance, ports_rescanned) > offsetof(struct omx_clap_instance, ambisonic) &&
            offsetof(struct omx_clap_instance, configurable_audio_ports) > offsetof(struct omx_clap_instance, ports_rescanned) &&
            offsetof(struct omx_clap_instance, audio_ports_config) > offsetof(struct omx_clap_instance, configurable_audio_ports),
        "the surround and ambisonic extensions, the ports rescan and the layout requests' extensions follow the full host's "
        "fields (%zu)", offsetof(struct omx_clap_instance, audio_ports_config));
  /* ABI 15 appends the pass-through placement (#270); ABI 16 the idle hold's count (#296), the instance's last */
  CHECK(offsetof(struct omx_clap_instance, passthrough) >= offsetof(struct omx_clap_instance, audio_ports_config) + sizeof(void *) &&
            sizeof(struct omx_clap_instance) - offsetof(struct omx_clap_instance, passthrough) <= _Alignof(struct omx_clap_instance),
        "the pass-through placement follows the layout requests' extensions, the instance's last (%zu)", offsetof(struct omx_clap_instance, passthrough));
  config.abi = 0;
  CHECK(omx_clap_host_configure(&config) == -1, "ABI 0 is refused");
  config.abi = 11;
  CHECK(omx_clap_host_configure(&config) == -1, "a consumer of libomx-clap-core.so.0's last header (ABI 11) is refused");
  config.abi = OMX_CLAP_CORE_ABI + 1u;
  CHECK(omx_clap_host_configure(&config) == -1, "a consumer of a newer header is refused");
  config.abi = 12;
  CHECK(omx_clap_host_configure(&config) == -1, "a consumer of ABI 12, whose instance lacks the stage's wide bounce, is refused");
  config.abi = 13;
  CHECK(omx_clap_host_configure(&config) == -1, "a consumer of ABI 13, whose instance lacks the surround and ambisonic extensions, is refused");
  config.abi = OMX_CLAP_CORE_ABI;
  CHECK(omx_clap_host_configure(&config) == 0, "a consumer of this header (ABI 16) is accepted");
}

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
static uint32_t g_roster_n, g_roster_hole = UINT32_MAX, g_roster_flags;
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
  info->flags = g_roster_flags;
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

  /* parameters none of which is a row, and a refusal with nowhere to name it */
  char open_why[OMX_CLAP_WHY_MAX];
  struct omx_clap_instance *in = NULL;
  g_roster_n = 3;
  g_roster_hole = UINT32_MAX;
  g_roster_flags = CLAP_PARAM_IS_HIDDEN;
  CHECK(omx_clap_host_open_entry(&ROSTER_ENTRY, ROSTER_DESC.id, &in, open_why) == 0 && in, "open the hidden roster (%s)", open_why);
  if (!in) return;
  struct omx_clap_param_row sentinel, *rows = &sentinel;
  got = omx_clap_host_param_roster(in, &rows, NULL);
  CHECK(got == 0 && rows == NULL, "three hidden parameters: no row, and *rows NULL (got %d, rows %s)", got, rows ? "set" : "NULL");
  free(rows);
  g_roster_hole = 1;
  rows = &sentinel;
  got = omx_clap_host_param_roster(in, &rows, NULL);
  CHECK(got == -1 && rows == NULL, "a hole refused with why NULL: -1 and no rows (got %d)", got);
  g_roster_flags = 0;
  g_roster_hole = UINT32_MAX;
  omx_clap_host_close(in);
}

/* the ceiling's arm: the plugin's own count() sizes no table past it */
static void t_roster_count_is_ceilinged(void) {
  const char *why = NULL;
  uint32_t last = 0;
  const int got = roster_read(10000000u, UINT32_MAX, &why, &last);
  printf("clap_core: roster of 10000000 params -> %d rows, last id %u, refusal %s\n", got, last, why ? why : "none");
  CHECK(got == (int)OMX_CLAP_PARAM_COUNT_MAX, "a plugin claiming 10M params is capped at %u rows (got %d)", OMX_CLAP_PARAM_COUNT_MAX, got);
  CHECK(last == 1000u + OMX_CLAP_PARAM_COUNT_MAX - 1u, "the last row served is the cap's own index (id %u, want %u)", last,
        1000u + OMX_CLAP_PARAM_COUNT_MAX - 1u);
}

/* ---- 8. the side chain ---- */
/* A stereo effect whose output is its main input plus its auxiliary input, one or more of them: the count and the width
 * are set per test. g_sc_seen records what process() was handed, the count of its audio inputs and whether the first
 * auxiliary input's first channel was present. */
static uint32_t g_sc_side = 1, g_sc_width = 2;
static uint32_t g_sc_seen_inputs = 0;
static int g_sc_side_read = 0;
static uint32_t sc_ports_count(const clap_plugin_t *p, bool is_input) { (void)p; return is_input ? 1 + g_sc_side : 1; }
static bool sc_ports_get(const clap_plugin_t *p, uint32_t i, bool is_input, clap_audio_port_info_t *info) {
  (void)p;
  if (i >= sc_ports_count(p, is_input)) return false;
  memset(info, 0, sizeof *info);
  info->id = i;
  if (i == 0) {
    snprintf(info->name, sizeof info->name, "main");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
  } else {
    snprintf(info->name, sizeof info->name, "side %u", i);
    info->channel_count = g_sc_width;
    info->port_type = g_sc_width == 2 ? CLAP_PORT_STEREO : CLAP_PORT_MONO;
  }
  info->in_place_pair = CLAP_INVALID_ID;
  return true;
}
static const clap_plugin_audio_ports_t SC_PORTS = {sc_ports_count, sc_ports_get};
static bool sc_init(const clap_plugin_t *p) { (void)p; return true; }
static clap_process_status sc_process(const clap_plugin_t *p, const clap_process_t *pr) {
  (void)p;
  g_sc_seen_inputs = pr->audio_inputs_count;
  g_sc_side_read = pr->audio_inputs_count >= 2 && pr->audio_inputs[1].data32 != NULL && pr->audio_inputs[1].data32[0] != NULL;
  for (uint32_t c = 0; c < 2; c++) {
    const float *main = pr->audio_inputs[0].data32[c];
    const float *side = g_sc_side_read ? pr->audio_inputs[1].data32[g_sc_width == 2 ? c : 0] : NULL;
    float *out = pr->audio_outputs[0].data32[c];
    for (uint32_t i = 0; i < pr->frames_count; i++) out[i] = main[i] + (side ? side[i] : 0.0f);
  }
  return CLAP_PROCESS_CONTINUE;
}
static const void *sc_ext(const clap_plugin_t *p, const char *id) {
  (void)p;
  return strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0 ? &SC_PORTS : NULL;
}
static const char *const SC_FEATURES[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL};
static const clap_plugin_descriptor_t SC_DESC = {CLAP_VERSION_INIT, "org.omx-clap-host.test.core-sidechain", "core sidechain", "omx-clap-host", "", "", "", "0", "",
                                                 SC_FEATURES};
static uint32_t sc_count(const clap_plugin_factory_t *f) { (void)f; return 1; }
static const clap_plugin_descriptor_t *sc_desc(const clap_plugin_factory_t *f, uint32_t i) { (void)f; return i == 0 ? &SC_DESC : NULL; }
static const clap_plugin_t *sc_create(const clap_plugin_factory_t *f, const clap_host_t *host, const char *id) {
  (void)f, (void)host;
  if (strcmp(id, SC_DESC.id) != 0) return NULL;
  clap_plugin_t *p = calloc(1, sizeof *p);
  *p = (clap_plugin_t){&SC_DESC, NULL, sc_init, ask_destroy, ask_activate, ask_nop, ask_start, ask_nop, ask_nop, sc_process, sc_ext, ask_nop};
  return p;
}
static const clap_plugin_factory_t SC_FACTORY = {sc_count, sc_desc, sc_create};
static const void *sc_factory(const char *id) { return strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &SC_FACTORY : NULL; }
static const clap_plugin_entry_t SC_ENTRY = {CLAP_VERSION_INIT, ask_entry_init, ask_entry_deinit, sc_factory};

static void sc_prime(struct omx_clap_instance *in, float l, float r) {
  float bl[64], br[64];
  for (uint32_t i = 0; i < 64; i++) bl[i] = l, br[i] = r;
  omx_clap_host_run(in, bl, br, 64);  /* the first cycle after arming fades the plugin in */
}
static int sc_expect(struct omx_clap_instance *in, float l, float r, float want_l, float want_r, const char *what) {
  float bl[64], br[64];
  int ok = 1;
  for (uint32_t i = 0; i < 64; i++) bl[i] = l, br[i] = r;
  omx_clap_host_run(in, bl, br, 64);
  for (uint32_t i = 0; i < 64; i++) ok = ok && fabsf(bl[i] - want_l) < 1e-6f && fabsf(br[i] - want_r) < 1e-6f;
  CHECK(ok, "%s: the lane reads %g, %g (wanted %g, %g)", what, bl[0], br[0], want_l, want_r);
  return ok;
}

/* Stop a running instance the way the stage takes it: a request, then one cycle that acknowledges it, then the unpublish */
static void sc_stop(struct omx_clap_instance *in) {
  omx_clap_request_stop(&in->stage);
  sc_prime(in, 0.0f, 0.0f);
  CHECK(omx_clap_host_unpublish(in, 10, 1000000) == 0, "unpublish the side-chain instance");
}

/* tests/fake.clap's configs, mono in and stereo out, with the fake's knob `knob` set while it is created */
static struct omx_clap_instance *open_configs(const char *knob) {
  char path[512], why[OMX_CLAP_WHY_MAX];
  struct omx_clap_instance *in = NULL;

  snprintf(path, sizeof(path), "%s/fake.clap", g_fault_dir);
  if (knob) setenv(knob, "1", 1);
  if (omx_clap_host_open(path, "org.omx-clap-host.test.configs", &in, why) != 0) in = NULL;
  if (knob) unsetenv(knob);
  return in;
}

/* the warm-up's cycle and a ports rescan: the warm-up's own deactivate is not reported, a rescan the plugin makes in
 * its activate is, in every activate or in the first one only, and one made before the activate is still reported
 * after it: only a read of the description (request_layout) answers it */
static void t_rescan_across_warmup(void) {
  struct omx_clap_instance *in = open_configs("FAKE_RESCAN_ON_DEACTIVATE");
  char why[OMX_CLAP_WHY_MAX];

  CHECK(in && omx_clap_host_activate(in, 48000.0, 64, why) == 0, "configs, rescanning in its deactivate, activates with the warm-up");
  if (in) {
    CHECK(omx_clap_host_tick(in) == 0 && omx_clap_host_ports_rescanned(in) == 0, "the warm-up's own deactivate is not reported");
    omx_clap_host_close(in);
  }
  in = open_configs("FAKE_RESCAN_ON_ACTIVATE");
  CHECK(in && omx_clap_host_activate(in, 48000.0, 64, why) == 0, "configs, rescanning in its activate, activates with the warm-up");
  if (in) {
    CHECK(omx_clap_host_tick(in) == 0 && omx_clap_host_ports_rescanned(in) == 1 && omx_clap_host_ports_rescanned(in) == 0,
          "the rescan its activate made is reported once, past the warm-up's cycle");
    omx_clap_host_close(in);
  }
  in = open_configs("FAKE_RESCAN_ON_FIRST_ACTIVATE");
  CHECK(in && omx_clap_host_activate(in, 48000.0, 64, why) == 0, "configs, rescanning in its first activate only, activates with the warm-up");
  if (in) {
    CHECK(omx_clap_host_tick(in) == 0 && omx_clap_host_ports_rescanned(in) == 1 && omx_clap_host_ports_rescanned(in) == 0,
          "the rescan only the warm-up's first activate made is reported once");
    omx_clap_host_close(in);
  }
  in = open_configs(NULL);
  if (in) {
    const clap_host_audio_ports_t *ports = in->host.get_extension(&in->host, CLAP_EXT_AUDIO_PORTS);

    ports->rescan(&in->host, CLAP_AUDIO_PORTS_RESCAN_LIST);
    CHECK(omx_clap_host_activate(in, 48000.0, 64, why) == 0, "configs activates after a rescan it made while inactive");
    CHECK(omx_clap_host_tick(in) == 0 && omx_clap_host_ports_rescanned(in) == 1, "the rescan made before the activate is still reported");
    omx_clap_host_deactivate(in);
    ports->rescan(&in->host, CLAP_AUDIO_PORTS_RESCAN_LIST);
    CHECK(omx_clap_host_request_layout(in, NULL, 0, why) == OMX_CLAP_LAYOUT_DECLARED && omx_clap_host_activate(in, 48000.0, 64, why) == 0,
          "read again by request_layout, then activated");
    CHECK(omx_clap_host_tick(in) == 0 && omx_clap_host_ports_rescanned(in) == 0, "the read answered it: nothing reported");
    omx_clap_host_close(in);
  } else {
    CHECK(0, "open configs");
  }
}

static void t_side_chain(void) {
  char why[OMX_CLAP_WHY_MAX];
  struct omx_clap_instance *in = NULL;
  float side_l[64], side_r[64];
  float dummy_l[64], dummy_r[64];
  float *side[2] = {side_l, side_r}, *dummy[2] = {dummy_l, dummy_r};

  g_sc_side = 1;
  g_sc_width = 2;
  CHECK(omx_clap_host_open_entry(&SC_ENTRY, SC_DESC.id, &in, why) == 0, "a stereo effect with a stereo side chain is admitted (%s)", why);
  if (!in) return;
  CHECK(in->aux_inputs == 1 && in->aux_in[0].channels == 2 && in->in_channels == 2, "one stereo auxiliary input beside the stereo pair (%u, %u, %u)",
        in->aux_inputs, in->aux_in[0].channels, in->in_channels);
  CHECK(omx_clap_host_activate(in, 48000.0, 64, why) == 0, "activate (%s)", why);
  omx_clap_host_publish(in, pthread_self());
  sc_prime(in, 0.5f, -0.25f);
  CHECK(g_sc_seen_inputs == 2 && g_sc_side_read, "the plugin is handed two inputs, the second with its channels (%u, %d)", g_sc_seen_inputs, g_sc_side_read);
  sc_expect(in, 0.5f, -0.25f, 0.5f, -0.25f, "unbound: the side chain is silence");

  CHECK(omx_clap_host_bind_aux_input(in, 0, dummy, 2, 64) == -1, "binding while the instance runs is refused");
  sc_stop(in);
  CHECK(omx_clap_host_bind_aux_input(in, 1, dummy, 2, 64) == -1, "a port past the auxiliary inputs is refused");
  CHECK(omx_clap_host_bind_aux_input(in, 0, dummy, 1, 64) == -1, "a stereo port bound with one channel is refused");
  for (uint32_t i = 0; i < 64; i++) side_l[i] = 0.25f, side_r[i] = 0.125f;
  CHECK(omx_clap_host_bind_aux_input(in, 0, dummy, 2, 32) == -1, "a pair shorter than the active block is refused");
  CHECK(omx_clap_host_bind_aux_input(in, 0, side, 2, 64) == 0, "bind a stereo pair to the side chain");
  omx_clap_host_publish(in, pthread_self());
  sc_prime(in, 0.5f, -0.25f);
  sc_expect(in, 0.5f, -0.25f, 0.75f, -0.125f, "bound: the plugin reads the caller's pair");
  sc_stop(in);

  omx_clap_host_deactivate(in);
  CHECK(omx_clap_host_activate(in, 48000.0, 128, why) == -1, "a block longer than the bound pair is refused while it is bound");
  CHECK(omx_clap_host_activate(in, 48000.0, 64, why) == 0, "activate again (%s)", why);
  omx_clap_host_publish(in, pthread_self());
  sc_prime(in, 0.5f, -0.25f);
  sc_expect(in, 0.5f, -0.25f, 0.75f, -0.125f, "the binding outlives a deactivate and activate");
  sc_stop(in);

  CHECK(omx_clap_host_bind_aux_input(in, 0, NULL, 2, 0) == 0, "unbind the side chain");
  omx_clap_host_publish(in, pthread_self());
  sc_prime(in, 0.5f, -0.25f);
  sc_expect(in, 0.5f, -0.25f, 0.5f, -0.25f, "unbound again: silence");
  sc_stop(in);
  omx_clap_host_close(in);
}

static void t_side_chain_mono(void) {
  char why[OMX_CLAP_WHY_MAX];
  struct omx_clap_instance *in = NULL;
  float side_m[64];
  float *side[1] = {side_m};

  g_sc_side = 1;
  g_sc_width = 1;
  CHECK(omx_clap_host_open_entry(&SC_ENTRY, SC_DESC.id, &in, why) == 0 && in && in->aux_in[0].channels == 1, "a mono side chain is admitted (%s)", why);
  if (!in) return;
  CHECK(omx_clap_host_activate(in, 48000.0, 64, why) == 0, "activate the mono side chain (%s)", why);
  for (uint32_t i = 0; i < 64; i++) side_m[i] = 0.25f;
  CHECK(omx_clap_host_bind_aux_input(in, 0, side, 1, 64) == 0, "a mono port binds its one channel");
  omx_clap_host_publish(in, pthread_self());
  sc_prime(in, 0.5f, 0.5f);
  sc_expect(in, 0.5f, 0.5f, 0.75f, 0.75f, "a mono side chain feeds both channels of the stereo effect");
  sc_stop(in);
  omx_clap_host_close(in);
  g_sc_width = 2;
}

static void t_side_chain_refused(void) {
  char why[OMX_CLAP_WHY_MAX];
  struct omx_clap_instance *in = NULL;

  g_sc_side = CLAP_HOST_AUX_INPUTS + 1;
  g_sc_width = 2;
  CHECK(omx_clap_host_open_entry(&SC_ENTRY, SC_DESC.id, &in, why) == -1 && in == NULL, "one side chain past the core's count is refused");
  CHECK(strcmp(why, "hosting.topology.too-many-ports") == 0, "and the code is hosting.topology.too-many-ports (%s)", why);
  g_sc_side = 1;
  g_sc_width = 3;
  CHECK(omx_clap_host_open_entry(&SC_ENTRY, SC_DESC.id, &in, why) == 0 && in && in->aux_in[0].channels == 3, "a side chain of three channels is admitted (%s)", why);
  if (in) omx_clap_host_close(in);
  in = NULL;
  g_sc_width = CLAP_HOST_PORT_CHANNELS + 1;
  CHECK(omx_clap_host_open_entry(&SC_ENTRY, SC_DESC.id, &in, why) == -1 && in == NULL, "a side chain of 65 channels is refused");
  CHECK(strcmp(why, "hosting.topology.port-too-wide") == 0, "and the code is hosting.topology.port-too-wide (%s)", why);
  g_sc_width = 0;
  CHECK(omx_clap_host_open_entry(&SC_ENTRY, SC_DESC.id, &in, why) == -1 && in == NULL, "a side chain of no channels is refused");
  CHECK(strcmp(why, "hosting.topology.port-without-channels") == 0, "and the code is hosting.topology.port-without-channels (%s)", why);
  g_sc_width = 2;
  g_sc_side = CLAP_HOST_AUX_INPUTS;
  CHECK(omx_clap_host_open_entry(&SC_ENTRY, SC_DESC.id, &in, why) == 0 && in && in->aux_inputs == CLAP_HOST_AUX_INPUTS, "exactly the core's count is admitted (%s)", why);
  if (in) omx_clap_host_close(in);
  g_sc_side = 1;
}

/* ---- 5. the tempo ---- */

/* The tempo fake: a stereo pass-through that records the transport its last process() was given. */
static struct {
  int saw_transport;   /* 1: a transport was passed, 0: NULL */
  int t_present;       /* the same, for the copy below */
  clap_event_transport_t t_copy;  /* the whole transport record, as delivered */
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
  g_tp.t_present = t != NULL;
  if (t) g_tp.t_copy = *t;
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

/* ---- 5b. the host's published transport ---- */

/* The published record reaches the plugin whole: flags, tempo, beats, seconds, bar and time signature. A withdrawn
 * record is no transport; the tempo word decides again once the source is unset; a read that does not come whole keeps
 * the block's last whole record. */
static void t_transport_record(void) {
  char why[OMX_CLAP_WHY_MAX];
  struct omx_clap_instance *in = NULL;
  CHECK(omx_clap_host_open_entry(&TP_ENTRY, TP_DESC.id, &in, why) == 0 && omx_clap_host_activate(in, 48000.0, 64, why) == 0, "the transport reader opens for the record (%s)", why);
  if (!in) return;
  omx_clap_host_publish(in, pthread_self());

  struct omx_clap_transport_src src;
  memset(&src, 0, sizeof src);
  struct omx_clap_transport rec;
  memset(&rec, 0, sizeof rec);
  rec.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE |
              CLAP_TRANSPORT_HAS_TIME_SIGNATURE | CLAP_TRANSPORT_IS_PLAYING;
  rec.tempo = 120.0;
  rec.song_pos_beats = 5 * (CLAP_BEATTIME_FACTOR / 2);  /* 2.5 beats */
  rec.song_pos_seconds = 3 * (CLAP_SECTIME_FACTOR / 2); /* 1.5 seconds */
  rec.bar_start = 4 * CLAP_BEATTIME_FACTOR;             /* the bar starts at beat 4 */
  rec.bar_number = 1;
  rec.tsig_num = 7;
  rec.tsig_denom = 8;
  omx_clap_transport_publish(&src, &rec);
  omx_clap_host_set_transport(in, &src);
  tp_run(in);
  const clap_event_transport_t *t = &g_tp.t_copy;
  CHECK(g_tp.t_present, "a published transport reaches the plugin");
  CHECK(t->flags == rec.flags, "the flags as published (0x%x)", t->flags);
  CHECK(t->tempo == 120.0, "the tempo as published (%g)", t->tempo);
  CHECK(t->song_pos_beats == rec.song_pos_beats && t->song_pos_seconds == rec.song_pos_seconds, "the position in beats and seconds as published");
  CHECK(t->bar_start == rec.bar_start && t->bar_number == 1, "the bar start and number as published");
  CHECK(t->tsig_num == 7 && t->tsig_denom == 8, "the time signature 7/8 as published (%u/%u)", t->tsig_num, t->tsig_denom);
  CHECK(g_tp.saw_theader_ok, "the transport's header is CLAP_EVENT_TRANSPORT, its size, time 0");

  /* a later publish: the playing state and the position change at the next block */
  rec.flags &= ~(uint32_t)CLAP_TRANSPORT_IS_PLAYING;
  rec.song_pos_beats += CLAP_BEATTIME_FACTOR;
  omx_clap_transport_publish(&src, &rec);
  tp_run(in);
  CHECK(!(g_tp.t_copy.flags & CLAP_TRANSPORT_IS_PLAYING) && g_tp.t_copy.song_pos_beats == rec.song_pos_beats,
        "a stopped position arrives at the next block (flags 0x%x)", g_tp.t_copy.flags);

  /* the tempo word is superseded while the source is set, and decides again once it is unset */
  struct omx_clap_tempo word;
  memset(&word, 0, sizeof word);
  omx_clap_tempo_publish(&word, 90.0);
  omx_clap_host_set_tempo(in, &word);
  tp_run(in);
  CHECK(g_tp.t_copy.tempo == 120.0, "the published transport supersedes the tempo word (%g)", g_tp.t_copy.tempo);
  omx_clap_host_set_transport(in, NULL);
  tp_run(in);
  CHECK(g_tp.t_present && g_tp.t_copy.flags == CLAP_TRANSPORT_HAS_TEMPO && g_tp.t_copy.tempo == 90.0,
        "unset, the tempo word decides again: HAS_TEMPO at 90 and nothing else");
  omx_clap_host_set_transport(in, &src);

  /* a withdrawn record is no transport at all */
  memset(&rec, 0, sizeof rec);
  omx_clap_transport_publish(&src, &rec);
  tp_run(in);
  CHECK(!g_tp.t_present, "a record with no flags passes transport NULL");

  /* the read: whole when no publish runs over it, and left alone when one does */
  struct omx_clap_transport out;
  memset(&out, 0, sizeof out);
  rec.flags = CLAP_TRANSPORT_HAS_TEMPO;
  rec.tempo = 77.0;
  omx_clap_transport_publish(&src, &rec);
  CHECK(omx_clap_transport_read(&src, &out) == 1 && out.tempo == 77.0, "a read between publishes is whole");
  tp_run(in);
  CHECK(g_tp.t_present && g_tp.t_copy.tempo == 77.0, "the block reads the whole record (%g)", g_tp.t_copy.tempo);
  atomic_fetch_add(&src.seq, 1u); /* a publish under way: the sequence is odd */
  struct omx_clap_transport torn;
  memset(&torn, 0, sizeof torn);
  CHECK(omx_clap_transport_read(&src, &torn) == 0 && torn.flags == 0, "a read over a publish under way does not come whole");
  atomic_store(&src.tempo_bits, 0);
  tp_run(in);
  CHECK(g_tp.t_present && g_tp.t_copy.tempo == 77.0, "a block that does not read whole keeps the last whole record (%g)", g_tp.t_copy.tempo);
  atomic_fetch_add(&src.seq, 1u);
  omx_clap_host_set_transport(in, NULL);
  omx_clap_request_stop(&in->stage);
  tp_run(in);
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
static _Atomic int g_rs_slow; /* process() takes ~300 us: a cycle is under way half the time */
static void rs_reset(const clap_plugin_t *p) {
  (void)p;
  if (pthread_equal(pthread_self(), g_rs_rt)) atomic_fetch_add(&g_rs_on_rt, 1u);
  else atomic_fetch_add(&g_rs_off_rt, 1u);
}
static clap_process_status rs_process(const clap_plugin_t *p, const clap_process_t *pr) {
  (void)p;
  atomic_fetch_add(&g_rs_process, 1u);
  if (atomic_load(&g_rs_slow)) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do clock_gettime(CLOCK_MONOTONIC, &t1);
    while ((t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec) < 300000L);
  }
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
  _Atomic float last;
  _Atomic int watch;          /* count the blocks that are not the plugin's output */
  _Atomic uint32_t not_wet;
} g_rs;
static void *rs_rt(void *arg) {
  (void)arg;
  float l[64], r[64];
  while (!atomic_load(&g_rs.go)) sched_yield();
  while (!atomic_load(&g_rs.quit)) {
    for (int i = 0; i < 64; i++) l[i] = r[i] = 0.5f;
    omx_clap_host_run(g_rs.in, l, r, 64);
    atomic_store(&g_rs.last, l[32]);
    if (atomic_load(&g_rs.watch) && (l[0] != 0.05f || l[63] != 0.05f)) atomic_fetch_add(&g_rs.not_wet, 1u);
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
  CHECK(atomic_load(&g_rs_process) > idle_from && atomic_load(&g_rs.last) == 0.05f, "processing again after the re-engage (%g)", (double)atomic_load(&g_rs.last));
  /* an off that was already off, many times over a stage whose cycle is under way half the time: holding the role for
   * it would pass a block dry and fade back in */
  atomic_store(&g_rs_slow, 1);
  rs_wait(4);
  atomic_store(&g_rs.watch, 1);
  const uint32_t watched0 = atomic_load(&g_rs.blocks);
  for (int k = 0; k < 200; k++) {
    omx_clap_host_bypass(g_rs.in, 0);
    usleep(150);
  }
  rs_wait(4);
  atomic_store(&g_rs.watch, 0);
  atomic_store(&g_rs_slow, 0);
  CHECK(atomic_load(&g_rs.not_wet) == 0, "a redundant off leaves every block the plugin's output (%u of %u moved)",
        atomic_load(&g_rs.not_wet), atomic_load(&g_rs.blocks) - watched0);
  CHECK(atomic_load(&g_rs_off_rt) - off0 == 1 && atomic_load(&g_rs.in->stage.resets) == 1, "an off that was already off resets nothing");
  omx_clap_request_stop(&g_rs.in->stage);
  rs_wait(2);
  atomic_store(&g_rs.quit, 1);
  pthread_join(rt, NULL);
  CHECK(omx_clap_host_unpublish(g_rs.in, 10, 1000) == 0, "unpublished");
  omx_clap_host_close(g_rs.in);
}

/* ---- 9. the meters, in process ---- */
/* A stereo pass-through declaring clap.meters/1: meter 7 "gain reduction" of one channel, -6 dB while the input's peak
 * reaches 0.5 and 0 otherwise, and meter 9 "input level" of two channels, each channel's peak. g_mt_shape breaks the
 * declarations: 1 a meter of no channel, 2 a meter with no info, 3 more meters than the library reads. */
static int g_mt_shape = 0;
static _Atomic int g_mt_processed = 0;
static _Atomic uint64_t g_mt_bits[3];
static uint32_t g_mt_capacity_asked = 0;
static void mt_publish(int at, double v) { uint64_t b; memcpy(&b, &v, sizeof b); atomic_store(&g_mt_bits[at], b); }
static double mt_read(int at) { uint64_t b = atomic_load(&g_mt_bits[at]); double v; memcpy(&v, &b, sizeof v); return v; }
static clap_process_status mt_process(const clap_plugin_t *p, const clap_process_t *pr) {
  (void)p;
  float peak[2] = {0.0f, 0.0f};
  for (uint32_t c = 0; c < 2; c++) {
    const float *in = pr->audio_inputs[0].data32[c];
    float *out = pr->audio_outputs[0].data32[c];
    for (uint32_t i = 0; i < pr->frames_count; i++) {
      out[i] = in[i];
      if (fabsf(in[i]) > peak[c]) peak[c] = fabsf(in[i]);
    }
  }
  mt_publish(0, (peak[0] >= 0.5f || peak[1] >= 0.5f) ? -6.0 : 0.0);
  mt_publish(1, peak[0] > 0.0f ? 20.0 * log10(peak[0]) : -120.0);
  mt_publish(2, peak[1] > 0.0f ? 20.0 * log10(peak[1]) : -120.0);
  atomic_store(&g_mt_processed, 1);
  return CLAP_PROCESS_CONTINUE;
}
static uint32_t mt_meters_count(const clap_plugin_t *p) { (void)p; return g_mt_shape == 3 ? OMX_CLAP_METER_COUNT_MAX + 1u : 2u; }
static bool mt_meters_info(const clap_plugin_t *p, uint32_t index, clap_meter_info_t *info) {
  (void)p;
  if (index >= mt_meters_count(p) || (g_mt_shape == 2 && index == 1)) return false;
  memset(info, 0, sizeof *info);
  info->id = index == 0 ? 7 : index == 1 ? 9 : 100 + index;
  snprintf(info->name, sizeof info->name, "%s", index == 0 ? "gain reduction" : "input level");
  info->kind = index == 0 ? CLAP_METER_KIND_GAIN_REDUCTION : CLAP_METER_KIND_LEVEL;
  info->flags = CLAP_METER_IS_COHERENT;
  info->channel_count = index == 0 ? 1 : (g_mt_shape == 1 ? 0 : 2);
  info->min_value = index == 0 ? -24.0 : -60.0;
  info->max_value = 0.0;
  return true;
}
static uint32_t mt_meters_values(const clap_plugin_t *p, clap_id id, double *values, uint32_t capacity) {
  (void)p;
  g_mt_capacity_asked = capacity;
  if (!atomic_load(&g_mt_processed) || capacity == 0) return 0;
  if (id == 7) { values[0] = mt_read(0); return 1; }
  if (id != 9) return 0;
  values[0] = mt_read(1);
  if (capacity < 2) return 1;
  values[1] = mt_read(2);
  return 2;
}
static const clap_plugin_meters_t MT_METERS = {mt_meters_count, mt_meters_info, mt_meters_values};
static const void *mt_ext(const clap_plugin_t *p, const char *id) {
  (void)p;
  if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &SC_PORTS;
  if (strcmp(id, CLAP_EXT_METERS) == 0) return &MT_METERS;
  return NULL;
}
static const clap_plugin_descriptor_t MT_DESC = {CLAP_VERSION_INIT, "org.omx-clap-host.test.core-meters", "core meters", "omx-clap-host", "", "", "", "0", "",
                                                 SC_FEATURES};
static const clap_plugin_descriptor_t *mt_desc(const clap_plugin_factory_t *f, uint32_t i) { (void)f; return i == 0 ? &MT_DESC : NULL; }
static const clap_plugin_t *mt_create(const clap_plugin_factory_t *f, const clap_host_t *host, const char *id) {
  (void)f, (void)host;
  if (strcmp(id, MT_DESC.id) != 0) return NULL;
  clap_plugin_t *p = calloc(1, sizeof *p);
  *p = (clap_plugin_t){&MT_DESC, NULL, sc_init, ask_destroy, ask_activate, ask_nop, ask_start, ask_nop, ask_nop, mt_process, mt_ext, ask_nop};
  return p;
}
static const clap_plugin_factory_t MT_FACTORY = {sc_count, mt_desc, mt_create};
static const void *mt_factory(const char *id) { return strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &MT_FACTORY : NULL; }
static const clap_plugin_entry_t MT_ENTRY = {CLAP_VERSION_INIT, ask_entry_init, ask_entry_deinit, mt_factory};

/* ---- 9. the note output ---- */
#define NO_FLOOD 300u
static _Atomic uint32_t g_no_runs;
static uint32_t no_note_ports_count(const clap_plugin_t *p, bool is_input) { (void)p; return is_input ? 0 : 1; }
static bool no_note_ports_get(const clap_plugin_t *p, uint32_t i, bool is_input, clap_note_port_info_t *info) {
  (void)p;
  if (is_input || i != 0) return false;
  memset(info, 0, sizeof *info);
  info->supported_dialects = CLAP_NOTE_DIALECT_CLAP;
  info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
  snprintf(info->name, sizeof info->name, "notes out");
  return true;
}
static const clap_plugin_note_ports_t NO_NOTE_PORTS = {no_note_ports_count, no_note_ports_get};
static clap_process_status no_process(const clap_plugin_t *p, const clap_process_t *pr) {
  (void)p;
  atomic_fetch_add(&g_no_runs, 1);
  for (uint32_t c = 0; c < 2; c++) memcpy(pr->audio_outputs[0].data32[c], pr->audio_inputs[0].data32[c], pr->frames_count * sizeof(float));
  clap_event_note_t e;
  memset(&e, 0, sizeof e);
  e.header = (clap_event_header_t){sizeof e, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_ON, 0};
  e.note_id = -1, e.port_index = 0, e.velocity = 1.0;
  for (uint32_t i = 0; i < NO_FLOOD; i++) {
    e.header.time = i % pr->frames_count;
    e.channel = (int16_t)(i / 128 % 16), e.key = (int16_t)(i % 128);
    pr->out_events->try_push(pr->out_events, &e.header);
  }
  clap_event_note_expression_t x;
  memset(&x, 0, sizeof x);
  x.header = (clap_event_header_t){sizeof x, pr->frames_count - 1, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_EXPRESSION, 0};
  x.note_id = -1, x.port_index = 0, x.channel = 0, x.key = 0, x.value = 0.5;
  pr->out_events->try_push(pr->out_events, &x.header);
  e.header.type = CLAP_EVENT_NOTE_END, e.header.time = pr->frames_count - 1;
  pr->out_events->try_push(pr->out_events, &e.header);
  return CLAP_PROCESS_CONTINUE;
}
static const void *no_ext(const clap_plugin_t *p, const char *id) {
  (void)p;
  if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &ASK_PORTS;
  if (strcmp(id, CLAP_EXT_NOTE_PORTS) == 0) return &NO_NOTE_PORTS;
  return NULL;
}
static const clap_plugin_descriptor_t NO_DESC = {CLAP_VERSION_INIT, "org.omx-clap-host.test.core-notes-out", "core notes out", "omx-clap-host", "", "", "", "0", "",
                                                 ASK_FEATURES};
static const clap_plugin_descriptor_t *no_desc(const clap_plugin_factory_t *f, uint32_t i) { (void)f; return i == 0 ? &NO_DESC : NULL; }
static const clap_plugin_t *no_create(const clap_plugin_factory_t *f, const clap_host_t *host, const char *id) {
  (void)f, (void)host;
  if (strcmp(id, NO_DESC.id) != 0) return NULL;
  clap_plugin_t *p = calloc(1, sizeof *p);
  *p = (clap_plugin_t){&NO_DESC, NULL, sc_init, ask_destroy, ask_activate, ask_nop, ask_start, ask_nop, ask_nop, no_process, no_ext, ask_nop};
  return p;
}
static const clap_plugin_factory_t NO_FACTORY = {ask_count, no_desc, no_create};
static const void *no_factory(const char *id) { return strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &NO_FACTORY : NULL; }
static const clap_plugin_entry_t NO_ENTRY = {CLAP_VERSION_INIT, ask_entry_init, ask_entry_deinit, no_factory};

static void t_notes_out(void) {
  static struct omx_clap_note_out_event got[OMX_CLAP_NOTE_OUT_DEPTH + 8];
  char why[OMX_CLAP_WHY_MAX];
  struct omx_clap_instance *in = NULL;
  float bl[64], br[64];

  CHECK(omx_clap_host_open_entry(&NO_ENTRY, NO_DESC.id, &in, why) == 0, "the note output effect opens (%s)", why);
  if (!in) return;
  CHECK(in->note_outputs == 1 && in->note_inputs == 0, "its note output port is read at open, with note inputs refused (%u, %u)", in->note_outputs,
        in->note_inputs);
  atomic_store(&g_no_runs, 0);
  CHECK(omx_clap_host_activate(in, 48000.0, 64, why) == 0, "activate it (%s)", why);
  CHECK(atomic_load(&g_no_runs) >= CLAP_HOST_WARMUP_BLOCKS && omx_clap_host_notes_out(in, got, OMX_CLAP_NOTE_OUT_DEPTH + 8) == 0 &&
            atomic_load(&in->note_out.dropped) == 0 && atomic_load(&in->note_out.unmapped) == 0,
        "the warm-up ran (%u blocks) and sent nothing out", atomic_load(&g_no_runs));
  omx_clap_host_publish(in, pthread_self());
  for (uint32_t b = 1; b <= 2; b++) {
    const int64_t start = in->stage.steady_time;
    for (uint32_t i = 0; i < 64; i++) bl[i] = br[i] = 0.25f;
    in_rt = 1;
    omx_clap_host_run(in, bl, br, 64);
    const uint32_t n = omx_clap_host_notes_out(in, got, OMX_CLAP_NOTE_OUT_DEPTH + 8);
    in_rt = 0;
    CHECK(n == OMX_CLAP_NOTE_OUT_DEPTH && got[0].frame == start && got[0].data[0] == 0x90 && got[0].data[1] == 0 && got[0].data[2] == 127 &&
              got[n - 1].frame == start + (OMX_CLAP_NOTE_OUT_DEPTH - 1) % 64 && got[n - 1].data[0] == 0x91 &&
              got[n - 1].data[1] == (OMX_CLAP_NOTE_OUT_DEPTH - 1) % 128,
          "block %u: %u of the %u note ons come out, the first at the block's first frame (%u)", b, OMX_CLAP_NOTE_OUT_DEPTH, NO_FLOOD, n);
    CHECK(atomic_load(&in->note_out.dropped) == b * (NO_FLOOD - OMX_CLAP_NOTE_OUT_DEPTH) && atomic_load(&in->note_out.unmapped) == b,
          "block %u: %u dropped and the expression unmapped, so far %u and %u; the NOTE_END is neither", b, NO_FLOOD - OMX_CLAP_NOTE_OUT_DEPTH,
          atomic_load(&in->note_out.dropped), atomic_load(&in->note_out.unmapped));
  }
  omx_clap_request_stop(&in->stage);
  omx_clap_host_run(in, bl, br, 64);
  CHECK(omx_clap_host_unpublish(in, 10, 1000000) == 0, "unpublish the note output effect");
  omx_clap_host_close(in);
}

/* the telemetry thread: one read of meter 7 off the audio thread */
struct mt_read_job { struct omx_clap_instance *in; double gr; uint32_t n; };
static void *mt_telemetry(void *arg) {
  struct mt_read_job *job = arg;
  job->n = omx_clap_host_meter_values(job->in, 7, &job->gr, 1);
  return NULL;
}

static void t_meters(void) {
  char why[OMX_CLAP_WHY_MAX];
  struct omx_clap_instance *in = NULL;
  struct omx_clap_meter m;
  double v[4] = {99.0, 99.0, 99.0, 99.0};

  g_sc_side = 0;
  g_mt_shape = 0;
  atomic_store(&g_mt_processed, 0);
  CHECK(omx_clap_host_open_entry(&MT_ENTRY, MT_DESC.id, &in, why) == 0, "the metering plugin opens (%s)", why);
  if (!in) { g_sc_side = 1; return; }
  CHECK(omx_clap_host_meter_count(in) == 2, "two meters are read at open (%u)", omx_clap_host_meter_count(in));
  CHECK(omx_clap_host_meter_info(in, 0, &m) == 0 && m.id == 7 && strcmp(m.name, "gain reduction") == 0 && m.kind == OMX_CLAP_METER_KIND_GAIN_REDUCTION
          && m.channel_count == 1 && m.flags == CLAP_METER_IS_COHERENT && m.min_value == -24.0 && m.max_value == 0.0,
        "meter 0 is the gain reduction as declared (%u \"%s\" kind %u, %u channel)", m.id, m.name, m.kind, m.channel_count);
  CHECK(omx_clap_host_meter_info(in, 1, &m) == 0 && m.id == 9 && m.kind == OMX_CLAP_METER_KIND_LEVEL && m.channel_count == 2,
        "meter 1 is the stereo input level (%u, %u channels)", m.id, m.channel_count);
  CHECK(omx_clap_host_meter_info(in, 2, &m) == -1, "an index past the count has no meter");
  CHECK(omx_clap_host_meter_values(in, 7, v, 4) == 0, "before the plugin processed, a read writes nothing");
  CHECK(omx_clap_host_activate(in, 48000.0, 64, why) == 0, "activate the metering plugin (%s)", why);
  omx_clap_host_publish(in, pthread_self());
  sc_prime(in, 0.75f, -0.25f);
  sc_expect(in, 0.75f, -0.25f, 0.75f, -0.25f, "the metering plugin passes the lane through");

  struct mt_read_job job = {in, 99.0, 0};
  pthread_t tele;
  pthread_create(&tele, NULL, mt_telemetry, &job);
  pthread_join(tele, NULL);
  CHECK(job.n == 1 && job.gr == -6.0, "the telemetry thread reads the plugin's gain reduction (%u: %g)", job.n, job.gr);
  CHECK(omx_clap_host_meter_values(in, 9, v, 4) == 2 && fabs(v[0] - 20.0 * log10(0.75)) < 1e-6 && fabs(v[1] - 20.0 * log10(0.25)) < 1e-6
          && v[2] == 99.0 && g_mt_capacity_asked == 2,
        "the stereo level writes its two channels, asked for no more than it declares (%g, %g; asked %u)", v[0], v[1], g_mt_capacity_asked);
  v[1] = 99.0;
  CHECK(omx_clap_host_meter_values(in, 9, v, 1) == 1 && v[1] == 99.0, "a capacity of one writes one channel");
  CHECK(omx_clap_host_meter_values(in, 8, v, 4) == 0, "an id the plugin did not declare writes nothing");
  sc_expect(in, 0.125f, 0.125f, 0.125f, 0.125f, "a quiet block");
  CHECK(omx_clap_host_meter_values(in, 7, v, 1) == 1 && v[0] == 0.0, "the reduction follows the plugin's report (%g)", v[0]);
  sc_stop(in);
  omx_clap_host_close(in);

  for (int shape = 1; shape <= 3; shape++) {
    g_mt_shape = shape;
    in = NULL;
    CHECK(omx_clap_host_open_entry(&MT_ENTRY, MT_DESC.id, &in, why) == 0 && in, "declarations broken (%d): the plugin still opens (%s)", shape, why);
    if (!in) continue;
    CHECK(omx_clap_host_meter_count(in) == 0 && omx_clap_host_meter_info(in, 0, &m) == -1 && omx_clap_host_meter_values(in, 7, v, 1) == 0,
          "declarations broken (%d): no meters, none read (%u)", shape, omx_clap_host_meter_count(in));
    omx_clap_host_close(in);
  }
  g_mt_shape = 0;
  g_sc_side = 1;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <fault dir>\n", argv[0]);
    return 2;
  }
  g_fault_dir = argv[1];
  t_abi();
  t_witness();
  t_guard_page();
  t_audio_role_on_split();
  t_roster_whole_or_refused();
  t_roster_count_is_ceilinged();
  t_tempo();
  t_transport_record();
  t_transport_in_the_cycle();
  t_reset_off_rt();
  t_rescan_across_warmup();
  t_side_chain();
  t_side_chain_mono();
  t_side_chain_refused();
  t_meters();
  t_notes_out();
  CHECK(rt_allocs == 0, "no allocation inside omx_clap_host_run across the run (%d)", rt_allocs);
  if (failures) {
    fprintf(stderr, "clap_core: %d failure(s)\n", failures);
    return 1;
  }
  printf("clap_core: all passed (rt allocations: %d)\n", rt_allocs);
  return 0;
}
