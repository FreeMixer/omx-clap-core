# Audio layouts (#92) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** the library and the LV2 adapter describe every audio port a plugin declares (any width, surround, ambisonic,
auxiliary inputs and outputs up to 64 a side) and the carriers judge the fit, so the 84 desk plugins refused today are
hosted and a layout no carrier can take is refused by a named code.

**Architecture:** one port struct (`struct omx_clap_port`) describes a port on both sides; the instance holds the two
mains and two aux arrays; the stage's pointer arrays grow to the limits; the two-leg run keeps its signature and its
bit-identity, a new `run_ports` carries any width for omx-clap-host. The LV2 adapter reads port groups into the same
model through rules 1–5. Six stacked PRs on `fix/92-audio-layouts`, each one task group below, each with its Codes
line; the new soname major lands in the first.

**Tech Stack:** C (gnu99), CLAP 1.2.10 headers (`/usr/include/clap`: surround, ambisonic, audio-ports-config,
configurable-audio-ports), lilv, JACK/PipeWire e2e under `unshare`, omx-contract renders (`make test-contract`).

**Spec:** `docs/audio-layouts-2026-10-20.md` (this repository).

## Global Constraints

- Author `Pau Aliagas <linuxnow@gmail.com>`, committer `commits@openmixer.org`, every commit GPG-signed, no AI trace or
  trailer of any kind (`clap-core-lane.sh check` refuses one). Small semantic commits.
- Limits, the only numbers: `portChannels` 64, `auxInputs` 64, `auxOutputs` 64 (`data/hosting.json`, rendered).
- New codes, exact spellings: `hosting.topology.too-many-ports`, `hosting.topology.port-too-wide`,
  `hosting.topology.port-without-channels`, `hosting.topology.channel-map-unknown`,
  `hosting.topology.ambisonic-config`. Retired: `hosting.topology.extra-inputs-fed-silence`. Kept, never reworded:
  `hosting.topology.wider-than-strip`.
