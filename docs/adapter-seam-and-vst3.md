# The adapter seam, and what a VST3 adapter would cost

Two questions, from omx-clap-core#287. First, which parts of the LV2 adapter are format-neutral and could be shared by
a second adapter? Second, what would that second adapter cost if it were VST3, the realistic candidate? (AU is
macOS-only, so it cannot be tested on our Linux CI or rig.) The operator's ruling of 2026-10-20 decides the outcome:
**no VST3 adapter unless it has a demonstrated advantage, but we must know we could host it, and keep the results.**
The spike is therefore kept on the branch `spike/vst3-feasibility` (not for merge, not for deletion). This report
holds the numbers.

Every number below has a command or a file:line. Part 1's tables are printed by `tools/adapter-seam.sh` from the
classification in `docs/adapter-seam.tsv`. The method (lizard for functions and NLOC, the "ours+plumbing" file set) is
the one of `tools/hosting-compare.sh`, the #267 report on the `docs/hosting-comparison` branch (283380c), which this
report extends.

## The answer in numbers

- The adapter and its plumbing hold **5754 NLOC in 336 functions**. By class: **1594 NLOC (139 functions)
  format-neutral**, 1699 (73) mixed (neutral logic around a few format calls), and 2461 (124) LV2-specific.
- The seam already exists for the plumbing. All six `src/` headers the adapter includes are neutral (725 NLOC), and
  four of them are already used outside `lv2/`.
- The seam still to cut is the shim, `lv2/clap/lv2_clap.c`: 1314 of its 1319 NLOC are neutral or mixed, and 1161 of
  them would serve a VST3 shim. `lv2/core` is 66% LV2-specific (2456 of 3710 NLOC). Its 522 neutral NLOC (the
  worker loop, path containment, atom byte helpers, the rtmempool pool) have no VST3 consumer.
- Of the three simplification candidates, **#280 moves the seam the right way**. #279 and #281 are LV2-internal and
  leave the seam where it is.
- Audio-thread indirection today: the shim adds **one indirect call per cycle plus one per MIDI-out event**. Every
  other indirect call on the audio path is the CLAP host's or the plugin's own (objdump, 1.5). A shared layer must add
  none.
- No other omx library (omx-dsp, plugin-hostd, omx-plugins) holds code that would replace a shared layer (1.6).
- A VST3 adapter needs **no C++**. Steinberg's generated C API works: the spike is 477 code lines of C11 that hosts an
  LSP effect end to end. The SDK is **MIT** and the C API **BSD-3-Clause**, both usable in a GPL-3.0-or-later host.
- The cost: about **2200 NLOC on the format side** plus tests and C fixtures, about **6200 lines** in all. At the pace
  `lv2/` grew, that is about **3 weeks of lane time with the seam, 2.5 without**.
- The demand is thin. In the distribution repositories, **64 open-source plugin packages are VST3-only on Linux** (no
  LV2 or CLAP build), none of them one we use. Among closed-source vendors, **6 ship Linux VST3 without CLAP or LV2**:
  ToneBoosters, Loomer, Renoise Redux, Tracktion BioTek/F'em, Bertom Audio, DDMF. Two of them have activation a
  headless host cannot do.
- **Recommendation: not now.** Land #280. Do not cut the seam until a second adapter is approved. Reopen on the
  trigger in section 3.3.

## Part 1. The seam

### 1.1 Method

The measured files are `lv2/core/*.[ch]`, `lv2/clap/*.[ch]` and the six `src/` headers they include:
`omx_msgring.h`, `hosted_stage.h`, `param_contract.h`, `omx_clap_midi.h`, `clap_host_limits.h` and `omx_clap_ext.h`.
The last two hold only types, constants and the CLAP extension vtables, so they contribute no function. lizard 1.24.1
lists 336 functions. Each was read from its body and given a class:

- **NEUTRAL**: a VST3 adapter would use the code unchanged. It depends only on CLAP (the face every adapter presents
  to the host), the omx contract, the ring, the limits, or plain C.
- **MIXED**: neutral logic wrapped around a few format calls. The row's reason names the hook the format side would
  supply.
- **SPECIFIC**: LV2 only. This includes code where only the idea is shared: a function whose body is lilv calls is
  SPECIFIC even if a VST3 adapter needs "the same idea".

Each function also has an area and a thread: AUDIO, WORKER, MAIN, or ANY (callable from any thread, as URID map is). The
classes are judgements. Each row of `docs/adapter-seam.tsv` gives its reason in one line so it can be challenged.
`tools/adapter-seam.sh` joins the table with lizard's list at the table's `# ref:` (00a9b2e, origin/main when this was
written). It fails on a function the table does not class and on a row whose function is gone, then prints the sums.

### 1.2 By file

