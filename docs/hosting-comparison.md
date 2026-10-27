# Our LV2 adapter against mod-host and jalv

Is the LV2 adapter in `lv2/` overcomplicated, or about the same as the two hosts it is most often compared with? This
report measures all three and maps the same six tasks through each. Every number comes from `tools/hosting-compare.sh`
and every claim cites a file:line.

## Subjects

| subject | version | what is measured |
|---|---|---|
| ours | omx-clap-core `origin/main` 092d172 (`lv2/` is identical to 07d70a1) | `lv2/core` + `lv2/clap` (the adapter); "ours+plumbing" adds the six `src/` headers `lv2/` includes (`omx_msgring.h`, `hosted_stage.h`, `param_contract.h`, `omx_clap_midi.h`, `clap_host_limits.h`, `omx_clap_ext.h`) |
| mod-host | mod-audio/mod-host f14a230 (2025-12-11, the audinux build) and main 12335c2 | `src/*.[ch]`, without the socket protocol, the command line (`mod-host.c`), the readline completer and the monitor client; the vendored `zix/`, `sha1/`, `rtmempool/`, `dsp/` and `fake-input/` subdirectories are not counted |
| jalv | drobilla/jalv v1.10.0 (a0f0162, 2026-06-11), the latest release | `src/*.[ch]`, without `main.c` and the JACK and PortAudio backends; the gtk/qt/console frontends live in subdirectories and are not counted |

The two hosts are applications with one audio backend (JACK) and a UI or socket front. Ours is a library: it presents
one LV2 plugin as a CLAP plugin and has no backend. Measuring host cores without their fronts and backends keeps the
three comparable. mod-host main differs from f14a230 by 26 lines, all in `effects_remove()`
(`git diff --stat f14a230 12335c2 -- src`: `src/effects.c` only). None of the six task paths below touch them. The
mod-host file:line references below are at f14a230.

To reproduce: `tools/hosting-compare.sh` clones the subjects into `$HC_DIR` (never vendored) and prints every table
below. It needs scc (3.5.0 here) and lizard (1.24.1); `--verbose` lists the area each function was put in.

## 1. Measured size and complexity

### Size

| subject | files | lines | code | comments | functions | external deps | public functions |
|---|---:|---:|---:|---:|---:|---:|---:|
| ours | 8 | 7657 | 5648 | 1160 | 273 | 4 | 90 |
| ours+plumbing | 14 | 9224 | 6591 | 1609 | 316 | 5 | 90 |
| mod-host f14a230 | 20 | 11248 | 8164 | 1341 | 165 | 4 | 84 |
| mod-host main | 20 | 11274 | 8182 | 1345 | 165 | 4 | 84 |
| jalv | 47 | 5492 | 3970 | 631 | 127 | 5 | 90 |

External dependencies are the distinct `<lib/...>` include roots: ours `clap lilv lv2 omxcontract` (+`omxdsp` with
plumbing); mod-host `jack lilv lv2 mach`; jalv `lilv lv2 serd sratom zix`. Public functions are the non-`static`
functions defined in a `.c` file.

### Cyclomatic complexity (lizard, per function)

| subject | functions | mean CCN | p95 CCN | max CCN | CCN > 15 | mean NLOC |
|---|---:|---:|---:|---:|---:|---:|
| ours | 273 | 6.38 | 19 | 86 | 20 | 17.2 |
| ours+plumbing | 316 | 6.60 | 19 | 86 | 26 | 17.2 |
| mod-host f14a230 | 165 | 9.08 | 28 | 127 | 18 | 40.9 |
| mod-host main | 165 | 9.12 | 28 | 127 | 18 | 41.0 |
| jalv | 127 | 3.62 | 9 | 21 | 1 | 19.4 |

Top 15 by CCN (CCN, NLOC, function, file:lines):