- `src/clap_host_limits.h`, `src/hosted_stage_limits.h`, `ts/omx-clap-core-hosting.js`, `ts/omx-clap-core-hosting.d.ts`
  are renders of `data/hosting.json`: re-render with `omx-contract render --target c|ts --package omx-clap-core`
  (the FreeMixer/omx-contract checkout at ci.yml's `OMX_CONTRACT_REF`), never edit by hand; `make test-contract
  OMX_CONTRACT_DIR=…` must pass.
- A mono or stereo plugin on the two-leg lane is bit-identical to today (`make test-identity-lv2`, `test-identity`).
- JACK names of the main pair stay `in_<k>`/`out_<k>`; aux ports are `aux_in_<p>_<k>`/`aux_out_<p>_<k>`, `p` and `k`
  1-based.
- A field of a public struct is never edited by hand into a different offset without the new major: this lane IS the
  new major (ABI 11, `OMX_CLAP_CORE_ABI_OLDEST` 11, `CORE_MAJOR` 1, baseline `abi/libomx-clap-core.so.1.abi` +
  `.layout` recorded by `make abi-baseline`, the `.so.0` baselines removed). `VERSION` is never touched by a PR: the
  release commit sets it (coordinator 2026-10-20); new exported symbols go into the map's latest existing node, no new
  node.
- Every refusal test names the code string, never the macro alone (`check_refused(path, id, "hosting.topology.…")`
  is fine through the macro; the string must appear in the test file once so a reworded macro goes red).
- Real-time rules of the repository: nothing allocated, locked or logged inside `process()`/the RT cycle.

## Review Focus

1. A CLAP plugin whose two sides each declare two `IS_MAIN` ports: refused `too-many-ports`, never the first one
   silently taken (Task 1.5).
2. A plugin with 64 aux inputs of 64 channels each activated at `max_block` 4096: the bounce is one mapping of
   (4 + 2 + 64·64 + mains) blocks, allocated once at activation, never on the process thread; the test asserts the
   mapping size and that process() allocates nothing (Task 1.6).
3. Stereo-in/mono-out on the two-leg lane: the right leg carries the mono output too, the input's right leg is read
   (Task 2.1).
4. An LV2 plugin whose `pg:mainOutput` names a group no port belongs to: the side has no main; with other groups the
   first is main (rule 2), with none and ≤2 ports rule 3; never a crash and never an empty main of zero channels
   (Task 3.3).
5. `request_layout` on an active instance: refused -1 and nothing applied; a plugin whose `apply_configuration`
   returns false after `can_apply_configuration` returned true: answered `OMX_CLAP_LAYOUT_DECLARED`, description
   unchanged (Task 5.2).

---

## PR 1 — limits, codes, the description model, the grown bounce, the new major

Codes: `hosting.topology.too-many-ports`, `hosting.topology.port-too-wide`, `hosting.topology.port-without-channels`;
retires `hosting.topology.extra-inputs-fed-silence`.

### Task 1.1: the sheet and its renders

**Files:**
- Modify: `data/hosting.json` (`HOSTED_STAGE_LIMITS.value/docs`, `HOSTING_CODES.codes`)
- Re-render: `src/clap_host_limits.h`, `src/hosted_stage_limits.h`, `ts/omx-clap-core-hosting.js`, `ts/omx-clap-core-hosting.d.ts`

**Interfaces:**
- Produces: `CLAP_HOST_PORT_CHANNELS` 64u, `CLAP_HOST_AUX_OUTPUTS` 64u, `CLAP_HOST_AUX_INPUTS` 64u (the
  `mainPortChannels`/`CLAP_HOST_MAIN_PORT_CHANNELS` entry is gone); `CLAP_HOST_CODE_TOO_MANY_PORTS`,
  `CLAP_HOST_CODE_PORT_TOO_WIDE`, `CLAP_HOST_CODE_PORT_WITHOUT_CHANNELS` (the `CLAP_HOST_CODE_EXTRA_INPUTS` macro
  is gone); the `OMX_HOSTED_STAGE_*` twins in `hosted_stage_limits.h`.

- [ ] **Step 1:** In `data/hosting.json` replace `mainPortChannels` by `portChannels: 64` (doc: "the channels a port
  is limited to, main or auxiliary"), set `auxOutputs` and `auxInputs` to 64 (docs: "the auxiliary output/input ports
  the core accepts beyond the main one"); in `HOSTING_CODES.codes` remove `extraInputs`, add `tooManyPorts` ("more
  than the limit of auxiliary ports on one side, or more than one main port on a side"), `portTooWide` ("a port of more
  channels than the limit, or an ambisonic port whose channels are not (order+1)²"), `portWithoutChannels` ("a port of
  zero channels"); `widerThanStrip` doc becomes "a main port is wider than the lane the caller runs the plugin on".
  In both `source.impl` lists replace `src/clap_host.c#read_topology` by `src/clap_host.c#describe_ports`.
- [ ] **Step 2:** Re-render the four files with the omx-contract tool; run
  `make test-contract OMX_CONTRACT_DIR=<checkout>` → PASS. (The build is red now: every user of the old macros fails
  to compile. That is Task 1.2's job; commit only after 1.2 compiles.)

### Task 1.2: the port struct, the instance and stage fields, the new major

**Files:**
- Modify: `src/clap_host.h:70-80` (ABI), `:114-128` (config), `:262-345` (instance), `src/clap_stage.h:178-200`,
  `:320-335` (stage, aux_in), `:640-770` (bind functions), `Makefile:17` (`CORE_MAJOR`),
  `src/omx-clap-core.map` (the latest node), `abi/` (baselines), `tests/clap_core_test.c:81-83`.

**Interfaces:**
- Produces, in `clap_host.h`:
  ```c
  enum omx_clap_port_kind { OMX_CLAP_PORT_UNKNOWN = 0, OMX_CLAP_PORT_MONO, OMX_CLAP_PORT_STEREO, OMX_CLAP_PORT_SURROUND, OMX_CLAP_PORT_AMBISONIC };
  struct omx_clap_port
  {
      clap_id id;                       ///< clap_audio_port_info.id
      char name[CLAP_NAME_SIZE];
      uint32_t channels;                ///< 1..CLAP_HOST_PORT_CHANNELS
      uint32_t kind;                    ///< an omx_clap_port_kind
      uint8_t map[CLAP_HOST_PORT_CHANNELS];  ///< CLAP surround ids, `channels` of them, for a surround port
      uint32_t ambisonic_ordering, ambisonic_normalization;  ///< clap_ambisonic_config_t, for an ambisonic port
      clap_id in_place_pair;            ///< CLAP_INVALID_ID for none
  };
  ```
  Instance: `struct omx_clap_port main_in, main_out;` (`main_in.channels` 0 and `main_out.channels` 0 are "none");
  `uint32_t channels, in_channels` KEPT as today (= `main_out.channels`, `main_in.channels`, read everywhere);
  `uint32_t aux_inputs; struct omx_clap_port aux_in[CLAP_HOST_AUX_INPUTS]; uint32_t aux_outputs; struct omx_clap_port aux_out[CLAP_HOST_AUX_OUTPUTS];`
  (`aux_channels[]`, `aux_in_channels[]` gone); `float *aux_in_bound[CLAP_HOST_AUX_INPUTS][CLAP_HOST_PORT_CHANNELS]`
  → replaced by a flat `float *aux_in_bound[CLAP_HOST_AUX_INPUTS * CLAP_HOST_PORT_CHANNELS]` indexed by
  `aux_in_base[port] + c`, same for `aux_out_bound`/`aux_out_base`; `uint32_t aux_in_frames[CLAP_HOST_AUX_INPUTS]`,
  `aux_out_frames[CLAP_HOST_AUX_OUTPUTS]`.
  Config: `uint32_t lane_channels;` appended, default 2 in `omx_clap_host_config_default`; 0 = any width.
  Stage (`clap_stage.h`): `float *in_ptrs[CLAP_HOST_PORT_CHANNELS], *out_ptrs[CLAP_HOST_PORT_CHANNELS];`
  `clap_audio_buffer_t aout[1 + CLAP_HOST_AUX_OUTPUTS]; float *aux_ptrs[CLAP_HOST_AUX_OUTPUTS * CLAP_HOST_PORT_CHANNELS]; uint32_t aux_base[CLAP_HOST_AUX_OUTPUTS];`
  `struct omx_clap_aux_in`: `ains[1 + CLAP_HOST_AUX_INPUTS]`, `float *aux_in_ptrs[CLAP_HOST_AUX_INPUTS * CLAP_HOST_PORT_CHANNELS]; uint32_t aux_in_base[CLAP_HOST_AUX_INPUTS]; float *aux_silence[2];`
  (every unbound channel reads `aux_silence[0]`: one zeroed block; the second stays for the two-leg bounce's symmetry).
  Bind functions, new shapes:
  `int omx_clap_bind_aux(struct omx_clap_stage *s, float *const *scratch, uint32_t count, const struct omx_clap_port *ports)`
  (`scratch[k]` one block per aux output channel in port order; a port's `data32` = `&s->aux_ptrs[s->aux_base[i]]`),
  `void omx_clap_set_aux_in(struct omx_clap_aux_in *a, uint32_t port, uint32_t channels, float *const *bufs)` (NULL
  bufs → silence), `int omx_clap_bind_aux_in(struct omx_clap_aux_in *a, float *silence, uint32_t count, const struct omx_clap_port *ports, float *const *bound, const uint32_t *base)`,
  `void omx_clap_set_aux_out(struct omx_clap_stage *s, uint32_t port, uint32_t channels, float *const *bufs)` (NULL
  → the scratch), `omx_clap_bind_ports_aux(s, plugin, n_in, n_out, aux)` with `n_in`, `n_out` ≤ CLAP_HOST_PORT_CHANNELS.
- Consumes: Task 1.1's macros.

- [ ] **Step 1:** Write the failing test in `tests/clap_stage_test.c`: `t_aux_bind_wide`: a stage bound with 3 aux
  outputs of 1, 6 and 2 channels over 9 scratch blocks asserts `aout[2].channel_count == 6`,
  `aout[2].data32[5] == scratch[6]`, `aout[3].data32[0] == scratch[7]`; `omx_clap_bind_aux_in` with 2 aux inputs of 4
  and 1 channels and no bound buffers asserts `ains[1].data32[3] == silence`, then `omx_clap_set_aux_in(a, 0, 4, bufs)`
  asserts `ains[1].data32[3] == bufs[3]` and `ains[2].data32[0] == silence`.
- [ ] **Step 2:** `make tests/clap_stage_test` → fails to compile (old signatures).
- [ ] **Step 3:** Make the header changes of the Interfaces block; bump ABI/major/version/map as the Global
  Constraints say; `_Static_assert(sizeof(struct omx_clap_stage) <= 1u << 17, "stage within 128 KiB")` beside the
  existing offset assert (the arrays are ~70 KiB; this pins the growth).
- [ ] **Step 4:** `make test-stage` → PASS (the new test included).
- [ ] **Step 5:** Commit: `core: the port description and the limits of 64 (#92), a new soname major`.

### Task 1.3: read_topology becomes describe_ports; activation judges the lane

**Files:**
- Modify: `src/clap_host.c:582-656` (`read_topology` → `describe_ports`), `:1045-1075` (bounce), `:1086-1092`
  (bind at activate), `:1860-1880` (`omx_clap_host_bind_aux_input`), `src/clap_host.h:695-712` (its doc),
  `tests/fake_plugin.c:114-136`, `tests/test_util.h:37-42`, `tests/clap_host_test.c:400-455`.

**Interfaces:**
- Produces: `static const char *describe_ports(struct omx_clap_instance *in)` (fills the two mains and the aux arrays
  from `clap.audio-ports`: `id`, `name`, `channels`, `kind` from `port_type` (NULL/other → UNKNOWN), `in_place_pair`;
  refuses in this order per port: `channel_count == 0` → PORT_WITHOUT_CHANNELS, `> CLAP_HOST_PORT_CHANNELS` →
  PORT_TOO_WIDE, a second IS_MAIN on a side or the 65th aux → TOO_MANY_PORTS; after the loop: no main output →
  NO_AUDIO_OUTPUT; `main_in.channels != main_out.channels` when both exist → WIDER_THAN_STRIP (dropped in Task 2.1);
  note-input rules unchanged).
  `omx_clap_host_activate`: after the rate/block checks, `if (cfg.lane_channels && (in->main_in.channels > cfg.lane_channels || in->main_out.channels > cfg.lane_channels)) → -1, why = CLAP_HOST_CODE_WIDER_THAN_STRIP`.
  `OMX_CLAP_EXPORT int omx_clap_host_bind_aux_input(struct omx_clap_instance *in, uint32_t port, float *const *bufs, uint32_t channels, uint32_t frames);`
  (`bufs` NULL unbinds; `channels != in->aux_in[port].channels` → -1; the running/frames rules as today).
  `OMX_CLAP_EXPORT int omx_clap_host_bind_aux_output(struct omx_clap_instance *in, uint32_t port, float *const *bufs, uint32_t channels, uint32_t frames);`
  (same rules; the plugin writes the caller's buffers directly while bound, the scratch otherwise).
  Bounce: `make_bounce` maps `4 + 1 + Σ aux_out[i].channels` blocks (two-leg in/out pairs, one silence block, the aux
  scratch), plus what Task 2.2 adds; `in->bounce_blocks` records the count.
- Fakes (`fake_plugin.c`, ids in `test_util.h`): keep `wide` (6→6 main), add `zero-port` (an aux input of 0
  channels) → `FAKE_ZERO_PORT`, `too-wide` (a 65-channel aux output) → `FAKE_TOO_WIDE`, `two-mains` (two IS_MAIN
  outputs) → `FAKE_TWO_MAINS`, `many-aux` (65 mono aux inputs) → `FAKE_MANY_AUX`, `aux-wide` (one aux input of 4
  channels whose process() copies aux channel 3 onto main output channel 0) → `FAKE_AUX_WIDE`. The fake's
  `fake_t` gains `aux_in_channels`, `aux_out_channels`, `two_mains` fields the descriptor table sets.

- [ ] **Step 1:** Write the failing tests in `clap_host_test.c` (`t_layout_description`):
  ```c
  check_refused(path, FAKE_ZERO_PORT, "hosting.topology.port-without-channels");
  check_refused(path, FAKE_TOO_WIDE, "hosting.topology.port-too-wide");
  check_refused(path, FAKE_TWO_MAINS, "hosting.topology.too-many-ports");
  check_refused(path, FAKE_MANY_AUX, "hosting.topology.too-many-ports");
  // FAKE_WIDE opens: main 6/6, kind unknown (its port_type is NULL); activation on the default config refuses
  CHECK(open_ok(path, FAKE_WIDE, &in) && in->main_in.channels == 6 && in->main_out.channels == 6 && in->channels == 6, …);
  CHECK(omx_clap_host_activate(in, 48000, 256, why) == -1 && strcmp(why, "hosting.topology.wider-than-strip") == 0, …);
  // with lane_channels 0 it activates
  cfg.lane_channels = 0; omx_clap_host_configure(&cfg); CHECK(omx_clap_host_activate(in, 48000, 256, why) == 0, …);
  // FAKE_AUX_WIDE: aux_in[0].channels == 4; bound with 4 buffers, buffer 3 = 0.25f, run on silence: out l == 0.25f
  CHECK(omx_clap_host_bind_aux_input(in, 0, bufs, 4, 256) == 0 && omx_clap_host_bind_aux_input(in, 0, bufs, 2, 256) == -1, …);
  ```
  and in `clap_core_test.c:649-660` the over-the-limit side chain expects `"hosting.topology.too-many-ports"`.
- [ ] **Step 2:** `make test-fake` → the new checks fail (FAKE_ZERO_PORT unknown id etc.).
- [ ] **Step 3:** Implement `describe_ports`, the activation judgment, the two bind functions, the bounce; update
  `effects.c` and `scan.c` to the new field names (behaviour unchanged: `effects.c` reads `in->channels`,
  `in->in_channels`; `scan.c:302-309` prints `channels` and now the `kind` name too). In `effects.c`'s `add`, an
  activation refusal answers `resp -102 <code>` exactly as an open refusal does (today it may map to another
  error number: the code must travel, the e2e of Step 4 reads it).
- [ ] **Step 4:** `make test-fake test-core test-scan` → PASS; `make test-jack test-jack-synth` → PASS with
  `jack_e2e.sh:130` and `jack_synth_e2e.sh:179` still expecting `wider-than-strip` (omx-clap-host still configures
  `lane_channels` 2 until Task 2.3).
- [ ] **Step 5:** Commit: `core: every port described, the carrier judged at activation (#92)`.

### Task 1.4: the LV2 adapter's constants and codes follow the core

**Files:**
- Modify: `lv2/core/lv2_core.h:77` (`LV2_CORE_SIDE_LEGS`), `:187-200` (codes), `lv2/core/lv2_world.c:866-980`
  (the refusal of the ninth side chain), `lv2/tests/lv2_host_test.c:307-356`, `lv2/tests/lv2_clap_test.c:468-515`,
  `tests/jack_lv2_e2e.sh:148,169`.

**Interfaces:**
- Produces: `LV2_CORE_AUX_PORTS` 64 (replaces `LV2_CORE_SIDE_LEGS` 8, now a count of ports, and
  `LV2_CORE_SIDE_CHANNELS` 64 for a side chain's channels), `LV2_CODE_TOO_MANY_PORTS`, `LV2_CODE_PORT_TOO_WIDE`;
  `LV2_CODE_EXTRA_INPUTS` gone. A `_Static_assert(LV2_CORE_AUX_PORTS == CLAP_HOST_AUX_INPUTS)` in `lv2/clap/lv2_clap.c`.
  The side-chain arrays in `struct lv2_plugin` are sized by the new constants (legs of all side chains ≤ 64·64 is
  not stored flat: `side_in` becomes `uint32_t side_in[LV2_CORE_AUX_PORTS][LV2_CORE_SIDE_CHANNELS]`; Task 3.1
  replaces the whole set).

- [ ] **Step 1:** In `lv2_host_test.c:312-313` expect `SC_URI "#extra"` ADMITTED (`p->n_side` = the Turtle's count)
  and `SC_URI "#wide"` admitted with `side_channels[0] == 3`; the ninth-side-chain refusal becomes a 65th (a new
  twin `#sixty-five` in `omx-sidechain-fixture.ttl` with 65 `lv2:isSideChain` mono ports: generate the Turtle block
  with a shell loop into the file, committed) → `"hosting.topology.too-many-ports"`; mirror in `lv2_clap_test.c:476`.
- [ ] **Step 2:** `make test-lv2-core test-lv2-clap` → red on the three expectations.
- [ ] **Step 3:** Implement: the constants, the codes, the sized arrays, `read_inputs`' refusals (`> 2` channels per
  side chain → nothing; `> LV2_CORE_SIDE_CHANNELS` → PORT_TOO_WIDE; the 65th → TOO_MANY_PORTS); the shim's
  `ports_get` passes `side_channels[k]` through with `port_type` NULL above 2.
- [ ] **Step 4:** `make test-lv2` → PASS; `make test-jack-lv2` → PASS (line 148's `#wide` still refused
  `wider-than-strip` by `read_ports`' `nout != p->legs` until Task 3.2; line 169 unchanged until Task 2.3).
- [ ] **Step 5:** Commit: `lv2: the side chains up to 64 ports of 64 channels, the ninth no longer a refusal (#92)`.

### Task 1.5: two mains on a side (Review Focus 1)

Covered by `FAKE_TWO_MAINS` in Task 1.3. Add the LV2 twin: `pg:mainInput` naming a group AND a second ungrouped
input outside it stays rule 1 (aux), so nothing to refuse on LV2; note this in the test's comment.

### Task 1.6: the bounce at the limits (Review Focus 2)

- [ ] **Step 1:** Test in `clap_host_test.c` (`t_bounce_at_limits`) with a fake `aux-max` (64 aux inputs × 64
  channels and 64 aux outputs × 64 channels, process() copies nothing): activate at `max_block` 4096 →
  `in->bounce_blocks == 4 + 1 + 64*64` and `in->bounce_map_len >= bounce_blocks * 4096 * sizeof(float)`; run 64
  blocks under the test's allocation guard (`test_util.h` has one if present; otherwise `mallopt`-free: count
  `malloc` through the existing `g_alloc_calls` hook of `clap_host_test.c` if it exists, else add a `--wrap=malloc`
  counter to this test's link line) → zero allocations inside.
- [ ] **Step 2:** Run, see red (fake missing), implement the fake, run green, commit:
  `core: the bounce at the limits, allocated once (#92)`.

### Task 1.7: PR 1 gate

- [ ] `make abi-baseline` (records `abi/libomx-clap-core.so.1.*`), remove `abi/libomx-clap-core.so.0.*`, `make abi-check` → PASS.
- [ ] `make test test-lv2 test-jack test-jack-synth test-jack-lv2 test-identity test-identity-lv2 test-contract OMX_CONTRACT_DIR=…` → all PASS.
- [ ] `clap-core-lane.sh check <wt>` OK. Push; PR titled `core: the port description, limits of 64 and the codes (#92, 1/6)`,
  body with `Codes: hosting.topology.too-many-ports hosting.topology.port-too-wide hosting.topology.port-without-channels (retired: hosting.topology.extra-inputs-fed-silence)`.

---

## PR 2 — the main pair of different widths and `run_ports`

Codes: none new (`wider-than-strip` moves to activation, done in PR 1).

### Task 2.1: mono-in/stereo-out and stereo-in/mono-out on the two-leg lane

**Files:** `src/clap_host.c` (`describe_ports`: drop the `main_in != main_out` refusal), `tests/fake_plugin.c`
(fakes `mono-stereo`: main in 1, out 2, process() writes in[0] to out[0] and -in[0] to out[1]; `stereo-mono`: in 2,
out 1, process() writes (in[0] + in[1]) to out[0]) → `FAKE_MONO_STEREO`, `FAKE_STEREO_MONO`; `tests/clap_host_test.c`.

- [ ] **Step 1:** Test `t_main_widths`: open both; `in_channels == 1 && channels == 2` and `2/1`; activate, publish,
  `omx_clap_host_run(in, l, r, 256)` with `l` = 0.5f, `r` = 0.25f constant: mono-stereo → `l[i] == 0.5f`,
  `r[i] == -0.5f` (the right leg is the plugin's second output, not the dry right); stereo-mono → `l[i] == 0.75f &&
  r[i] == 0.75f` (Review Focus 3: the mono output on both legs). Bypassed (`omx_clap_host_bypass(in, 1)` after a
  crossfade's worth of blocks): both legs dry.
- [ ] **Step 2:** Run → red (refused at open). **Step 3:** drop the refusal (one `if`). **Step 4:** `make test-fake` → PASS.
- [ ] **Step 5:** Commit: `core: the main pair may differ in width; a mono output is carried to both legs (#92)`.

### Task 2.2: `omx_clap_run_ports` and `omx_clap_host_run_ports`

**Files:** `src/clap_stage.h` (after `omx_clap_run_io`), `src/clap_host.h` (after `omx_clap_host_run_io`),
`src/clap_host.c` (`make_bounce`: when either main is wider than 2, `main_in.channels + main_out.channels` more
blocks, `in->stage.wide_in[c]`/`wide_out[c]` point at them; `omx_clap_bind_ports_aux` then points `main_in->data32`
at `wide_in` and `aout[0].data32` at `wide_out`), `tests/clap_stage_test.c`, `tests/clap_host_test.c`.

**Interfaces:**
- Produces: in the stage, `float *wide_in[CLAP_HOST_PORT_CHANNELS], *wide_out[CLAP_HOST_PORT_CHANNELS]; uint32_t wide;`
  (`wide` 1 when the wide bounce is bound);
  `static inline void omx_clap_run_ports(struct omx_clap_stage *s, const float *const *ins, float *const *outs, uint32_t n)`:
  when `!s->wide` → `omx_clap_run_io(s, ins, outs, n)` (the two-leg path, bit-identical); when wide → the state
  transitions of `omx_clap_run_locked` (ARMED→PROCESSING with `start_processing`, STOPPING→STOPPED, not
  PROCESSING → dry: `outs[c] = ins[min(c, n_in-1)]`, silence for a source), then: copy `ins[c]` into `wide_in[c]`
  for `c < h.n_in`, `omx_clap_run_plugin(s, n)`, per output channel `omx_hosted_clamp` under `OMX_HOSTED_GUARD_CLAMP`
  (counted in `clamped_samples`), `omx_block_finite` under `GUARD_NONFINITE` (a non-finite block delivers dry and
  strikes as `omx_hosted_run_end_why` does), then `outs[c] = wide_out[c]` when wet, dry when the commanded bypass is
  on. **Decision:** the wide path switches bypass hard, no crossfade (the two-leg core's crossfade is two-leg; the
  only wide carrier is omx-clap-host under JACK).
  `static inline void omx_clap_host_run_ports(struct omx_clap_instance *in, const float *const *ins, float *const *outs, uint32_t n)`:
  the transport record as `omx_clap_host_run` builds it, then `omx_clap_run_ports`; when the stage is not wide it
  calls `omx_clap_io_lane` + `omx_clap_host_run` so the transport reaches the two-leg path unchanged.
  `omx_clap_host_activate` refuses `wider-than-strip` by `lane_channels` BEFORE binding the wide bounce (so a
  two-leg caller never allocates it).

- [ ] **Step 1:** Tests: `clap_stage_test.c` `t_run_ports_wide`: a raw 6→6 fake stage (the test file's own plugin
  shim, as the other stage tests build one) bound wide, `run_ports` with six input blocks of value `c * 0.1f` →
  output `c` equals the fake's gain × input; bypass commanded → `outs[c] == ins[c]` the next block, no fade.
  `clap_host_test.c` `t_run_ports`: `FAKE_WIDE` with `lane_channels` 0, activate, publish, `omx_clap_host_run_ports`
  on six buffers → values as above and `instance->stage.wide == 1`; `FAKE_MONO_STEREO` through `run_ports` with
  `ins[1]`, `outs[2]` → `wide == 0` and the Task 2.1 values (the two-leg path).
- [ ] **Step 2:** Run → red. **Step 3:** implement. **Step 4:** `make test-stage test-fake` → PASS.
- [ ] **Step 5:** Commit: `core: run_ports carries a main of any width; the two-leg lane keeps its path (#92)`.

### Task 2.3: omx-clap-host carries any width as ports

**Files:** `src/effects.c:125-140` (port arrays sized `CLAP_HOST_PORT_CHANNELS`), `:340-401` (process: `run_ports`),
`:1388-1415` (registration loops read `main_in.channels`/`main_out.channels`), `src/main.c` (the host's config:
`lane_channels = 0`), `tests/jack_e2e.sh:130-131`, `tests/jack_synth_e2e.sh:179-180`.

- [ ] **Step 1:** `jack_e2e.sh`: `add clap:$fake#org.omx-clap-host.test.wide 1` → `resp 1`; `ports effect_1 i` =
  `in_1 … in_6`, `o` = `out_1 … out_6`; `jack_meter_source` (0.5) on `effect_1:in_1` and the probe on `out_1`
  reads the fake's default gain applied, as the stereo step of the same file reads it; `jack_synth_e2e.sh`:
  `synth-wide` → `resp 2`, `out_1 … out_6`. Also add the mono-stereo fake: `add … mono-stereo` → ports `in_1`,
  `out_1 out_2`, 0.5 on `in_1` read as 0.5 on `out_1` and -0.5 on `out_2`.
- [ ] **Step 2:** Run the two e2e → red. **Step 3:** implement (the not-active dry loop keeps
  `inputs[min(c, in_channels-1)]`). **Step 4:** `make test-jack test-jack-synth test-identity` → PASS.
- [ ] **Step 5:** Commit: `host: a main of any width is a JACK port per channel (#92)`.

### Task 2.4: PR 2 gate

- [ ] Full gate as 1.7 (no ABI change: `make abi-check` PASS against the new baseline). PR
  `core, host: the main pair of different widths and run_ports (#92, 2/6)`, `Codes: none (wider-than-strip now at activation)`.

---

## PR 3 — LV2 rules 1–5 and the group-type table

Codes: none new (the adapter's `LV2_CODE_WIDER_THAN_STRIP` is removed).

### Task 3.1: the adapter's port model

**Files:** `lv2/core/lv2_core.h:70-112` (constants), `:185-215` (`struct lv2_plugin`), `lv2/core/lv2_instance.c:1125-1260`
(buffers and `connect_port`), `:1584-1610` (accessors), `lv2/clap/lv2_clap.c:450-510` (`ports_count/get`),
`:780-900` (`plugin_process`).

**Interfaces:**
- Produces, in `lv2_core.h`:
  ```c
  #define LV2_CORE_PORT_CHANNELS 64
  #define LV2_CORE_AUX_PORTS 64
  enum lv2_port_kind { LV2_PORT_UNKNOWN = 0, LV2_PORT_MONO, LV2_PORT_STEREO, LV2_PORT_SURROUND, LV2_PORT_AMBISONIC };
  struct lv2_audio_port
  {
      uint32_t n;                                  // channels, 0 for an absent main
      uint32_t port[LV2_CORE_PORT_CHANNELS];       // the plugin's port index of each channel, in map order
      char *name;                                  // the group's label/name, else the port's name
      uint32_t kind;                               // an lv2_port_kind
      uint8_t map[LV2_CORE_PORT_CHANNELS];         // CLAP surround ids for a surround kind; ACN indices for ambisonic
  };
  ```
  `struct lv2_plugin`: `struct lv2_audio_port main_in, main_out; uint32_t n_aux_in, n_aux_out; struct lv2_audio_port *aux_in, *aux_out;`
  (calloc'd `LV2_CORE_AUX_PORTS` each at open; freed at close); `legs` = `main_out.n`, `in_legs` = `main_in.n` kept
  as the names the instance and the shim read; `in_ports[2]`/`out_ports[2]`, `n_side*`, `side_*` removed; the CV
  arrays unchanged. `passthrough` unchanged.
  Instance: `float *lv2_instance_audio_in(in, c)` / `audio_out(in, c)` for `c < main.n`;
  `float *lv2_instance_aux_in(struct lv2_instance *in, uint32_t port, uint32_t c)`, `lv2_instance_aux_out(...)`
  (`lv2_instance_side_in` removed); the audio block is `(main_in.n + main_out.n + Σaux_in.n + Σaux_out.n + n_cv_in + n_cv_out) * max_frames` floats.
  Shim: inputs = main (if `main_inputs()`), then `aux_in` ports, then CV in; outputs = main, `aux_out` ports, CV out;
  `port_type` from kind (mono/stereo/`CLAP_PORT_SURROUND`/`CLAP_PORT_AMBISONIC`, NULL for unknown); `process()`
  copies per port and channel, silence for an input the host hands nothing.

- [ ] **Step 1:** Rewrite `t_sidechains` in `lv2_host_test.c:307-356` and `t_sidechain` in `lv2_clap_test.c:1827-1900`
  against the new fields (`p->n_aux_in == 1 && p->aux_in[0].n == 2 && p->aux_in[0].port[0] == 4 && strcmp(p->aux_in[0].name, "Sidechain") == 0`),
  same values through `process()`.
- [ ] **Step 2:** `make test-lv2-core test-lv2-clap` → fails to compile. **Step 3:** implement the model with the
  rules as they are today (main = ≤2 ungrouped or the `pg:mainInput` group; side chains → aux_in) so this commit
  changes shape, not admission. **Step 4:** `make test-lv2 test-jack-lv2 test-identity-lv2` → PASS, identical.
- [ ] **Step 5:** Commit: `lv2: the audio ports as a description, main and auxiliary each side (#92)`.

### Task 3.2: rules 1–5 per side

**Files:** `lv2/core/lv2_world.c:866-1000` (`read_inputs` → `read_side(p, pl, is_input)`, `read_ports`' output
block), `lv2/core/lv2_core.h` (`LV2_CODE_WIDER_THAN_STRIP` removed), new fixture
`lv2/tests/fixtures/lv2_layout_fixture.c` + `omx-layout-fixture.lv2/{manifest,omx-layout-fixture}.ttl`, `Makefile`
(`LV2_BUNDLES` + rule), `lv2/tests/lv2_host_test.c`, `lv2/tests/lv2_clap_test.c`, `tests/jack_lv2_e2e.sh:148`.

**Interfaces:**
- Produces: `static const char *read_side(struct lv2_plugin *p, const LilvPlugin *pl, int is_input)`: rules 1–5 of
  the spec, in words: main group declared → it is the main, every other audio port aux (grouped: one aux per group in
  first-member order; ungrouped: mono aux each); no main group but groups → first group main; no groups → ≤2 ports
  main; `n > 2 && n == other side's count` → one main of n; else first two main, the rest mono aux; side-chain ports
  aux whatever rule; CV untouched. Refusals: a port beyond `LV2_CORE_PORT_CHANNELS` in a group → PORT_TOO_WIDE; the
  65th aux → TOO_MANY_PORTS. `read_ports` calls `read_side` for inputs then outputs (the output rule needs the input
  count for rule 3, so inputs first; rule 3's "other side's count" for the INPUT side is the output port count,
  computed before either).
- Fixture twins (URIs `urn:openmixer:test:layout-fixture#<name>`; every one's `run()` is a fixed arithmetic so a
  value proves the wiring): `main-aux` (main group 2 + two stereo aux groups: out_c = main_c + aux1_c + aux2_c),
  `two-mono` (two mono groups, no main: out = a + b), `four-four` (ungrouped 4→4: out_c = in_c), `four-two` (4→2:
  out_c = in_c + in_{c+2}), `mono-stereo` (1 in, `pg:mainOutput` stereo group: out_l = in, out_r = -in), `two-one`
  (2→1: out = l + r), `bands` (main pair + three stereo band groups: band_k = in · (k+1)/4), `sixty-four` (64
  ungrouped mono inputs, 1 output = Σ), `sixty-five` (65 → refused). The Turtle for the 64/65 twins is generated
  by a shell loop and committed.

- [ ] **Step 1:** Tests, `lv2_host_test.c` `t_layouts`: per twin the description (`main_in.n`, `main_out.n`,
  `n_aux_in`, `aux_in[k].n`, `aux_in[k].port[0]`, names), `sixty-five` → `"hosting.topology.too-many-ports"`;
  `FIXTURE_URI "#wide"` (host fixture, 1→2) now admitted `main_in.n == 1 && main_out.n == 2`; `#three-in` (3→2)
  → `main_in.n == 2 && n_aux_in == 1`. `lv2_clap_test.c` `t_layouts`: through `process()` on `four-two` with
  in = {0.1,0.2,0.3,0.4} → out {0.4, 0.6}; on `main-aux` with the second aux handed {0.5,0.5} and the first not →
  out = main + 0.5. `jack_lv2_e2e.sh:148`: `#wide` → `resp 2`, ports `in_1` / `out_1 out_2`.
- [ ] **Step 2:** red. **Step 3:** implement. **Step 4:** `make test-lv2 test-jack-lv2 test-identity-lv2 test-lv2-validate` → PASS.
- [ ] **Step 5:** Commit: `lv2: which ports are main and which auxiliary, by the port-group rules (#92)`.

### Task 3.3: a main group nobody belongs to (Review Focus 4)

- [ ] Twin `empty-main` (`pg:mainInput` names a group no port carries `pg:group` for; two ungrouped inputs):
  expected rule 3 → `main_in.n == 2`, no aux, no crash; test in `t_layouts`; commit with 3.2 or alone:
  `lv2: a main group without members is no main (#92)`.

### Task 3.4: the group-type table

**Files:** `lv2/core/lv2_world.c` (`group_kind(const char *type_uri, uint32_t *kind, const uint8_t **order, uint32_t *n)`:
the spec's table, element order = `port-groups.ttl`'s `pg:element` order, members ordered by their
`lv2:designation` matching the elements; a member without one, or a type outside the table → index order, kind
unknown), fixture twins `five-one` (`pg:FivePointOneGroup` 6→6, designations `pg:left pg:center pg:right pg:rearLeft pg:rearRight pg:lowFrequencyEffects` declared in a scrambled port order; out_c = in_c per designation), `ambi-first`
(`pg:AmbisonicBH1P1Group` 4→4, ACN designations), `ambi-mixed` (`pg:AmbisonicBH2P1Group` 6→6).

- [ ] **Step 1:** Tests: `five-one` → `main_in.kind == LV2_PORT_SURROUND`, `map` = {FL, FC, FR, BL, BR, LFE} as CLAP
  ids {0, 2, 1, 4, 5, 3} (`CLAP_SURROUND_FL`=0, `FR`=1, `FC`=2, `LFE`=3, `BL`=4, `BR`=5), `port[]` in that order;
  `ambi-first` → `LV2_PORT_AMBISONIC`, `map` = {0,1,2,3}; `ambi-mixed` → `LV2_PORT_UNKNOWN`, n 6, ACN designations
  in `map`. Through the shim: `port_type` strings `CLAP_PORT_SURROUND` / `CLAP_PORT_AMBISONIC` / NULL.
- [ ] **Step 2:** red. **Step 3:** implement. **Step 4:** `make test-lv2` → PASS.
- [ ] **Step 5:** Commit: `lv2: port-group types give a port its kind and channel map (#92)`.

### Task 3.5: PR 3 gate

- [ ] `docs/lv2-compatibility.md:67-79`: rows for side chains, "any other layout" (now supported: the rules, the
  table, the twins by name), port groups (all groups read, typed groups mapped) — only what the tests prove.
- [ ] Full gate; PR `lv2: every audio layout described by the port-group rules (#92, 3/6)`, `Codes: none`.

---

## PR 4 — surround and ambisonic through the CLAP extensions

Codes: `hosting.topology.channel-map-unknown`, `hosting.topology.ambisonic-config` (added to the sheet here;
`ambisonic-config` is raised only in PR 5 and the sheet's doc says so).

### Task 4.1: the sheet

- [ ] As Task 1.1 for the two codes: `channelMapUnknown` ("a surround port names a channel id the host does not know,
  or refuses the carrier's channel mask"), `ambisonicConfig` ("an ambisonic port refuses the carrier's ordering or
  normalisation"); re-render; `test-contract` PASS; commit `contract: the surround and ambisonic codes (#92)`.

### Task 4.2: the description reads the two extensions

**Files:** `src/clap_host.c` (`open_from`: `in->surround = get_extension(CLAP_EXT_SURROUND)` falling back to
`CLAP_EXT_SURROUND_COMPAT`, `in->ambisonic` likewise; `describe_ports`: for `kind == SURROUND`, `get_channel_map`
into `map` (count ≠ channels or an id > `CLAP_SURROUND_TSR` → CHANNEL_MAP_UNKNOWN); for `AMBISONIC`, `get_config` →
`ambisonic_ordering/normalization`, `(order+1)² != channels` → PORT_TOO_WIDE, where `order` is derived from
`channels` as the integer root minus one; `host_get_extension` answers `clap_host_surround_t{ changed }` and
`clap_host_ambisonic_t{ changed }` both setting `ports_rescan_requested`), `src/clap_host.h` (the two extension
pointers appended to the instance), `tests/fake_plugin.c` (fakes `surround`: 6-ch mains, map {FL FR FC LFE BL BR};
`surround-bad`: an id 25; `ambisonic`: 4-ch mains, ACN/SN3D; `ambisonic-bad`: 5 channels), `doc/reference.md`
(extension table rows: `clap.surround/4`, `clap.ambisonic/3`, what is read and when).

- [ ] **Step 1:** Tests `t_surround_ambisonic`: `FAKE_SURROUND` opens, `main_out.kind == OMX_CLAP_PORT_SURROUND`,
  `map[3] == CLAP_SURROUND_LFE`; `check_refused(path, FAKE_SURROUND_BAD, "hosting.topology.channel-map-unknown")`;
  `FAKE_AMBISONIC` → `kind == AMBISONIC`, `ambisonic_ordering == CLAP_AMBISONIC_ORDERING_ACN`,
  `ambisonic_normalization == CLAP_AMBISONIC_NORMALIZATION_SN3D`; `check_refused(path, FAKE_AMBISONIC_BAD, "hosting.topology.port-too-wide")`;
  the fake calling the host's `surround.changed` → `omx_clap_host_tick` reports a ports rescan.
- [ ] **Step 2:** red. **Step 3:** implement. **Step 4:** `make test-fake test-scan` → PASS (`scan.c` prints the map ids).
- [ ] **Step 5:** Commit: `core: surround and ambisonic ports described through their extensions (#92)`.

### Task 4.3: the LV2 shim offers the two extensions

**Files:** `lv2/clap/lv2_clap.c` (`get_extension`: `CLAP_EXT_SURROUND` when any port's kind is surround →
`get_channel_map` from `map`, `is_channel_mask_supported` true when the mask equals the port's own; `CLAP_EXT_AMBISONIC`
when any is ambisonic → `get_config` ACN + SN3D, `is_config_supported` true for exactly that), `lv2/tests/lv2_clap_test.c`,
`docs/lv2-compatibility.md` (the row says SN3D is assumed: LV2 declares none).

**Decision (deviation from the spec's "the description says normalisation: assumed"):** CLAP carries no such mark
and the core sees only CLAP, so the assumption is written in `docs/lv2-compatibility.md` and in the shim's comment,
not in `struct omx_clap_port`.

- [ ] **Step 1:** Test through the core (`lv2_clap_test` `t_layouts_kinds`): open `five-one` through
  `omx_clap_host_open` with the shim's entry → `main_in.kind == SURROUND`, `map` {0,2,1,4,5,3}; `ambi-first` →
  AMBISONIC, ACN, SN3D.
- [ ] **Step 2:** red. **Step 3:** implement. **Step 4:** `make test-lv2` PASS. **Step 5:** Commit:
  `lv2: a typed group is a surround or ambisonic CLAP port (#92)`.

### Task 4.4: PR 4 gate

- [ ] Full gate; PR `core, lv2: surround and ambisonic layouts (#92, 4/6)`,
  `Codes: hosting.topology.channel-map-unknown hosting.topology.ambisonic-config`.

---

## PR 5 — `request_layout`

Codes: none new (`channel-map-unknown` and `ambisonic-config` gain their `request_layout` arm).

### Task 5.1: the call

**Files:** `src/clap_host.h`, `src/clap_host.c`, `src/omx-clap-core.map` (`omx_clap_host_request_layout` in the
latest node), `tests/fake_plugin.c` (fakes `configs`: two `audio-ports-config`s, id 1 mono-in/stereo-out and id 2
stereo/stereo, declared layout id 1; `configurable`: `configurable-audio-ports` accepting any main of 1 or 2 each
side; `surround-refuses`: a surround 6-ch main whose `is_channel_mask_supported` answers false for every mask but
its own), `tests/clap_host_test.c`.

**Interfaces:**
- Produces:
  ```c
  #define OMX_CLAP_LAYOUT_DECLARED 1   ///< request_layout: the plugin cannot offer it; its declared layout stands
  OMX_CLAP_EXPORT int omx_clap_host_request_layout(struct omx_clap_instance *in, const clap_audio_port_configuration_request_t *reqs, uint32_t n, char why[OMX_CLAP_WHY_MAX]);
  ```
  0 = applied and the description re-read (`describe_ports` again; its refusals return -1 with their code);
  `OMX_CLAP_LAYOUT_DECLARED` = no extension offers it, or `can_apply_configuration`/the config list has no match,
  or `apply_configuration`/`select` answered false; -1 = active instance, or the plugin refused the carrier's mask
  (`channel-map-unknown`) or ambisonic config (`ambisonic-config`) through `is_channel_mask_supported` /
  `is_config_supported` for a request whose `port_type` is surround/ambisonic. Order: `configurable-audio-ports`
  first, then `audio-ports-config` (a config matches when `input_port_count/output_port_count`, the main channel
  counts and the main port types equal the requests for the mains), then DECLARED.

- [ ] **Step 1:** Tests `t_request_layout`: `FAKE_CONFIGS` declared 1→2; request {in 2 stereo, out 2 stereo} → 0 and
  `in_channels == 2`; request {in 6 surround} → DECLARED, description unchanged; `FAKE_CONFIGURABLE` request {in 1,
  out 2} → 0, `main_in.channels == 1`; `FAKE_WIDE` (no extension) → DECLARED; `FAKE_SURROUND_REFUSES` request with a
  5.1 mask → -1, `why == "hosting.topology.channel-map-unknown"`; after activate → -1 and nothing applied
  (Review Focus 5); a `configurable` fake whose `apply_configuration` returns false → DECLARED (Review Focus 5).
- [ ] **Step 2:** red. **Step 3:** implement. **Step 4:** `make test-fake` PASS; `tests/exports.sh` lists the symbol.
- [ ] **Step 5:** Commit: `core: request_layout asks a plugin for the carrier's layout (#92)`.

### Task 5.2: omx-clap-host asks for the lane's width

**Files:** `src/main.c` (option `-c <channels>` / `--channels`, default 2, values 1 or 2), `src/effects.c` (`add`:
when the declared mains differ from `-c`, request {in c, out c, mono/stereo type} before `omx_clap_host_activate`; a
DECLARED answer carries on with the declared layout), `tests/jack_e2e.sh` (`add … configs` under the default → ports
`in_1 in_2` / `out_1 out_2` (config 2 selected); under `-c 1` the request is 1/1, the configs fake offers no 1/1 →
DECLARED → the declared 1→2 stands: `in_1` / `out_1 out_2`), `doc/reference.md` (the two extensions' rows: asked
only by `request_layout`).

- [ ] **Step 1:** the e2e expectations above. **Step 2:** red. **Step 3:** implement. **Step 4:** `make test-jack` PASS.
- [ ] **Step 5:** Commit: `host: the lane's width is asked of a plugin that offers layouts (#92)`.

### Task 5.3: PR 5 gate

- [ ] Full gate; PR `core, host: request_layout (#92, 5/6)`, `Codes: none new (channel-map-unknown, ambisonic-config at request time)`.

---

## PR 6 — omx-clap-host aux ports, the scan, docs, sabotage, coverage

### Task 6.1: aux ports under JACK

**Files:** `src/effects.c` (`effect_t`: `jack_port_t *aux_in_ports[CLAP_HOST_AUX_INPUTS * CLAP_HOST_PORT_CHANNELS]`,
`aux_out_ports[…]`, `float *aux_in_buf`, `aux_out_buf` (one block per aux channel, allocated at `add` for the JACK
buffer size, re-allocated in the buffer-size callback where the main bounce is); at `add`: register
`aux_in_<p>_<k>`/`aux_out_<p>_<k>` and bind each port's channel buffers with `omx_clap_host_bind_aux_input/output`;
`process()`: copy each JACK aux-in buffer into its bound block before `run_ports`, each bound aux-out block onto its
JACK port after), `tests/jack_e2e.sh`, `tests/jack_lv2_e2e.sh`.

**Decision:** one copy per aux channel per cycle (JACK buffers move; the binding does not).

- [ ] **Step 1:** e2e: `add … sidechain` → `aux_in_1_1` listed; `jack_meter_source` on `aux_in_1_1`, silence on
  the mains → the probe on `out_1` reads the fake's side-chain contribution (the sidechain fake adds it; assert the
  value the stereo e2e knows); `add … auxout` → `aux_out_1_1 aux_out_1_2` listed and the probe on `aux_out_1_1`
  reads the fake's aux output value. LV2: `add lv2:…layout-fixture#main-aux` → `aux_in_1_1 aux_in_1_2 aux_in_2_1 aux_in_2_2`;
  source on `aux_in_2_1` heard on `out_1`; `#bands` → `aux_out_1_1 … aux_out_3_2`, the probe on `aux_out_2_1`
  reads in · 2/4.
- [ ] **Step 2:** red. **Step 3:** implement. **Step 4:** `make test-jack test-jack-lv2 test-identity test-identity-lv2` PASS.
- [ ] **Step 5:** Commit: `host: a JACK port per auxiliary channel, bound to the plugin (#92)`.

### Task 6.2: the scan and the pin's neighbour verb print the description

**Files:** `src/scan.c:295-312` (per port: `role`, `channels`, `kind`, `map` as ids, `name`; the text form one line
per port `main|aux <n> channels <kind> [map …] "<name>"`), `src/layout_pin.h` and the neighbour verb (the same
names as the JACK ports), `tests/clap_scan_test.c`, `tests/jack_pin_e2e.sh`.

- [ ] **Step 1:** `clap_scan_test`: the surround fake's JSON carries `"kind":"surround","map":[0,1,2,3,4,5]` and the
  aux-wide fake `"role":"aux","channels":4`; the pin e2e lists `aux_in_1_1` for the sidechain fake.
- [ ] **Step 2–5:** red, implement, PASS, commit `scan, pin: the description printed with its JACK names (#92)`.

### Task 6.3: sabotage

**Files:** new `tests/layout_sabotage.sh`, `Makefile` target `test-layouts-sabotage` (in `test`'s list beside
`test-transport-sabotage`).

Each arm copies `src lv2 tests data Makefile VERSION` into `build/sabotage-<arm>/`, applies one `sed`, asserts the
file changed (`! cmp`), builds the named test there with `make -C build/sabotage-<arm> <target>` and asserts it
exits non-zero. Arms:
1. `widths`: re-insert `if (in->main_in.channels && in->main_in.channels != in->main_out.channels) return CLAP_HOST_CODE_WIDER_THAN_STRIP;`
   after `describe_ports`' main loop → `tests/clap_host_test` red (Task 2.1's test).
2. `nn-main`: in `read_side`, the rule-3 condition `n > 2 && n == other` → `0` → `lv2/tests/lv2_host_test` red (`four-four`).
3. `aux-silence`: `omx_clap_set_aux_in`'s `bufs ? bufs[c] : silence` → `silence` → `tests/clap_host_test` red (Task 1.3's bound-aux value).

- [ ] **Step 1:** write the script with the three arms; `make test-layouts-sabotage` → each arm reports `red as
  expected`. A surviving arm fails the target. **Step 2:** commit `tests: three sabotage arms for the layouts (#92)`.

### Task 6.4: docs

- [ ] `docs/lv2-compatibility.md:152` (the adapter's refusals list: the codes as they now are), `doc/reference.md`
  (host options `-c`; the aux port names; the extension rows), `README.md` only if it lists refusal codes. Commit
  `docs: the layouts, the aux ports, the codes (#92)`.

### Task 6.5: coverage and the PR

- [ ] `clap-core-lane.sh coverage <wt> g92` (ops `ops/clap-core-lane` branch) → expected 907 accepted of 975;
  write the delta (plugin by plugin, with the rule that admitted each) into the PR's first comment, and what remains
  below mod-host's 915 with its code.
- [ ] Full gate; PR `host, docs: auxiliary ports, the scan, sabotage and coverage (#92, 6/6)`, `Codes: none`.

### Task 6.6: plugin-hostd e2e (separate repository, after PR 6 merges)

- [ ] In a plugin-hostd worktree: `tests/jack_e2e.sh` scenario: `add` the sidechain fake, `connect` a source to
  `effect_N:aux_in_1_1` by name, read the output change; its own PR, `Codes: none`.

---

## Self-review notes

- Spec coverage: model → 1.2/1.3/3.1; limits+codes → 1.1/4.1; main widths → 2.1; run_ports → 2.2/2.3; bounce →
  1.3/1.6/2.2; bind aux in/out → 1.3/6.1; LV2 rules → 3.2/3.3; group table → 3.4/4.3; CLAP extensions → 4.2;
  request_layout → 5.1/5.2; JACK names → 2.3/6.1; scan/pin → 6.2; codes table → 1.1/4.1; proof list → the tests
  named per task, sabotage 6.3, coverage 6.5, bit-identity in every gate; lane_channels → 1.2/1.3/2.3; new major → 1.2/1.7.
- Deviation recorded: the `assumed` normalisation mark lives in the docs, not the struct (Task 4.3).
- Not in this plan (its own issue): a native CLAP plugin with aux outputs and no main output (#211).