| file | NEUTRAL | MIXED | SPECIFIC | total |
|---|---:|---:|---:|---:|
| lv2/clap/lv2_clap.c | 347 (46) | 967 (44) | 5 (1) | 1319 (91) |
| lv2/core/lv2_features.c | 30 (3) | 30 (1) | 449 (31) | 509 (35) |
| lv2/core/lv2_instance.c | 275 (29) | 501 (22) | 923 (44) | 1699 (95) |
| lv2/core/lv2_rtmempool.c | 169 (12) | 0 (0) | 12 (3) | 181 (15) |
| lv2/core/lv2_world.c | 48 (6) | 201 (6) | 1072 (45) | 1321 (57) |
| src/hosted_stage.h | 201 (15) | 0 (0) | 0 (0) | 201 (15) |
| src/omx_clap_midi.h | 35 (2) | 0 (0) | 0 (0) | 35 (2) |
| src/omx_msgring.h | 59 (6) | 0 (0) | 0 (0) | 59 (6) |
| src/param_contract.h | 430 (20) | 0 (0) | 0 (0) | 430 (20) |
| **all** | **1594 (139)** | **1699 (73)** | **2461 (124)** | **5754 (336)** |

Each cell is NLOC (functions).

### 1.3 By area and by thread

| area | NEUTRAL | MIXED | SPECIFIC | total |
|---|---:|---:|---:|---:|
| params / contract | 618 (39) | 450 (19) | 371 (14) | 1439 (72) |
| ports / layouts / aux | 92 (16) | 222 (9) | 592 (20) | 906 (45) |
| instantiate / discovery | 48 (8) | 265 (11) | 442 (13) | 755 (32) |
| state / presets / paths | 207 (16) | 181 (11) | 297 (18) | 685 (45) |
| features / URID / options / log | 0 (0) | 30 (1) | 413 (31) | 443 (32) |
| process / run | 154 (11) | 145 (3) | 57 (1) | 356 (15) |
| MIDI / events / time | 98 (8) | 85 (3) | 172 (12) | 355 (23) |
| other | 234 (21) | 28 (2) | 56 (7) | 318 (30) |
| lifecycle | 19 (4) | 149 (5) | 23 (3) | 191 (12) |
| worker / threading / rings | 91 (9) | 67 (5) | 10 (2) | 168 (16) |
| latency / rate / activate | 8 (2) | 65 (3) | 28 (3) | 101 (8) |
| refusal / errors | 25 (5) | 12 (1) | 0 (0) | 37 (6) |

| thread | NEUTRAL | MIXED | SPECIFIC | total |
|---|---:|---:|---:|---:|
| AUDIO | 343 (37) | 455 (16) | 339 (16) | 1137 (69) |
| ANY | 86 (11) | 30 (1) | 91 (10) | 207 (22) |
| WORKER | 22 (1) | 30 (2) | 0 (0) | 52 (3) |
| MAIN | 1143 (90) | 1184 (54) | 2031 (98) | 4358 (242) |

### 1.4 What is neutral, and what would move

**The plumbing is already shared.** Each of the six `src/` headers is neutral, and four are already used outside
`lv2/`: `omx_msgring.h` by `src/effects.c` and `tests/msgring_test.c`; `hosted_stage.h` and `omx_clap_midi.h` by the
CLAP host's real-time body `src/clap_stage.h`; `param_contract.h` by `src/clap_host.c`. `param_contract.h` reads the
parameter contract of any CLAP plugin, a shim's included, so it is neutral although it takes `clap_plugin_t`. The
shared internal layer the standing position asks for (headers plus a small static library, inside omx-clap-core, not
public ABI) therefore already exists for the ring, the hosted-stage body, the contract and the MIDI helpers.

**The shim is the seam still to cut.** `lv2/clap/lv2_clap.c` presents one LV2 plugin as one CLAP plugin. It is 1319
NLOC, and only `omx_clap_lv2_worker_quiesce` (:1799, 5 NLOC) is specific. Another 12 functions (153 NLOC) are neutral
or mixed but serve LV2-only ideas a VST3 shim would never call: the patch-text parameters `omx_clap_lv2_patch_text_*`
(:1845-1897), `patch_info` :275, `patch_of_id` :259, `emit_patch_changes` :455, `text_of_uri` :1835, the makePath
directory `omx_clap_lv2_saved_dir/drop` :1805/:1815, and the core configuration `omx_clap_lv2_configure` :1669 and
`omx_clap_lv2_feature_options` :1701. That leaves **1161 NLOC a VST3 shim would reuse**:

- As-is (NEUTRAL): the refusal record (`why_set` :216, `refuse` :232, `omx_clap_lv2_last_refusal` :1786); the port
  arithmetic (`ports_count` :538, `audio_port_at` :592, `any_port_of` :604, `main_inputs` :504, `shim_main_width`
  :511); the surround and ambisonic extensions (`surround_mask_of` :628, `ambisonic_*` :678/:684); the note ports
  (:702/:709); the contract getters (:899-935); the factory, descriptor and entry bookkeeping (`descriptor_fill`
  :1465, `slot_factory` :1634, `entry_init` :1624); the preset-provider glue (:1550-1622); the CLAP state stream reader
  `read_all` :1259; the MIDI-out sink `midi_out_event` :772.
- With hooks (MIXED): `plugin_process` :1123 (113 NLOC, the leg copies, events in and out, the fixed-block check, the
  counters), `params_get_info` :288, `params_value_to_text/text_to_value` :346/:375, `contract_make` :843,
  `ports_get` :545, `plugin_activate` :982, `plugin_init` :940, `factory_create` :1480, `omx_clap_lv2_entry` :1714,
  `state_save/state_load` :1303/:1328, `midi_in_events` :734, `time_of` :1094, `latency_watch` :1070, `apply_events`
  :406 and `triggers_reset` :431.

