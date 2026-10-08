/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * clap_untrusted_test.c: what a plugin says about its parameters is its own answer, not a fact the core measured. A
 * fake this file owns reports a count and names of its choosing, no jack.
 *
 *   ./tests/clap_untrusted_test
 *
 *  1. THE COUNT IS CAPPED at CLAP_HOST_PARAM_COUNT_MAX (2048): a plugin reporting exactly 2048 parameters serves them all;
 *     one reporting 2049 or 10000000 serves the first 2048, an id past them is not a parameter of it, and the open
 *     leaves one log message saying so. The layout pin of a plugin past the cap is refused, never pinned short.
 *  2. A FAILED ALLOCATION REFUSES: the read-back table that open sizes from the count cannot be had, and the open is
 *     refused with a code, no instance; the roster's own table cannot be had, and the roster is refused, no rows. The
 *     same plugin opens and serves its rows once allocation works again.
 *  3. THE NAME IS BOUNDED: a name that fills CLAP_NAME_SIZE with no NUL, followed by a module path and numbers with no
 *     zero byte either, is read no further than its own buffer. The build with -fsanitize=address (make test-untrusted-asan)
 *     compiles the core into the test and catches a read past the plugin's info.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/clap_host.h"
#include "../src/clap_host_limits.h"
#include "../src/layout_pin.h"

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

/* ---- the allocator: calloc of exactly `g_fail_nmemb` elements fails while armed ---- */
#ifndef OMX_UNTRUSTED_ASAN
/* exported (the build hides everything by default), so it interposes on the core's calloc too: the core is a shared
 * library that links it from here */
static size_t g_fail_nmemb;
static int g_failed;
extern void *__libc_calloc(size_t, size_t);
__attribute__((visibility("default"))) void *calloc(size_t nmemb, size_t size) {
  if (g_fail_nmemb && nmemb == g_fail_nmemb) {
    g_failed++;
    errno = ENOMEM;
    return NULL;
  }
  return __libc_calloc(nmemb, size);
}
#endif

/* ---- the plugin: a stereo effect whose params extension answers what the test sets ---- */
static uint32_t g_n;
static int g_unterminated;  /* fill name, module and the numbers with no zero byte */

static uint32_t p_count(const clap_plugin_t *p) { (void)p; return g_n; }
static bool p_info(const clap_plugin_t *p, uint32_t i, clap_param_info_t *info) {
  (void)p;
  if (i >= g_n) return false;
  memset(info, 0, sizeof *info);
  info->id = 1000u + i;
  if (g_unterminated) {
    memset(info->name, 'x', sizeof info->name);
    memset(info->module, 'm', sizeof info->module);
    /* 0x41 in every byte: a finite double, about 2.3e6, with no zero byte */
    memset(&info->min_value, 0x41, sizeof info->min_value);
    memset(&info->max_value, 0x41, sizeof info->max_value);
    memset(&info->default_value, 0x41, sizeof info->default_value);
  } else {
    snprintf(info->name, sizeof info->name, "p%u", i);
    info->max_value = 1.0;
    info->default_value = 0.5;
  }
  return true;
}
static bool p_value(const clap_plugin_t *p, clap_id id, double *v) { (void)p, (void)id; *v = 0.5; return true; }
static bool p_to_text(const clap_plugin_t *p, clap_id id, double v, char *t, uint32_t n) {
  (void)p, (void)id;
  snprintf(t, n, "%g", v);
  return true;
}
static bool p_from_text(const clap_plugin_t *p, clap_id id, const char *t, double *v) { (void)p, (void)id; *v = atof(t); return true; }
static void p_flush(const clap_plugin_t *p, const clap_input_events_t *in, const clap_output_events_t *out) { (void)p, (void)in, (void)out; }
static const clap_plugin_params_t PARAMS = {p_count, p_info, p_value, p_to_text, p_from_text, p_flush};