| ours | mod-host f14a230 | jalv |
|---|---|---|
| 86 154 `instance_make` lv2_instance.c:1235-1398 | 127 736 `effects_add` effects.c:4541-5455 | 21 150 `jalv_open` jalv.c:878-1081 |
| 62 113 `plugin_process` lv2_clap.c:1123-1252 | 122 440 `ProcessPlugin` effects.c:1707-2288 | 14 41 `jalv_process_port_init` process_setup.c:174-229 |
| 57 173 `read_ports` lv2_world.c:1209-1401 | 70 307 `effects_remove` effects.c:5580-5968 | 11 59 `apply_ui_events` process.c:47-116 |
| 50 79 `read_side` lv2_world.c:1120-1206 | 68 344 `RunPostPonedEvents` effects.c:1186-1612 | 11 42 `jalv_process_activate` process_setup.c:83-133 |
| 42 125 `lv2_instance_new` lv2_instance.c:1407-1538 | 58 193 `lv2_atom_forge_property_set` effects.c:6078-6310 | 10 58 `jalv_create_controls` jalv.c:204-269 |
| 28 82 `read_patches` lv2_world.c:733-817 | 51 230 `ProcessGlobalClient` effects.c:2500-2796 | 10 44 `create_port` jalv.c:101-153 |
| 25 40 `atom_port` lv2_world.c:602-642 | 46 423 `effects_init` effects.c:3876-4429 | 9 49 `jalv_send_to_plugin` jalv.c:284-338 |
| 24 47 `lv2_instance_activate` lv2_instance.c:1553-1604 | 46 175 `effects_cv_map` effects.c:7205-7433 | 9 41 `jalv_update` jalv.c:537-587 |
| 21 35 `read_required` lv2_world.c:1404-1442 | (the rest: `tools/hosting-compare.sh`) | 8 50 `jalv_select_custom_ui` jalv.c:622-689 |
| 21 24 `time_object` lv2_instance.c:1857-1881 | | 8 46 `set_port_value` state.c:123-179 |
| 20 42 `patch_read_outputs` lv2_instance.c:974-1020 | | 8 45 `jalv_close` jalv.c:1116-1177 |
| 19 75 `lv2_core_configure` lv2_features.c:302-379 | | 8 31 `symap_search` symap.c:76-113 |
| 19 51 `lv2_instance_run` lv2_instance.c:1918-1974 | | 8 20 `set_port_types` process_setup.c:149-171 |
| 19 51 `dirs_under` lv2_instance.c:449-503 | | 8 20 `jalv_process_strerror` process.c:24-44 |
| 19 44 `port_points` lv2_world.c:485-531 | | |

### Lines of code by area

Each cell is the summed NLOC of the functions whose names match that area, with the function count in brackets. The
script's `AREAS` list holds the patterns, and the first match wins. It is a heuristic: `--verbose` prints every
assignment, so a misfiled function can be checked.

| area | ours | mod-host f14a230 | mod-host main | jalv |
|---|---:|---:|---:|---:|
| worker | 88 (8) | 78 (6) | 78 (6) | 153 (11) |
| state (and presets, state paths) | 522 (34) | 456 (17) | 456 (17) | 177 (8) |
| CV | 8 (2) | 197 (2) | 197 (2) | 0 (0) |
| port groups / layouts / aux | 236 (14) | 0 (0) | 0 (0) | 0 (0) |
| CLAP contract / params / descriptor | 413 (30) | 201 (6) | 201 (6) | 0 (0) |
| latency / rate / buffer change | 157 (17) | 7 (1) | 7 (1) | 46 (1) |
| atoms / events / MIDI | 945 (51) | 1820 (50) | 1820 (50) | 599 (41) |
| ports and connect | 386 (14) | 287 (11) | 287 (11) | 313 (13) |
| error / refusal | 114 (9) | 141 (3) | 141 (3) | 64 (4) |
| lifecycle and shutdown | 291 (14) | 402 (10) | 418 (10) | 188 (12) |
| run / process | 164 (2) | 678 (3) | 678 (3) | 54 (2) |
| instantiate | 1103 (44) | 1339 (12) | 1339 (12) | 470 (12) |
| other | 278 (34) | 1141 (44) | 1141 (44) | 399 (23) |