**`lv2/core` stays LV2.** `lv2_world.c` (TTL through lilv) is 1072 of 1321 NLOC specific, and `lv2_features.c` (URID,
options, log, the feature set) 449 of 509. `read_ports` (:1208, 195 NLOC, CCN 64) is lilv queries throughout, so it is
specific even though "classify the I/O" is an idea VST3 shares. `lv2_instance.c` is 923 of 1699 specific:
`instance_make` (:1302, 183 NLOC, CCN 106), `lv2_instance_new` (:1505, 130), the atom sequences, `time:Position`, patch
forging. Its 275 neutral NLOC are real code: the worker thread loop `worker_main` :690 over `omx_msgring`, the
symlink-safe path containment `path_inside` :481, `dirs_under` :513 and `open_refusal` :501, the seqlock text slot
`text_report` :982 and `lv2_instance_text_get` :2286, the byte helpers `put32/put64/get32/pad8` :745-766, and the
keep-or-reinstantiate rule `lv2_instance_keep` :1709. The rtmempool pool of #265 (`lv2_rtmempool.c`, 169 neutral NLOC of
chunks and lists under a priority-inheriting lock) is the same kind. Each serves an LV2 idea: the worker, makePath, atom
forging, patch text, the lv2dynparam pool. VST3 has none of these, so a VST3 adapter would not call them, and moving
them would share nothing. The mixed rows of `lv2_world.c` are the one exception worth keeping in view. The width rules
of a side bus (`read_side` :1119, `side_main_width` :1057, `describe` :1093) apply to any format with aux buses, but
their inputs are lilv facts, so they would split rather than move.

### 1.5 The proposed seam and the interface the format side supplies

When a second adapter is approved, `lv2_clap.c` splits in two. The 1161 reusable NLOC go to a shared shim inside
omx-clap-core. Its audio-thread half is `static inline` in a header, its main-thread half lives in the internal static
library, and it is not public ABI. The LV2 remainder becomes `lv2/clap/lv2_fmt.c`. From the hooks the mixed functions
call (`docs/adapter-seam.tsv`, the reason column), the format side supplies this interface:

| group | hooks | VST3 counterpart |
|---|---|---|
| lifecycle | `load(id) → instance`, `free`, `activate(rate, min, max) / keep(rate, min, max)`, `deactivate`, `reset`, `latency` | `createInstance` + `initialize`, `terminate`, `setupProcessing` + `setActive(true)`, `setActive(false)`, `setProcessing(false/true)`, `getLatencySamples` |
| parameters | `param_count`, `param_info(k)` (range, flags, steps, unit), `param_get/set(k)`, `value_to_text / text_to_value(k)`, `symbol(k)` | `getParameterCount/Info`, normalised ↔ plain, `getParamStringByValue/ValueByString` |
| ports | a port table (kind, width, CLAP channel map, main or aux), `buffer_in/out(leg, channel)` | `getBusInfo`, `SpeakerArrangement`, `AudioBusBuffers` |
| process | `run(n)`, `midi_in(frame, bytes)`, `midi_out_each(sink)`, `transport(time)`, `counters` | `process(ProcessData)`, `IEventList`, `ProcessContext` |
| state and presets | `state_save → blob`, `state_load(blob)`, `presets_each`, `preset_load(key)` | `getState/setState` (component and controller), `.vstpreset` |
| discovery | `bundle_open/close(path)`, `bundle_count`, `bundle_id/name(i)`, `refuse_before_open(id) → code` | bundle scan, `IPluginFactory`, `moduleinfo.json` |

That is about 25 hooks. VST3 needs four things the LV2 side never asked for. The controller and the processor are
split, with messages between them (`IConnectionPoint`/`IMessage`). Parameter changes flow back from the processor to
the controller. `restartComponent` flags must be turned into CLAP requests. State is two blobs, not one. These are
VST3-side work (Part 2) and do not widen the interface.

**Which of #279, #280 and #281 move the seam the right way.**

- **#280 (plugin_process copies legs through one helper): yes.** Commit f051a9d on
  `cluster/omx-clap-core-280-cluster` adds `host_channel` and `leg_copy` as `static inline` helpers over
  `clap_audio_buffer_t` and `float *` only. Neither contains an `lv2_*` call, so they are the first piece of the
  shared shim's audio half as they stand.
- **#279 (instance_make binds ports from a role table built at TTL read): no.** The table is indexed by LV2 port, for
  a one-time `connect_port` per port. VST3 hands whole bus and channel arrays to every `process` call and has no
  per-port binding. The cleanup is still worth it on LV2's merits (CCN 106), but it does not move the seam.
- **#281 (read_ports and read_side classify one port kind per function): no.** Each `classify_*` would still decide
  by lilv predicates, while VST3 gets its buses already classified from `IComponent::getBusInfo`. Only the refusal
  codes carry over, and they already live in omx-contract (`omxcontract/omx_vocabulary.h`, included at
  `lv2/core/lv2_core.h:62`). Of those codes a VST3 adapter would need `NO_REALISATION`, `FEATURES_MISSING`,
  `NO_AUDIO_OUTPUT`, `TOO_MANY_PORTS`, `PORT_TOO_WIDE`, `STATE_UNREADABLE` and `PRESET_NOT_FOUND`.