static uint32_t ports_count(const clap_plugin_t *p, bool is_input) { (void)p, (void)is_input; return 1; }
static bool ports_get(const clap_plugin_t *p, uint32_t i, bool is_input, clap_audio_port_info_t *info) {
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
static const clap_plugin_audio_ports_t PORTS = {ports_count, ports_get};

static const void *p_ext(const clap_plugin_t *p, const char *id) {
  (void)p;
  if (strcmp(id, CLAP_EXT_PARAMS) == 0) return &PARAMS;
  if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &PORTS;
  return NULL;
}
static bool p_init(const clap_plugin_t *p) { (void)p; return true; }
static void p_destroy(const clap_plugin_t *p) { free((void *)p); }
static bool p_activate(const clap_plugin_t *p, double sr, uint32_t a, uint32_t b) { (void)p, (void)sr, (void)a, (void)b; return true; }
static void p_nop(const clap_plugin_t *p) { (void)p; }
static bool p_start(const clap_plugin_t *p) { (void)p; return true; }
static clap_process_status p_process(const clap_plugin_t *p, const clap_process_t *pr) { (void)p, (void)pr; return CLAP_PROCESS_CONTINUE; }

static const char *const FEATURES[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL};
static const clap_plugin_descriptor_t DESC = {CLAP_VERSION_INIT, "org.omx-clap-host.test.untrusted", "untrusted", "omx-clap-host", "", "", "", "0", "",
                                              FEATURES};
static uint32_t f_count(const clap_plugin_factory_t *f) { (void)f; return 1; }
static const clap_plugin_descriptor_t *f_desc(const clap_plugin_factory_t *f, uint32_t i) { (void)f; return i == 0 ? &DESC : NULL; }
static const clap_plugin_t *f_create(const clap_plugin_factory_t *f, const clap_host_t *host, const char *id) {
  (void)f, (void)host;
  if (strcmp(id, DESC.id) != 0) return NULL;
  clap_plugin_t *p = malloc(sizeof *p);
  *p = (clap_plugin_t){&DESC, NULL, p_init, p_destroy, p_activate, p_nop, p_start, p_nop, p_nop, p_process, p_ext, p_nop};
  return p;
}
static const clap_plugin_factory_t FACTORY = {f_count, f_desc, f_create};
static bool e_init(const char *path) { (void)path; return true; }
static void e_deinit(void) {}
static const void *e_factory(const char *id) { return strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &FACTORY : NULL; }
static const clap_plugin_entry_t ENTRY = {CLAP_VERSION_INIT, e_init, e_deinit, e_factory};

static struct omx_clap_instance *open_with(uint32_t n, char why[OMX_CLAP_WHY_MAX]) {
  struct omx_clap_instance *in = NULL;
  g_n = n;
  return omx_clap_host_open_entry(&ENTRY, DESC.id, &in, why) == 0 ? in : NULL;
}

/* ---- 1. the count is capped ---- */
#define CAP CLAP_HOST_PARAM_COUNT_MAX

/* A plugin reporting `n` parameters: every way of reading them stops at min(n, CAP), and the open says so once when it
 * capped. */
static void count_is_capped(uint32_t n) {
  char why[OMX_CLAP_WHY_MAX];
  const uint32_t want = n > CAP ? CAP : n;
  struct omx_clap_instance *in = open_with(n, why);
  CHECK(in, "open the plugin claiming %u parameters (%s)", n, why);
  if (!in) return;
  omx_clap_host_tick(in);
  const char *said = omx_clap_host_log_take(in);
  if (n > CAP) {
    char text[CLAP_HOST_LOG_BYTES];
    snprintf(text, sizeof text, "the plugin reports %u parameters; the host serves the first %u", n, CAP);
    CHECK(said && strcmp(said, text) == 0, "%u parameters: the open says it capped them (%s)", n, said ? said : "nothing");
  } else {
    CHECK(said == NULL, "%u parameters: nothing to say (%s)", n, said ? said : "nothing");
  }
  struct omx_clap_param_row *rows = NULL, row;
  const char *refused = NULL;
  const int got = omx_clap_host_param_roster(in, &rows, &refused);
  const uint32_t last = got > 0 && rows ? rows[got - 1].id : 0u;
  printf("clap_untrusted: roster of %u params -> %d rows, last id %u, refusal %s\n", n, got, last, refused ? refused : "none");
  CHECK(got == (int)want, "a plugin claiming %u params serves %u rows (got %d)", n, want, got);
  CHECK(last == 1000u + want - 1u, "the last row served is index %u (id %u, want %u)", want - 1u, last, 1000u + want - 1u);
  free(rows);
  const uint32_t counted = omx_clap_host_param_count(in);
  CHECK(counted == want, "%u params: param_count says %u (%u)", n, want, counted);
  CHECK(omx_clap_host_param_row(in, want - 1u, &row) == 0 && row.id == 1000u + want - 1u, "%u params: param_row serves index %u", n,
        want - 1u);
  CHECK(omx_clap_host_param_row(in, want, &row) == -1, "%u params: param_row refuses index %u", n, want);
  CHECK(omx_clap_host_param_is_row(in, 1000u + want - 1u), "%u params: the id of index %u is a row", n, want - 1u);
  CHECK(omx_clap_host_param_is_row(in, 1000u + want) == 0, "%u params: the id of index %u is not looked up: no row", n, want);
  if (n > CAP + 1u)
    CHECK(omx_clap_host_param_row_of(in, 1000u + n - 1u, &row) == -1, "%u params: the plugin's last id is refused by row_of", n);
  omx_clap_host_tick(in);
  CHECK(omx_clap_host_log_take(in) == NULL, "%u params: said once, not again on the reads after the open", n);
  omx_clap_host_close(in);
}

static void t_count_is_capped(void) {
  CHECK(CAP == 2048u, "the cap is 2048 (%u)", CAP);
  CHECK(OMX_CLAP_PARAM_COUNT_MAX == CAP, "clap_host.h's OMX_CLAP_PARAM_COUNT_MAX is the same number");
  count_is_capped(CAP);         /* exactly the cap: whole, nothing said */
  count_is_capped(CAP + 1u);    /* one past: capped */
  count_is_capped(10000000u);   /* far past: capped, openmixer #1142's case */
}

/* ---- the layout pin refuses past the cap: a pin names the whole layout ---- */
static void t_layout_pin_refuses_past_cap(void) {
  static const clap_plugin_t plugin = {&DESC, NULL, p_init, p_destroy, p_activate, p_nop, p_start, p_nop, p_nop, p_process, p_ext, p_nop};
  char hex[PHD_SHA256_HEX_LEN + 1];
  g_n = CAP;
  CHECK(layout_pin_write(&plugin, &PARAMS, NULL, NULL, hex) == 0, "a layout of exactly %u parameters is pinned", CAP);
  g_n = CAP + 1u;
  CHECK(layout_pin_write(&plugin, &PARAMS, NULL, NULL, hex) == -1, "a layout of %u parameters is refused", CAP + 1u);
  g_n = 10000000u;
  CHECK(layout_pin_write(&plugin, &PARAMS, NULL, NULL, hex) == -1, "a layout of 10000000 parameters is refused");
}

/* ---- 2. a failed allocation refuses ---- */
static void t_failed_calloc_refuses(void) {
#ifdef OMX_UNTRUSTED_ASAN
  printf("clap_untrusted: the allocation arm runs in the plain build (make test-stage)\n");
#else
  char why[OMX_CLAP_WHY_MAX];
  struct omx_clap_instance *in;

  g_failed = 0;
  g_fail_nmemb = 777u;
  in = open_with(777u, why);
  g_fail_nmemb = 0;
  printf("clap_untrusted: open with its read-back table unallocated -> %s, why %s\n", in ? "opened" : "refused", why);
  CHECK(g_failed == 1, "the read-back table of 777 entries was asked for, and failed (%d)", g_failed);
  CHECK(in == NULL, "the open is refused, never an instance whose read-back table is missing");
  CHECK(strcmp(why, CLAP_HOST_CODE_HEADLESS_FAILED) == 0, "the refusal is coded %s (got %s)", CLAP_HOST_CODE_HEADLESS_FAILED, why);
  if (in) omx_clap_host_close(in);

  in = open_with(777u, why);
  CHECK(in && why[0] == '\0', "positive control: the same plugin opens once allocation works (%s)", why);
  if (!in) return;
  struct omx_clap_param_row sentinel, *rows = &sentinel;
  const char *refused = NULL;
  g_failed = 0;
  g_fail_nmemb = 777u;
  int got = omx_clap_host_param_roster(in, &rows, &refused);
  g_fail_nmemb = 0;
  CHECK(g_failed == 1 && got == -1 && rows == NULL, "the roster's table cannot be had: -1 and no rows (got %d)", got);
  CHECK(refused && strcmp(refused, OMX_CLAP_PARAM_ROW_UNREADABLE) == 0, "that refusal is coded %s (got %s)", OMX_CLAP_PARAM_ROW_UNREADABLE,
        refused ? refused : "none");
  got = omx_clap_host_param_roster(in, &rows, &refused);
  CHECK(got == 777, "positive control: the whole roster once allocation works (got %d)", got);
  free(rows);
  omx_clap_host_close(in);
#endif
}

/* ---- 3. the name is bounded ---- */
static void t_name_is_bounded(void) {
  char why[OMX_CLAP_WHY_MAX], want[CLAP_NAME_SIZE];
  g_unterminated = 1;
  struct omx_clap_instance *in = open_with(3u, why);
  CHECK(in, "open the plugin with unterminated names (%s)", why);
  if (!in) {
    g_unterminated = 0;
    return;
  }
  memset(want, 'x', sizeof want - 1);
  want[sizeof want - 1] = '\0';
  struct omx_clap_param_row *rows = NULL, row;
  const int got = omx_clap_host_param_roster(in, &rows, NULL);
  CHECK(got == 3, "the roster of unterminated names serves its 3 rows (got %d)", got);
  for (int i = 0; i < got && rows; i++)
    CHECK(memcmp(rows[i].name, want, sizeof want) == 0, "row %d's name is the first %zu bytes of the plugin's, terminated", i,
          sizeof want - 1);
  free(rows);
  CHECK(omx_clap_host_param_row_of(in, 1001u, &row) == 0 && memcmp(row.name, want, sizeof want) == 0,
        "row_of bounds the name the same");
  CHECK(omx_clap_host_param_row(in, 2u, &row) == 0 && memcmp(row.name, want, sizeof want) == 0, "param_row bounds the name the same");
  printf("clap_untrusted: an unterminated %u-byte name -> a row name of %zu bytes\n", (unsigned)CLAP_NAME_SIZE, strlen(row.name));
  omx_clap_host_close(in);
  g_unterminated = 0;
}

int main(void) {
  t_count_is_capped();
  t_layout_pin_refuses_past_cap();
  t_failed_calloc_refuses();
  t_name_is_bounded();
  if (failures) {
    fprintf(stderr, "clap_untrusted: %d failure(s)\n", failures);
    return 1;
  }
  printf("clap_untrusted: all checks passed\n");
  return 0;
}