Name matching undercounts code that sits inside a large function. Most of mod-host's port, CV and refusal code lives
in `effects_add`, which is filed under instantiate. Our CV binding lives in `instance_make` (instantiate) and
`plugin_process` (run). Read the area table together with the six tasks below.

## 2. The same tasks side by side

### Instantiate and connect a stereo effect

| | call sequence |
|---|---|
| ours | `factory_create` lv2_clap.c:1480 → `lv2_plugin_open` lv2_world.c:1509 (TTL read: `read_ports` :1209, `read_required` :1404; a refusal here comes before any binary is opened) → `lv2_instance_new` lv2_instance.c:1407 → `plugin_init` lv2_clap.c:940 → `lv2_plugin_load` lv2_world.c:1558 → `plugin_activate` lv2_clap.c:982 → `lv2_instance_activate` lv2_instance.c:1553 → `instance_make` :1235 (`d->instantiate` :1268, every port connected once to the instance's own memory :1328-1373, default state :1375) → LV2 activate → `plugin_process` lv2_clap.c:1123 copies in, `lv2_instance_run` lv2_instance.c:1918 → `desc->run` :1956, copies out |
| mod-host | `effects_add` effects.c:4541: `jack_client_open` :4589, `lilv_plugins_get_by_uri` :4601, `GetFeatures` :2936, `lilv_plugin_instantiate` :4636, port loop and `jack_port_register` :4780-4805, `jack_set_process_callback(ProcessPlugin)` :5411, `lilv_instance_activate` :5414, `jack_activate` :5417; wiring is a separate `effects_connect` :5996; `ProcessPlugin` :1707 copies in :2066-2070, `lilv_instance_run` :2080, copies out :2094-2098 |
| jalv | `jalv_open` jalv.c:878: `jalv_create_ports` :157 → `create_port` :101 → `jalv_process_port_init` process_setup.c:174; `jalv_backend_open` jack.c:381; feature check :1027-1034; `lilv_plugin_instantiate` :1038; `jalv_process_activate` process_setup.c:83; `jalv_backend_activate_port` jack.c:448; JACK `process_cb` jack.c:265 → `pre_process_port` jack.c:162 connects the JACK buffer itself every cycle (`lilv_instance_connect_port` jack.c:175) → `jalv_run` process.c:119 → `lilv_instance_run` :131 |

### A MIDI instrument via an atom port

| | call sequence |
|---|---|
| ours | `plugin_process` lv2_clap.c:1123 → `midi_in_events` :734 → `lv2_instance_midi_in_add` lv2_instance.c:1976 → `sequence_append` :326; out: `lv2_instance_midi_out_each` :1988 (bounded by its own capacity; ASan-proven by `lv2/tests/lv2_asan_test.c`) → `midi_out_event` lv2_clap.c:772 → CLAP `try_push` |
| mod-host | port branch effects.c:5016/5033, `AllocatePortBuffers` :965 → `lv2_evbuf_new` lv2_evbuf.c:44; per cycle `ProcessPlugin`: `jack_midi_event_get` :1880 → `lv2_evbuf_write` :1881; out `lv2_evbuf_begin/next` :2176 → `jack_midi_event_write` :2186 |
| jalv | `jalv_process_port_init` process_setup.c:164-215 → `lv2_evbuf_new` lv2_evbuf.c:23 in `jalv_process_activate`; per cycle `pre_process_port` jack.c:162: `jack_midi_event_get` :184-187 → `lv2_evbuf_write` lv2_evbuf.c:162; out `post_process_output_port` jack.c:204 → `jack_midi_event_write` :245, `jalv_write_event` comm.c:30 to the UI ring |

### The worker extension

| | call sequence |
|---|---|
| ours | `schedule_work` lv2_instance.c:571 → `omx_msgring_push` (src/omx_msgring.h:91, lock-free, `LV2_WORKER_ERR_NO_SPACE` when full) → `worker_main` :626 → `worker_service` :609 → `work()` :618 → `respond` :590 (refused outside its own `work()`: `g_in_work` :594) → next `lv2_instance_run`: `work_response` :1929, `run` :1956, `end_run` :1957; `worker_start/stop` :651/:663 |
| mod-host | `worker_init` worker.c:58 (from effects.c:4676) → `worker_schedule` worker.c:84 (`jack_ringbuffer_write` :89, `sem_post` :91) → `worker_func` :31 → `work()` :50 → `worker_respond` :21; `ProcessPlugin` drains `worker_emit_responses` worker.c:95 (effects.c:2086), `end_run` effects.c:2089 |
| jalv | `jalv_worker_new` worker.c:103 → `jalv_worker_launch` :142 → `worker_func` :62; `jalv_worker_schedule` :182 (threaded, or synchronous for the state worker :198-207); `jalv_worker_emit_responses` :214 and `jalv_worker_end_run` :231 from `jalv_run` process.c:136-138 |

The three are the same design: a request ring, a thread, a response ring drained before `run`, then `end_run`. Ours is
88 NLOC against 78 and 153.

### State save and restore

| | call sequence |
|---|---|
| ours | CLAP `state_save` lv2_clap.c:1303 → `lv2_instance_state_save` lv2_instance.c:1713; `state_load` lv2_clap.c:1328 → `lv2_instance_state_restore_live` :1721 (a `state:threadSafeRestore` plugin, no restart) or `lv2_instance_state_load` :1758 (held, applied by `state_apply` :1541 at the next activate) → `lv2_plugin_state_restore` lv2_world.c:1717; default state on instantiate `lv2_plugin_default_state` lv2_world.c:1616; `make_path` confined to the state root (`path_inside` lv2_instance.c:417, `make_path_refused` :506); presets through the CLAP preset-discovery factory (`presets_create` lv2_clap.c:1610) |
| mod-host | `effects_state_save` effects.c:7825 → `lilv_state_new_from_instance` :7892 → `lilv_state_save` :7913; `effects_state_load` :7743 → `lilv_state_restore` :7808 under `state_restore_mutex` for `HINT_STATE_UNSAFE` plugins (the audio thread `trylock`s it, :1717); presets `effects_preset_load` :5457, `effects_preset_save` :5512; paths state-paths.c:122/149 |
| jalv | `jalv_apply_state` state.c:182: without `state:threadSafeRestore` it pauses the audio thread through the UI ring and `zix_sem_wait(&proc->paused)` (:191-199), then `lilv_state_restore` :212, then resumes (:220-224); `jalv_save_preset` :241 → `lilv_state_save` :267; `jalv_load_presets` :79 |

### A sample-rate change

| | call sequence |
|---|---|
| ours | CLAP `plugin_activate` lv2_clap.c:982 → `lv2_instance_activate` lv2_instance.c:1553: same rate, a block that fits and an `options:interface` → `options_tell` :1105 (no re-instantiation, :1567); a new rate or a longer block → `instance_drop` (:1572) and `instance_make` again (:1583); `lv2_instance_keep` :1606 lets the shim ask first |
| mod-host | not handled at runtime: the rate is read once (`jack_get_sample_rate` effects.c:3962) and there is no `jack_set_sample_rate_callback` in the tree (0 hits). The block length is live: `BufferSize` effects.c:989 resizes the evbufs (:1001) and calls `options_interface->set` (:1038) |
| jalv | not handled at runtime: read once (`jack_get_sample_rate` jack.c:401), no sample-rate callback (0 hits in `src/`). The block length is live: `buffer_size_cb` jack.c:57 → `jalv_process_activate` |

### A plugin with CV or aux ports

| | call sequence |
|---|---|
| ours | TTL: `read_side` lv2_world.c:1120 groups aux, CV and surround ports by `pg:group`/designation (`port_group` :850, `group_kind` :976, `side_main_width` :1058, refused past a width `LV2_CODE_PORT_TOO_WIDE` :1097); `instance_make` binds the aux legs (lv2_instance.c:1354-1357) and CV legs (:1362-1364); `ports_get` lv2_clap.c:545 offers them as CLAP audio ports; `plugin_process` copies aux in :1156-1168, CV in :1170-1180, aux out :1228-1241, CV out :1243-1250 |
| mod-host | CV ports in `effects_add` effects.c:4932-5015 (`JackPortIsControlVoltage` :4961, range as JACK metadata :5012-5015); CV as a control source in `ProcessPlugin` :1913-1964 (`trylock` on `cv_source_mutex` :1915); no sidechain or port-group grouping |
| jalv | `set_port_types` process_setup.c:149-171 makes a CV port `TYPE_CV`, processed like audio (`pre_process_port` jack.c:170-174), registered as a JACK audio port with `signal-type=CV` metadata (jack.c:477-487); an unknown optional port is connected to NULL (jack.c:457-460); port groups are read only for UI grouping (`pg_group` nodes.c:52, control.c:49) |

## 3. Where we differ, and why

The classes: (a) real-time safety obligations the others do not meet; (b) the contract (hosting codes, verdicts,
refusals, descriptor); (c) layouts, aux and port-group features the others lack; (d) accidental complexity we could
delete.

| where ours is bigger | number | class | why, and the proof |
|---|---|---|---|
| Refusal before instantiation | `read_ports` CCN 57, `read_side` 50, `read_required` 21, `atom_port` 25; 11 `LV2_CODE_*` refusal sites in lv2_world.c | (b) | Every plugin is judged from its TTL and refused with a code before its binary is opened (codes lv2_core.h:107-126; tests `t_refusals` lv2/tests/lv2_clap_test.c:481 and lv2/tests/lv2_host_test.c:601, `t_midi_refusal` lv2_clap_test.c:526). jalv refuses only an unknown mandatory port type (process_setup.c:194-198, `jalv_open` returns -10) and a missing feature (-8). mod-host leaves an unknown port unconnected without a check (no `connectionOptional` in effects.c) and leaves a missing required feature to the check inside `lilv_plugin_instantiate` (`ERR_LV2_INSTANTIATION` :4641). |
| CLAP contract, params, descriptor | 413 NLOC (30 functions) against 201 and 0 | (b) | The parameter table, its text/value maps, the contract getters (`contract_make` lv2_clap.c:843) and the descriptor: the CLAP face the others do not have. mod-host's 201 is its parameter get/set/monitor commands (`effects_set_parameter` effects.c:5994, `effects_get_parameter_info` :6591). |
| A live rate or block change | 157 NLOC (17) against 7 and 46 | (b) | CLAP may re-activate at a new rate: ours re-instantiates or tells `options:interface` (lv2_instance.c:1567-1583). Neither JACK host handles a rate change at runtime (0 sample-rate callbacks). |
| Layouts, aux, sidechain, surround | 236 NLOC (14) against 0 and 0; `read_side` CCN 50 | (c) | Aux legs, CV legs and surround/ambisonic groups become CLAP audio ports (`ports_get` lv2_clap.c:545). The others expose CV only (mod-host as JACK CV ports, jalv as audio ports) and no group as a bus. Tests: `t_sidechain_limits` lv2_clap_test.c:2459. |
| State, presets and state paths | 522 NLOC (34) against 456 and 177 | (b) + (c) | A live restore for `state:threadSafeRestore` plugins plus a held restore applied at activate; `make_path` confined to a root with a refusal code (`path_inside` lv2_instance.c:417); presets through the CLAP preset-discovery factory. jalv joins the path to its save or temp directory with no containment check (`jalv_make_path` state.c:33) and pauses the audio thread instead of holding the state. |
| Allocation-free, lock-free run | `lv2_instance_run` CCN 19; `respond` refused off its `work()` (lv2_instance.c:594); `schedule_work` off the audio thread counted (:576-577) | (a) against mod-host; equal to jalv | The run path takes no lock and allocates nothing. Proof: the allocation witness in `lv2/tests/lv2_run_test.c` (wraps malloc/calloc/realloc/free, :55-64, with its own positive control :336-341), run by `make test-lv2-core` (Makefile:726). mod-host's `ProcessPlugin` takes a blocking `pthread_mutex_lock(&g_rtsafe_mutex)` (effects.c:2271), as does `ProcessGlobalClient` (:2625, :2686, :2736), and calls `fprintf` on an evbuf overflow (:1883). jalv's `process_cb`/`jalv_run` take no lock and allocate nothing (jack.c:265-299, process.c:119-150), so here we match jalv. |
| A fixed block length the plugin was promised | lv2_clap.c:1134-1140 | (b) | A cycle of another length is refused, counted and reported from the main thread. Neither JACK host can receive such a cycle. |
| Buffers copied in and out | `plugin_process` copies every leg (lv2_clap.c:1141-1250) | (b) | Every LV2 port is connected once, to memory the instance owns (lv2_instance.c:1328-1373), because a CLAP host may hand different buffers each cycle and an LV2 plugin may dereference any port. jalv connects the JACK buffer in place every cycle (jack.c:175). mod-host copies too (effects.c:2066-2070). |
| `instance_make` port binding | CCN 86, 154 NLOC; one loop of 14 role tests per port (lv2_instance.c:1328-1373) | (d) | A linear search over every role for every port index. **Candidate 1** below. |
| `plugin_process` copy loops | CCN 62; four near-identical "host channel or silence" loops (lv2_clap.c:1156-1250) | (d) | The null-guard ternaries repeat for main, aux, CV, in and out. **Candidate 2** below. |
| `read_ports` / `read_side` | CCN 57 + 50, 252 NLOC | (b)/(c) and (d) | The classification is the contract and the layouts, but one function holds every port kind's rule. **Candidate 3** below. |
| `NO_REALISATION` refusal tails | 13 `lv2_why_set(why, LV2_CODE_NO_REALISATION)` sites in lv2_instance.c, 9 in `instance_make` alone | (d), small | Each is a three-line `why`/`return -1` tail; one `goto refuse` per function would remove about 20 lines and change no behaviour. Folded into candidate 1. |

Where ours is simpler:

- **Than mod-host, everywhere measured:** 5648 code lines against 8164; mean CCN 6.38 against 9.08, p95 19 against 28,
  max 86 against 127, mean function length 17.2 NLOC against 40.9. mod-host puts a plugin's whole life in four
  functions of 300-736 NLOC (`effects_add`, `ProcessPlugin`, `effects_remove`, `RunPostPonedEvents`). Ours spreads the
  same life over 273 functions.
- **Run path:** our `run / process` area is 164 NLOC (2 functions) against mod-host's 678 (3), with no lock (mod-host
  effects.c:2271).