**The audio-thread functions, and the proof they stay free of indirection.** 69 functions (1137 NLOC) run on the
audio thread; `tools/adapter-seam.sh` lists each with how it is defined. The neutral ones that would move are already
`static` or `static inline`: the `hosted_stage.h` body, `omx_clap_midi.h`, `omx_msgring.h`, `midi_out_event`,
`frame_in`, `control_of_id`, `main_inputs`, and the #280 helpers. The shim reaches `lv2/core` through direct calls
to the `lv2_instance_*_in/_out` accessors (`lv2_instance.c:1884-1912`, extern, 4 NLOC each). To count the indirect
calls in the compiled audio path, build the objects (`make lv2/clap/lv2_clap.o lv2/core/lv2_instance.o`, `-O3`) and
run `objdump -d -l --no-show-raw-insn <obj>`. Every `call *` resolves to a source line as follows:

| function | instructions | direct calls | indirect calls, and whose |
|---|---:|---:|---|
| `plugin_process` | 833 | 35 | 7, **all the CLAP host's**: `events->size/get` (lv2_clap.c:736/:741), `out->try_push` (:450/:473), `host->request_callback` (:1138/:1250), `host->request_restart` (:1089) |
| `lv2_instance_run` | 773 | 12 | 4: **the plugin's own** `work_response`, `run`, `end_run` (lv2_instance.c:2034/:2066/:2068), and **ours**: `before(ctx)` (:2055), which is `write_controls` passed from lv2_clap.c:1192 |
| `lv2_instance_midi_out_each` | 149 | 0 | 2: **ours**, `fn(ctx, …)` (lv2_instance.c:2127/:2157), which is `midi_out_event` passed from lv2_clap.c:1198, once per MIDI event |

The shim's own indirection is therefore one call per cycle (`write_controls`) plus one per MIDI-out event. The third
pointer, `shim_is_audio` (lv2_clap.c:965), is called only when the plugin calls back into a host feature. The rule for
the shared layer follows. Its audio-thread half is `static inline` in a header compiled into each format's shim, and
the format hooks are direct calls to that format's functions, bound when it is compiled, with no vtable. The
acceptance test is this objdump count: no indirect call beyond the CLAP host's and the plugin's own. The split could
even remove the two the shim has today.

### 1.6 Is anything in another omx library reusable instead?

No. Each repository was cloned at its default branch on 2026-10-20 and searched for a ring, a worker, a parameter
table, event-to-MIDI conversion, a state stream, a scanner and a refusal vocabulary:

- **omx-dsp** (bf94f78) is DSP. `omx_wetdry.h` is already the crossfade `hosted_stage.h` uses. `omx_note_map.h` maps
  a MIDI message to a MIDI message (transpose, split, velocity), not a note event to MIDI. It has no ring, no worker
  and no parameter table.
- **plugin-hostd** (03d6d27) runs "your LV2 and CLAP audio plugins, each in a worker process of its own"
  (`README.md:4`): "LV2 (through mod-host) and CLAP (through omx-clap-host)" (`README.md:11`). Its format dispatch is
  a prefix table (`src/formats.c`). `grep -rli vst3` over all three repositories finds 0 files. Its one overlap is a
  second from-scratch SHA-256 (`include/plugin-hostd/pin.h:55-150`, `phd_sha256_*`) beside `pc_sha256_*` in
  `src/param_contract.h`.
- **omx-plugins** (c275aa6) is the plugin side. `include/omx_param_contract.h` mirrors the parameter-contract
  vocabulary of `src/omx_clap_ext.h`. `include/omx_meter_ring.h` is a three-slot meter snapshot, not a message ring.

The refusal vocabulary is already shared through a fourth library, omx-contract (`omx_vocabulary.h`). So the shared
layer stays inside omx-clap-core, where four of its six headers already are. The only cross-repository duplicate
found, the SHA-256, is a follow-up (section 4).

## Part 2. What a VST3 adapter would cost

### 2.1 The SDK: licence, language, what a host implements

**Licence.** VST 3 SDK 3.8.1 as Fedora 44 packages it (`vst3sdk-common`, `vst3sdk-hosting`, `vst3sdk-devel`
3.8.1-1.fc44, RPM licence tag `MIT`) ships `/usr/share/licenses/vst3sdk-common/LICENSE.txt`:

> MIT License
>
> Copyright (c) 2026, Steinberg Media Technologies GmbH
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
> documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
> rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software [...]
>
> The above copyright notice and this permission notice shall be included in all copies or substantial portions of
> the Software.

The SDK's `README.md` ("License & Usage guidelines") adds: "Licensing under **GPLv3** and the **Steinberg proprietary
license** is no longer available". It also says "Trademark usage (e.g. "VST" name or logo) is optional under MIT
license, but if used, must comply with Steinberg's official trademark rules", and lists under "Prohibited Use":
"Using “VST” (or derivatives like “VSTi”) in company or product names."

MIT code combines into a GPL-3.0-or-later work. The one duty is to keep the MIT notice with any SDK file we ship. The
trademark rule limits naming only. The audinux copr build of the same version has the RPM tag `GPL-3.0-or-Later`, but
ships the same MIT `LICENSE.txt`, so the tag is stale packaging metadata (`rpm -qp --qf '%{license}'
vst3sdk-3.8.1-1.fc44.x86_64.rpm`).