- **Worker:** 88 NLOC against jalv's 153; jalv also carries a synchronous state worker (worker.c:198-207).
- **CV:** our CV binding is part of the generic leg binding (lv2_instance.c:1362-1364). mod-host's
  `effects_cv_map` (CCN 46, effects.c:7205) and its CV-as-control-source path in `ProcessPlugin` (:1913-1964) have no
  counterpart.
- **Dependencies:** 4 include roots (`clap lilv lv2 omxcontract`) against jalv's 5. lilv is opened at runtime (no test
  links liblilv, Makefile:728).

Against jalv, ours is bigger: 5648 code lines against 3970 (1.42×), mean CCN 6.38 against 3.62 (1.76×), p95 19
against 9. The difference sits in four areas jalv does not have: the CLAP contract/params/descriptor (413), layouts/aux
(236), a rate change (157), and the part of state/presets beyond jalv's (522 − 177 = 345). Together that is 1151 NLOC,
which covers most of the 1678-line code gap.

## 4. Coverage, by the admission probe

The same question put to all three hosts on one set: of the LV2 plugins installed on the desk, how many does the host
instantiate and activate on a live graph? The set is the 979 URIs lilv listed on the desk on 2026-10-20 (Fedora 44,
after the operator's removals of that day and the ams-lv2 install). The probe is the coverage kit's: a private user,
net, pid and mount namespace with its own PipeWire (48 kHz, null sink), one URI at a time, 10 s per reply, the desk
graph checked untouched before and after each arm. Hosted means admitted, instantiated and activated; it is not an
audio-identity or parity claim (that is `tests/lv2_parity.py`).

| host | version | hosted | not hosted |
|---|---|---|---|
| ours | omx-clap-core main 9972e16 (2026-10-20) | 979 of 979 | none (978 in the batch; `ir` hangs beside a GLib-criticals neighbour there and hosts 3 of 3 alone) |
| mod-host | 0.10.6 f14a230-11 (2026-10-26) | 975 of 979 | drumkv1 (SIGABRT), samplv1 and synthv1 (the host died on `add`), zynadd (`resp -102`) |
| jalv | 1.8.0 (2026-10-26) | 977 of 979 | zynadd (`Mandatory port "midi_in" has unknown type`: the `llext:MidiPort` our reader realises since #292), synthv1 (SIGABRT at instantiate, alone too) |

How each arm asks: ours and mod-host take `add <uri> <n>` then `remove <n>` on the host's socket, a fresh host every 20
plugins and after any crash; jalv takes one `jalv -n <name> <uri>` per URI, headless with stdin held open, and counts the
plugin hosted when its node appears on the namespace's graph (every one did within 0.4 s). jalv 1.8.0 does not leave on
stdin EOF, SIGINT or SIGTERM in that namespace, so the probe ends it with SIGKILL after the verdict; that is noted per
row and is not a hosting fault.

Two things the table says beyond the counts. The v1 family (drumkv1, samplv1, synthv1) crashes or kills mod-host and
jalv and runs under ours. And mod-host's earlier figure, 915 of 975 on the previous day's set, is the same binary: 55
of its 56 refusals were plugins removed from the desk on 2026-10-20 (52 ll-plugins bundles among them), and the one that
remains in the set is zynadd.

Raw rows, logs and the jalv probe are kept with the coverage evidence of 2026-10-20 and 2026-10-26.

## 5. Verdict

**Similar in the end, not overcomplicated.** All three hosts make the same lilv calls in the same order: instantiate,
connect, a worker of two rings and a thread, then state through `lilv_state_*`. Ours is smaller and simpler than
mod-host on every measure above, and it meets real-time obligations mod-host does not (a blocking lock in mod-host's
audio callback, effects.c:2271). It is 1.42× jalv in code. That excess is almost all features jalv does not offer and
the CLAP contract requires: refusal before instantiation with a code, a live rate change, aux, CV and surround legs as
audio ports, confined state paths and CLAP preset discovery. The accidental complexity is concentrated in three
functions, which between them hold all four of our functions at CCN 50 or above. These are the three simplification candidates:

1. **`instance_make`: bind ports from a table built at TTL read** (CCN 86). `read_ports` already knows each port's
   role. It can record a `role, leg, channel` triple per port index, so the connect loop (lv2_instance.c:1328-1373)
   becomes a single switch, and a `goto refuse` collapses the nine `NO_REALISATION` tails. Expected CCN under 30.
   Risk: low. A wrong binding connects a port to the wrong buffer, which `lv2_host_test`, `lv2_clap_test` (sidechain,
   CV, MIDI, patch arms) and `lv2_asan_test` catch.
2. **`plugin_process`: one "host channel or silence" helper for every leg** (CCN 62). The main, aux and CV loops in and
   out (lv2_clap.c:1141-1250) differ only in the buffer index and the mono-feeds-both rule. Expected CCN under 25.
   Risk: low to medium. This is the audio thread, so the helper must stay `static inline` with no new branch per
   sample. The allocation witness and the sidechain/CV tests must stay green, and `bench/` should show no cost change.
3. **`read_ports` / `read_side`: one classifier per port kind** (CCN 57 + 50). Split the per-port body into
   `classify_audio`, `classify_atom`, `classify_control` and `classify_cv` returning a refusal code, and leave the
   loop as a dispatcher. Risk: medium. The refusal codes and their order are part of the contract, so each existing
   refusal test must name the same code before and after.

Follow-up issues: the three candidates are filed on FreeMixer/omx-clap-core as #279 (`instance_make` binds ports from
a role table built at TTL read), #280 (`plugin_process` copies legs through one helper) and #281 (`read_ports` and
`read_side` classify one port kind per function), all under #267.