**Language.** The SDK is C++: COM-style interfaces (`queryInterface`/`addRef`/`release`, 16-byte interface ids,
`tresult` codes). Its hosting helpers are C++ too (`public.sdk/source/vst/hosting/*.h`, 991 header lines;
`libvst3sdk_hosting.so`, 187 exported functions, which links `libstdc++.so.6` per `readelf -d`). **A usable C API
exists.** Steinberg publishes `vst3_c_api.h`, generated from the C++ interfaces
(https://github.com/steinbergmedia/vst3_c_api, tag "VST3 SDK 3.8.1" at 93a116f, 3777 lines). Its `LICENSE.txt` is the
BSD 3-clause text ("Redistribution and use in source and binary forms, with or without modification, are permitted
provided that [...]"). Every interface is a struct with an `lpVtbl`, and every id is a `static const Steinberg_TUID`.
The spike (2.2) is plain C11 against that header, with no C++ compiler and no SDK library, so a VST3 adapter can be C
like the rest of this library. The price is writing the host's COM objects by hand: 257 code lines in the spike.

**What a host implements, mapped onto the six tasks of the LV2 comparison.** The estimates are calibrated by the
spike's measured sections and by the LV2 area doing the same job (1.3). "Seam" names the shared code each task would
reuse. SDK references are to `/usr/include/vst3sdk/pluginterfaces/`.

| task | what VST3 needs from the host | NLOC (estimate) | reuses from the seam |
|---|---|---:|---|
| instantiate and connect a stereo effect | scan `*.vst3` bundles (`Contents/x86_64-linux/<name>.so`; `moduleinfo.json` optional); `dlopen`, `ModuleEntry(handle)`, `GetPluginFactory` (exported by `lsp-plugins.so`, `nm -D`); `IPluginFactory` (`base/ipluginbase.h:192`, `IPluginFactory2/3` :321/:447); `IComponent::initialize` with an `IHostApplication` (`vst/ivsthostapplication.h:35`) that also allocates `IMessage`/`IAttributeList` (`vst/ivstmessage.h:40`, `vst/ivstattributes.h:40`); the controller; `IConnectionPoint` both ways (`vst/ivstmessage.h:73`); buses (`vst/ivstcomponent.h:93`/`:158`); `setBusArrangements`, `setupProcessing`, `activateBus`, `setActive`, `setProcessing`; `process(ProcessData)` with `AudioBusBuffers` (`vst/ivstaudioprocessor.h:202`/`:224`) | 650 | the CLAP face, the port table, the leg copies (#280), the fixed-block check, the refusal record |
| a MIDI instrument | there is no raw MIDI input. Notes arrive as `Event` `kNoteOnEvent/kNoteOffEvent/kPolyPressureEvent` (`vst/ivstevents.h:161-164`) in an `IEventList` (`:198`) the host implements, preallocated. CC, pitch bend and aftertouch become parameter changes through `IMidiMapping::getMidiControllerAssignment` (`vst/ivsteditcontroller.h:659`). Out: `kLegacyMIDICCOutEvent` (`vst/ivstevents.h:170`) | 300 | the note ports, `midi_in_events`, `omx_clap_midi.h` |
| the worker-like threading rules | there is no worker. Each call has its thread: `IComponent`/`IEditController` on the "UI-thread" (`vst/ivstcomponent.h:163-203`, `vst/ivsteditcontroller.h:223-740`), `process` on the "processing-thread", `setProcessing` on either (`vst/ivstaudioprocessor.h:342`). Controller edits (`IComponentHandler::performEdit`, `vst/ivsteditcontroller.h:229`) arrive on the main thread and must reach the next `process` as `IParameterChanges`/`IParamValueQueue` (`vst/ivstparameterchanges.h:84`/`:121`). Output changes travel back to the controller on the main thread. `restartComponent` flags (`vst/ivsteditcontroller.h:115-141`, `:240`) become a CLAP restart or latency request | 400 | `omx_msgring`, `params_flush`, `latency_watch` |
| state save and restore | an `IBStream` (`base/ibstream.h:29`; 56 code lines in the spike). One CLAP state holds two blobs, `IComponent::getState/setState` and `IEditController::getState/setState`, plus `setComponentState` to keep the controller in step. Restore only after `setupProcessing` (2.2) | 200 | `read_all`, `state_save/load`, the state held until activate |
| a rate change | `setActive(false)`, `setupProcessing` with the new rate and block (allowed only while inactive, `vst/ivstaudioprocessor.h:330`), `setActive(true)`. No re-instantiation, unlike an LV2 plugin without `options:interface` | 60 | `plugin_activate`, `lv2_instance_keep`'s rule |
| aux / sidechain buses | `kAux` buses after the `kMain` ones (`vst/ivstcomponent.h:57`, `:84-85`). `SpeakerArrangement` bitmasks (`vst/vstspeaker.h`) map onto CLAP port types and surround channel maps. An optional bus may be deactivated (`activateBus`) | 200 | `ports_get`, the surround and ambisonic extensions, `shim_main_width` |
| parameters (the CLAP face needs them) | `getParameterCount/getParameterInfo` (`ParameterInfo` flags `kIsBypass`, `kIsReadOnly`), normalised doubles, `normalizedParamToPlain`, `getParamStringByValue/getParamValueByString`, sample-accurate queues | 250 | `params_*`, `contract_make`, `param_contract.h` |
| discovery and refusals | a code per refusal (no audio bus, a bus too wide, no 32-bit processing, a factory without an audio class), and a scan that need not open the binary when `moduleinfo.json` is present | 150 | the refusal vocabulary, the factory and entry bookkeeping |
| **format side, total** | | **≈ 2200** | |

### 2.2 The spike (branch `spike/vst3-feasibility`, 2b3cc43, not for merge)

`spike/vst3/vst3_probe.c` dlopens `lsp-plugins.vst3` (lsp-plugins-vst3 1.2.29-1.fc44). It reads the factory, creates
"Compressor Stereo", lists the buses and parameters, round-trips the state, and processes one block of silence and
eight of noise. The run log is `spike/vst3/run-compressor-stereo.txt`, and `spike/vst3/README.md` has the
reproduction steps.

| step | result |
|---|---|
| module | `ModuleEntry` ok; 388 classes (194 audio modules, 194 controllers) |
| component and controller | a separate controller (`getControllerClassId`); both `initialize` ok; `IConnectionPoint` connected both ways |
| buses | audio in 0 "stereo_in", 2 ch, main; audio out 0 "stereo_out", 2 ch, main. "Sidechain Compressor Stereo" adds audio in 1 "sidechain_in", 2 ch, **aux**, and processes with only the main bus handed over |
| parameters | 48 ("Bypass" flagged `kIsBypass`, flags 0x10001); names, units and default text through `getParamStringByValue` |
| state | 517 bytes; `setState` then `getState` byte-identical |
| process | silence in, silence out (−inf dBFS). Noise in at −6.04 dBFS peak / −10.84 dBFS RMS, out at −6.57 / −11.37 (left channel). Latency 0, tail 0. `setProcessing` returned `kNotImplemented`, which the specification allows |
| C++ needed | **none**: C11 and `vst3_c_api.h` |
| lines | 533 lines, 477 of code (scc 3.5.0), 467 NLOC in 40 functions (lizard). The COM objects are 257 code lines: `IHostApplication` 18, `IMessage` + `IAttributeList` 147, `IBStream` 56, empty `IParameterChanges` + `IEventList` 36. The driving sequence is 160 |

The time box was about two hours, most of it spent on three plugin quirks. Each is a rule the adapter would carry:

1. **The host must allocate `IMessage`.** LSP's `setState` asks `IHostApplication::createInstance` for an `IMessage`
   and crashes when refused. The SDK's C++ hosting library hides this; in C it is 147 lines.
2. **Restore state only after `setupProcessing`.** The specification allows `setState` from "Initialized"
   (`vst/ivstcomponent.h:199`). LSP 1.2.29 divides by its still-zero rate there (SIGFPE, `div %r8` at
   `lsp-plugins.so+0x3b55e6`).
3. **A crashed instance poisons the next.** LSP keeps a catalog in `/dev/shm/lsp-catalog-<user>.shm`. The SIGFPE run
   left it at 0 bytes, and every later instance died of SIGBUS on a background thread at `setupProcessing`, while
   reading the catalog header (`cmp $0x53434154`, "SCAT", at `lsp-plugins.so+0x2ddceb`). `: >
   /dev/shm/lsp-catalog-$USER.shm` alone reproduces it (exit 135), and removing the file fixes it (exit 0 three times
   out of three). Hosting in-process inherits such crashes. Running each plugin in its own worker process, as
   plugin-hostd does (`README.md:4`), contains them.

Steinberg's own `validator` (vst3sdk 3.8.1: `LD_LIBRARY_PATH=… validator lsp-plugins.vst3`) did worse in this headless
pod. On the first class it failed "Scan Editor Classes" (`ERROR: IPluginBase::initialize (..) failed.`), then died of
SIGSEGV in "Valid State Transition 32bits" (exit 139). It cannot be our oracle for LSP on CI; our own probe passed the
same class.

### 2.3 The test plan

- **Fixtures.** One small VST3 plugin per behaviour, written in C against `vst3_c_api.h` (a factory, a component, a
  processor and a controller, about 600 NLOC shared), as `lv2/tests/fixtures` does for LV2. CI stays hermetic and
  the adapter's refusals are exercised on purpose.
- **Real plugins.** The VST3 builds in Fedora 44 and audinux: lsp-plugins-vst3 (Fedora; 194 audio classes),
  `vst3-surge-xt`, `vst3-dexed`, `vst3-odin2`, `vst3-airwin2rack` and the `vst3-zl-*` family (audinux;
  `dnf list --available 'vst3-*'`). The issue also names Roth-Air, SwankyAmp and vitalium (on msi); they join the same
  run.
- **Parity.** The same plugin through two adapters. LSP ships the same effects as LV2 (`lsp-plugins-lv2`, installed
  here) and CLAP (`lsp-plugins-clap`). For each LSP effect: instantiate the VST3, LV2 and CLAP builds through
  omx-clap-core at 48 kHz and a 512-frame block, feed the same seeded noise and a sine sweep, and compare outputs
  sample by sample. Pass when the residual is at least 100 dB below the signal for identical parameter values, or when
  a measured difference (a latency, a denormal floor) is named. The parameter map is checked the same way: the count,
  names, ranges and default values through each adapter.
- **Real-time.** The allocation witness of `lv2/tests/lv2_run_test.c`, which wraps malloc, calloc, realloc and free,
  runs around the VST3 `process` path too.
- **Crash containment.** Quirk 3 becomes a regression test: a 0-byte catalog file must give a refusal with a code
  from the worker process, never a dead engine.

### 2.4 Recommendation

**Not now.**

1. **Demand.** In Fedora 44 plus audinux there are 248 `vst3-*` plugin packages. 183 have an LV2 or CLAP package of
   the same name, 1 ships LV2 in its base package (camomile), and **64 are VST3-only**
   (`tools/adapter-seam.sh --vst3-packages`, by name and file list). They are open-source titles (paulxstretch,
   vmpc, cabbage, the zl-* compressors, and others), and none is a plugin our work depends on. The list is an upper
   bound: guitarix, for one, ships its effects as separate `lv2-Gx*` packages that a name match misses. Among
   closed-source vendors, 6 are VST3-only on Linux (3.2), and none of their plugins is asked for by an issue or a
   user.
2. **Cost.** About 2200 NLOC on the format side, which is about 3600 lines (at 0990c30 `lv2/` has 1.62 lines per NLOC:
   7851 lines, 4833 NLOC). On top come about 2000 lines of tests (there, the LV2 tests are 7566 lines against 7851 of
   adapter, `wc -l lv2/tests/*`) and about 600 of C fixtures: **about 6200 lines** in all. The adapter code of `lv2/`
   grew from 2871 to 7851 lines between 2026-10-07 and 2026-10-20 (`git ls-tree` and `wc -l` at 5da3b2e and 0990c30),
   partly by moving code rather than writing it. At that pace the format side is about 2 weeks. With tests and fixtures
   in parallel lanes, the adapter is **about 2.5 weeks of lane time**. Cutting the seam first (moving 1161 NLOC behind
   about 25 hooks, with the LV2 tests as the oracle) adds **about half a week**, so about 3 weeks in all.
3. **Does it validate the abstraction?** Partly, and that part was measured. Every VST3 task above maps onto an
   existing shim function group, so the seam drawn in 1.5 holds for the CLAP face. It does not hold for `lv2/core`:
   its neutral code is LV2-motivated, and VST3 would use only `omx_msgring`. Writing the adapter would confirm the
   shim seam, not discover a new one. That confirmation is already on paper for the price of this report and the
   spike.

What to do instead. Land #280, whose helpers are the first piece of the shared shim. Treat #279 and #281 as LV2
cleanups on their own merits. Keep new code in `lv2_clap.c` free of `lv2_*` calls in its neutral functions, so the seam
stays cheap to cut. Keep the spike branch.

## 3. Could we host it?

### 3.1 Yes or no, per area

| area | can we? | evidence |
|---|---|---|
| licence of the SDK | **yes** | MIT (quoted in 2.1); the C API is BSD-3-Clause. Both combine into GPL-3.0-or-later. "VST" may not appear in a product name |
| C++ / COM requirement | **yes, in C** | `vst3_c_api.h` 3.8.1; the spike is C11, 477 code lines, no libstdc++ |
| real-time rules | **yes** | `process` on the processing thread; the host preallocates `IParameterChanges` and `IEventList` (the spike's are static). Our allocation witness applies as for LV2. A plugin that allocates in `process` is its own fault, as in LV2 and CLAP |
| threading | **yes** | the "UI-thread" calls map to our main thread; no worker; `setProcessing` may run on either thread (`vst/ivstaudioprocessor.h:342`) |
| headless (no display) | **yes for LSP, not for every plugin** | the spike ran headless; Steinberg's validator failed `IPluginBase::initialize` on a controller in the same pod (2.2) |
| activation of closed-source plugins | **depends on the vendor** | serial or key-file activation works headless (u-he, AudioThing, Renoise, DDMF). Activation inside the plugin's editor (Loomer) needs one GUI session on a desktop machine first. iLok (Tracktion BioTek/F'em) has no Linux client, unless the vendor's Machine-ID option applies |
| EULA | **depends on the vendor** | no EULA found forbids loading in a third-party host. One-licence-per-person terms (AudioThing) and a ban on reverse engineering (Vital, Tracktion) bind our test rig as they bind any user |
| crash containment | **yes** | plugin-hostd runs each plugin in a worker process of its own (`README.md:4`); quirk 3 shows why that matters |
| Windows plugins through Wine / yabridge | **out of scope** | not considered: no native Linux build, no hosting |

**Keeping the results.** The spike stays on `spike/vst3-feasibility` (2b3cc43): source, build, run logs and README.
This report and `tools/adapter-seam.sh` stay on main. A later adapter starts from those three.

### 3.2 Closed-source vendors with Linux VST3 builds

This is a web survey of each vendor's own pages on 2026-10-20. Re-check before acting on it: vendors change formats
and terms.

| vendor | Linux products (examples) | Linux formats | VST3-only on Linux | activation | EULA / headless notes | sources |
|---|---|---|---|---|---|---|
| u-he | Diva, Zebra, Hive, Repro | VST3, CLAP | no | serial and offline key file | "You don't need an active Internet connection to install, activate or use our products." | u-he.com/support/faq/licenses/ |
| TAL Software | TAL-Sampler, TAL-U-No-LX, TAL-BassLine-101 | VST, VST3, CLAP | no | serial, up to 4 activations | EULA not found | tal-software.com |
| ToneBoosters | the whole range | VST, VST3 | **yes** | perpetual licence, up to 4 computers | EULA not found | toneboosters.com |
| Modartt | Pianoteq, Organteq | standalone, VST3, LV2 | no | serial, 3 activation slots | EULA not found | modartt.com |
| Audio Damage | Phosphor 3, Eos 2, Kombinat4 | VST3, CLAP, LV2 | no | none: "NO DONGLES, NO DRM, NO SUBSCRIPTIONS" | no licence code | audiodamage.com |
| discoDSP | OB-Xd, Corona, Bliss, Discovery | VST3 (OB-Xd also LV2) | per product | one-time online activation, then offline | "A one-time internet connection is required to activate the software license." | discodsp.com |
| Loomer | Aspect, String, Manifold, Resound | VST, VST3 | **yes** | licence key entered in the plugin's GUI | headless hosts cannot activate | loomer.co.uk |
| Auburn Sounds | Graillon 3, Couture, Panagement, LENS | VST, VST3, CLAP, LV2 | no | serial; iLok optional for one product | demo "for evaluation purposes only" | auburnsounds.com/terms.html |
| Renoise | Redux | VST3 | **yes** | licence file tied to the person | not machine-locked | renoise.com/license-agreement |
| Tracktion | BioTek 3, F'em | VST3 | **yes** | **iLok** (Machine-ID possible) | "licensed to You, not sold"; no reverse engineering; user reports of Linux instability | tracktion.zendesk.com, kvraudio.com |
| AudioThing | Reels, Materia, Wires, Filterjam | VST2, VST3, CLAP | no | key file, online or offline, 3 computers | "one-license-per-person basis" | audiothing.net/eula/ |
| Bertom Audio | Denoiser Classic, Air Shelf, Phantom Center | VST3 | **yes** | pay-what-you-want, no activation described | EULA not found | bertomaudio.com |
| DDMF | IIEQPro, Comprezzore, MagicDeathEye | VST3 | **yes** | serial, licence in the person's name | no host clause found | ddmf.eu/legal.html |
| Vital Audio | Vital | VST, VST3, LV2 | no | account download | "You may not reverse engineer, decompile, or disassemble" | vital.audio/eula/ |

No native Linux build, per each vendor's own site or forum: Kilohearts, MeldaProduction, Klevgrand, KV331 Audio,
Analog Obsession, Sonic Academy, Ample Sound, FabFilter, Valhalla DSP, Native Instruments, Waves, Arturia. Overtone DSP
had Linux builds but has stopped selling.

Tally: 14 vendors ship Linux VST3; 8 of them also ship CLAP or LV2, which we already host. The **6 VST3-only** are
ToneBoosters, Loomer, Renoise (Redux), Tracktion (BioTek 3, F'em), Bertom Audio and DDMF.

### 3.3 The trigger that reopens the decision

Any one of these reopens it:

1. **A named VST3-only plugin we would use.** An issue names a plugin with a Linux x86_64 VST3 build, no LV2 or CLAP
   build, activation that works without its editor (serial or key file, no iLok without Machine-ID), and an EULA that
   allows loading in a third-party host. Three such plugins asked for make the case; one is enough if it fills a role
   nothing we host fills.
2. **A second non-CLAP adapter is approved for another reason.** Then cut the seam (1.5) first, whatever the format.
3. **A dual-format vendor drops CLAP and LV2 on Linux** for a plugin we host today.

The re-check is `tools/adapter-seam.sh --vst3-packages` for the repositories, and section 3.2 for the vendors.

## 4. Follow-ups

Filed against this repository, each linked from #287:

- #332, lv2: `write_controls` and `midi_out_event` become direct calls, removing the shim's two indirect calls on the audio
  path (1.5); the objdump count is the test.
- #333, one SHA-256 for omx-clap-core and plugin-hostd: `pc_sha256_*` (`src/param_contract.h`) and `phd_sha256_*`
  (`include/plugin-hostd/pin.h:55-150`) are the same FIPS 180-4 code twice.
- #334, seam: when a second adapter is approved, move the 1161 reusable NLOC of `lv2_clap.c` into the shared shim (1.5),
  with #280's helpers first. This one is parked behind the trigger of 3.3.

## Reproduce

    tools/adapter-seam.sh                   # Part 1's tables, at the table's ref 00a9b2e (needs lizard)
    tools/adapter-seam.sh --list            # every function with its class, area and thread
    tools/adapter-seam.sh --vst3-packages   # the repository count of 2.4 (needs dnf with audinux enabled)
    make lv2/clap/lv2_clap.o lv2/core/lv2_instance.o && objdump -d -l --no-show-raw-insn lv2/clap/lv2_clap.o   # 1.5
    git fetch origin spike/vst3-feasibility && git show origin/spike/vst3-feasibility:spike/vst3/README.md      # 2.2
