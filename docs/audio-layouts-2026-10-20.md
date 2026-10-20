Audio layouts: every port a plugin declares, described; the carrier judges the fit
================================================================================

Status: design, 2026-10-20, for issue #92 under the operator's ruling of the same day (support all LV2 and CLAP
plugins; develop what is missing). Builds on #183/#184 (pass-through tools, sources) and the instruments spec of
openmixer (`docs/design/specs/2026-10-06-instruments-on-channels.md`, §7.2 ruled 2026-10-19).

Intent
------

Today the host admits one main input and one main output of the same width, mono or stereo, plus up to eight
auxiliary inputs and outputs of one or two channels, and refuses everything else with two codes:
`hosting.topology.wider-than-strip` and `hosting.topology.extra-inputs-fed-silence`. On the desk's 975 LV2 plugins
that refuses 84 that mod-host hosts: 51 with inputs beyond the main pair (side chains, mixers, meters, matrix
mixers, patchbays, up to 64 inputs) and 33 whose main pair differs in width or whose outputs are more than the pair
(mono reverbs with a stereo tail, crossovers with band outputs, a 34-output instrument). A CLAP plugin with a
surround or ambisonic main port is refused the same way, and the host never reads the extensions that describe
such ports or offer alternative layouts.

The ruling: every layout a plugin declares is supported. Inputs beyond the main one are the host's auxiliary
inputs, silent until something binds them. A main port of any width is carried where the carrier has that many
channels. A layout the carrier cannot carry is refused with a named code, never silently and never folded down.
A mono output on a stereo strip is carried to both sides, as a mono input is (§7.2).

The principle that makes this small enough: **the library describes, the carrier judges.** `libomx-clap-core`
and the LV2 adapter admit any audio layout within declared limits and describe it completely (every port, its
channels, its kind, its channel map). Whoever runs the plugin on a lane of a given width, the console's native
lane or omx-clap-host under JACK, reads that description and decides what it can carry. The console already does
this for the width it knows (`insertWidthFits`, `destination-suitability`); omx-clap-host under JACK can carry any
width as ports.

Goals
-----

1. The 84 refused desk plugins are hosted through the adapter, measured with the lane coverage runner.
2. A CLAP plugin whose main port is surround (any of the twenty CLAP channel ids) or ambisonic (any order, FuMa or
   ACN, any normalisation) is admitted with its map or configuration in the description.
3. A plugin that offers alternative layouts (`audio-ports-config`, `configurable-audio-ports`) can be asked for
   the layout the carrier wants before activation; a plugin that cannot offer it keeps its declared layout and
   the carrier judges that.
4. Every refusal is a code in `data/hosting.json`, rendered, named in the PR's Codes line and posted to
   omx-contract#119's vocabulary.
5. Nothing the existing carriers read changes meaning: a mono or stereo plugin on a stereo lane behaves as today,
   bit-identically; the JACK port names of the main pair stay `in_<k>`, `out_<k>`.

Non-goals
---------

- Carrying a main wider than two channels through the console's native lane or its strips: the console is two
  channels inside (`stereo-support.md`); its admission refuses such a plugin with `hosting.topology.wider-than-strip`
  as it does now. The bus widths of a future surround console are that console's design.
- Binding auxiliary inputs from the console's UI (patching a side chain): a console feature. This note gives the library API and the JACK ports; what connects to them is the
  carrier's.
- Live layout changes: a plugin that rescans its ports while active is restarted into the new judgment, as today.
- ev:EventPort (#52), control-only plugins and the ll-plugins bundles: their own lanes, in that order, after this.

Anti-goals
----------

- Folding a layout down silently (a 5.1 plugin heard as its first two channels): refused by name instead.
- Guessing a layout the plugin did not declare: the rules below are deterministic from what the plugin says, and
  a plugin that means a wider main declares a port group.
- Rewording an existing code: `wider-than-strip` keeps its text; it changes who raises it.

The model
---------

A plugin's audio layout, each side (input, output), is an ordered list of ports:

| field | CLAP source | LV2 source |
|---|---|---|
| `id` | `clap_audio_port_info.id` | the port index of the group's first channel |
| `name` | `info.name` | the group's `lv2:name`/`rdfs:label`, else the port's `lv2:name` |
| `main` | `CLAP_AUDIO_PORT_IS_MAIN` | the rules below |
| `channels` | `info.channel_count` | the group's member count, 1 for an ungrouped port |
| `kind` | `info.port_type`: mono, stereo, surround, ambisonic, or unknown (NULL) | from the group type (table below) |
| `map[]` | `clap.surround/4` `get_channel_map`, for a surround port | from the group type (table below) |
| `ambisonic` | `clap.ambisonic/3` `get_config`: ordering, normalisation | from the group type (table below): ACN, normalisation assumed SN3D |
| `in_place_pair` | `info.in_place_pair` | `CLAP_INVALID_ID` |

Limits, declared in `data/hosting.json` `HOSTED_STAGE_LIMITS` and rendered: `portChannels` 64 (a port's channel
count), `auxInputs` 64 and `auxOutputs` 64 (ports beyond the main one, up from 8). A side with more aux ports is
refused `hosting.topology.too-many-ports`; a port of more channels `hosting.topology.port-too-wide`; a port of
zero channels `hosting.topology.port-without-channels`. The 64s are what the desk measured plus a margin
(patchbay64, lufsmetermultichannel 24, yoshimi 34 outputs, multisampler 98 outputs as 49 stereo groups) and are
the only numbers in this design.

There is exactly one main port per side, or none: a plugin with no main input is a source (#184); an LV2
plugin with no main output is a pass-through tool (#183, the shim's own process copies the input through). A
native CLAP plugin with aux outputs and no main output keeps today's refusal, `hosting.topology.no-audio-output`:
no desk plugin has that shape, and carrying the input through would be new real-time code in the core, which is
its own issue (#211), not this lane's.

The main pair may differ in width. `in->in_channels` and `in->channels` are already two fields; the refusal
`main_in != main_out` goes. On a two-leg lane: a mono input reads the left leg (as a mono plugin does today), a
stereo output writes both legs, a mono output writes both legs (§7.2), a stereo input reads both. The library's
two-leg run (`omx_clap_host_run(in, l, r, n)`) keeps its signature and does this; it refuses at activation, with
`wider-than-strip`, a plugin whose main port is wider than two on either side, since that lane is the two-leg
carrier. A new `omx_clap_host_run_ports(in, const float *const *ins, float *const *outs, n)` runs any width:
`ins` holds one buffer per main input channel, `outs` one per main output channel, counts from the description.
omx-clap-host uses that one.

How activation knows the carrier: `struct omx_clap_host_config` gains `lane_channels`, the width of the lane the
caller runs the instance on: 2 is the default and what the console's native lane gets without a change of its
own; 0 says the caller carries any width as ports (omx-clap-host sets it). Activation refuses
`hosting.topology.wider-than-strip` when either main port is wider than `lane_channels`, so the live console
stays safe with the library alone, and the ports carrier admits everything.

Buffers: the single mmap'd bounce grows from eight blocks to what the description needs: two silence blocks shared
by every unbound aux input channel, one scratch block per aux output channel, and one block per main channel each
side. Allocated at activation from the description, never on the process thread.

Aux inputs: `omx_clap_host_bind_aux_input(in, port, bufs[], channels, frames)` takes one buffer per channel
(today `l, r`). Unbound reads silence. Aux outputs are handed their scratch and otherwise unconnected, as today;
`omx_clap_host_bind_aux_output(in, port, bufs[], channels)` lets a carrier read them (the band outputs of a
crossover, the per-track outputs of an instrument). omx-clap-host registers a JACK port per aux channel.

LV2: which ports are main, which are auxiliary
----------------------------------------------

Read, per side: `pg:mainInput`/`pg:mainOutput` on the plugin, `pg:group` on each audio port, `lv2:isSideChain`,
`pg:sideChainOf`, and the group's `rdf:type`. Then, per side:

1. **A main group is declared** (`pg:mainInput`/`pg:mainOutput`): that group is the main port, its member count
   the channels, its type the kind. Every other audio port of the side is auxiliary: grouped ports one aux port
   per group, ungrouped ports one mono aux port each. (lsp mixers, ab_tester, oscilloscope, spectrum analyzers,
   true-stereo reverbs, crossovers, calf XOver, the 12 side-chain compressors.)
2. **No main group, but groups**: the first group in port order is the main port; the rest and every ungrouped
   port are auxiliary as in 1. (lsp ab_tester_x2_mono: two mono groups, main a, aux b.)
3. **No groups at all**: when the side has at most two ports, they are the main port (one mono, two stereo), as
   today. When it has more than two and the other side has exactly as many, the side is one main port of that
   width (gareus surround meters, sisco, matrixmixer i8o8, essp, lufsmetermultichannel, patchbay N→N, mixtri,
   noise_generator_x4). Otherwise the first two ports are the main port and the rest are mono aux ports (xfade
   4→2: main a pair, aux b pair; sc3 3→2: main a pair, aux the side chain; bodeShifterCV 2→3: main pair out, aux
   the shift; surroundEncoder 4→2: main pair, aux C and S). A plugin that means a wider main declares a group.
4. Side-chain ports (`isSideChain`/`sideChainOf`) are auxiliary whatever rule applies, grouped by their group as
   today.
5. CV ports stay what they are: mono aux ports after the audio ones.

A mono input and a stereo output are two main ports of different widths, admitted (rule 1 for the 13 true-stereo
reverbs, rule 3 for the 6 swh splitters: their two outputs are the main pair, which is what mod-host presents too).

Group types map to kinds and channel maps. The members of a typed group are ordered by the group type's elements
(each member port's `lv2:designation`), which is also the order of the map; a member without a designation, or
a group of a type the table lacks, orders by port index with kind unknown. The CLAP surround ids are
`port-groups.ttl`'s elements in index order:

| LV2 group type | kind | channels | map (CLAP surround ids, in the group's element order) |
|---|---|---|---|
| `pg:MonoGroup` | mono | 1 | |
| `pg:StereoGroup` | stereo | 2 | FL FR |
| `pg:MidSideGroup` | stereo | 2 | FC + side: carried as a pair, the designations in the description |
| `pg:ThreePointZeroGroup` | surround | 3 | FL FR BC |
| `pg:FourPointZeroGroup` | surround | 4 | FL FC FR BC |
| `pg:FivePointZeroGroup` | surround | 5 | FL FC FR BL BR |
| `pg:FivePointOneGroup` | surround | 6 | FL FC FR BL BR LFE |
| `pg:SixPointOneGroup` | surround | 7 | FL FC FR SL SR BC LFE |
| `pg:SevenPointOneGroup` | surround | 8 | FL FC FR SL SR BL BR LFE |
| `pg:SevenPointOneWideGroup` | surround | 8 | FL FLC FC FRC FR BL BR LFE |
| `pg:AmbisonicBH1P1Group`, `BH2P2Group`, `BH3P3Group` | ambisonic | 4, 9, 16 | ordering ACN (the elements are ACN0..ACN(n-1) in order) |
| the other `pg:AmbisonicBH<h>P<p>Group`s (BH1P0 3, BH2P0 5, BH2P1 6, BH3P0 7, BH3P1 8, BH3P2 11) | unknown | the element count | mixed-order streams a CLAP ambisonic port cannot express: carried as discrete channels, the ACN designations in the description |
| `pg:DiscreteGroup`, `pg:InputGroup`, `pg:OutputGroup`, an untyped group | unknown | member count | none |

LV2 port groups declare no normalisation. A full-order ambisonic group is exposed by the shim as a CLAP
ambisonic port with ordering ACN and normalisation SN3D, the AmbiX convention, recorded below as a decision; the
description says `normalisation: assumed`. No desk plugin declares a surround or ambisonic group today; the
table is exercised by fixtures.

CLAP: the extensions
--------------------

- `clap.audio-ports`: `port_type` and `in_place_pair` are read into the description. Zero channels is refused
  `port-without-channels`. `is_rescan_flag_supported` stays false.
- `clap.surround/4`: for a surround port, `get_channel_map` fills `map[]`; a channel id above `CLAP_SURROUND_TSR`
  is refused `hosting.topology.channel-map-unknown` at open. The library asks `is_channel_mask_supported` only
  inside `request_layout`, with the mask of the map the carrier requests; a `false` there answers the request
  with `hosting.topology.channel-map-unknown` and the declared layout stands. At open there is no carrier to ask
  for, and asking with the plugin's own map proves nothing. The host's `changed()` sets the rescan flag, as a
  port rescan does.
- `clap.ambisonic/3`: for a port whose `port_type` is ambisonic, `get_config` fills ordering and normalisation;
  `(order+1)²` must equal `channels` or the port is refused `port-too-wide` at open (the description is
  inconsistent). `is_config_supported` is asked only inside `request_layout`, with the configuration the carrier
  names; `false` answers the request with `hosting.topology.ambisonic-config`.
- `clap.audio-ports-config` and `clap.configurable-audio-ports/1`: a new call before activation,
  `omx_clap_host_request_layout(in, const clap_audio_port_configuration_request_t *reqs, n)`. The host first tries
  `configurable-audio-ports` (`can_apply_configuration`, then `apply_configuration`), then the config list of
  `audio-ports-config` for one whose main channels and types match every request (`select`), then gives up and
  answers `HOSTING_LAYOUT_DECLARED` (the plugin keeps its layout; the carrier judges it). After a successful
  request the description is read again. omx-clap-host asks for the lane's width (`-c`/the daemon's strip width,
  mono or stereo on both mains) before `add` activates the plugin, so a reverb that offers mono-in/stereo-out and
  stereo-in/stereo-out takes the one the strip has.
- `clap.audio-ports-activation/2` is #190's and is not touched here.

Every new identifier read goes into `doc/reference.md`'s extension table (and #196's page when it exists).

omx-clap-host and plugin-hostd
------------------------------

JACK ports, client `effect_<N>`: the main pair keeps `in_<k>`/`out_<k>` for `k` up to the main channel count (so
a stereo plugin's names are unchanged). Aux port `p` (1-based, in description order) channel `k` is `aux_in_<p>_<k>`
and `aux_out_<p>_<k>`. `omx-clap-scan` and `pin_layout`'s neighbour verb print the description with the names so
a controller can connect by it. plugin-hostd forwards `connect` verbs by name and needs no change; its e2e gains
one scenario: an aux input connected and heard.

Codes
-----

| code | raised by | meaning |
|---|---|---|
| `hosting.topology.wider-than-strip` | activation, against the caller's `lane_channels` (2 by default; 0 for a ports carrier); the console's admission | a main port is wider than the lane; kept, never reworded |
| `hosting.topology.too-many-ports` | the library | more than `auxInputs`/`auxOutputs` ports beyond the main on one side |
| `hosting.topology.port-too-wide` | the library | a port of more than `portChannels` channels, or an ambisonic port whose channels are not `(order+1)²` |
| `hosting.topology.port-without-channels` | the library | a port of zero channels |
| `hosting.topology.channel-map-unknown` | the library at open (an unknown id); `request_layout` (the plugin refuses the carrier's mask) | a surround port names a channel id the host does not know, or refuses the carrier's mask |
| `hosting.topology.ambisonic-config` | `request_layout` only | an ambisonic port refuses the carrier's ordering or normalisation |
| `hosting.topology.extra-inputs-fed-silence` | nobody: retired | aux inputs are admitted up to `auxInputs`; the ninth is no longer a refusal |

The LV2 adapter's `LV2_CODE_EXTRA_INPUTS` and its uses go; `LV2_CODE_WIDER_THAN_STRIP` goes from the adapter (it
no longer judges width). The codes are added to `data/hosting.json` (the vocabulary's home until omx-clap-core#208
reads omx-contract's render) and posted to omx-contract#119 for the vocabulary PRs.

Proof
-----

- Fixtures, LV2 (one twin each in `lv2/tests/fixtures/`): main group plus two stereo aux groups; two mono groups
  without a main; ungrouped 4→4; ungrouped 4→2; mono in with a stereo main-output group; 2→1 ungrouped; main
  pair plus three band output groups; `pg:FivePointOneGroup` 6→6; `pg:AmbisonicBH1P1Group` 4→4 and a mixed `BH2P1Group` 6→6; a 64-port side and a
  65-port side (the limit and the refusal). Each asserts the description (main/aux, channels, kind, map) and a
  value through the ports (a known sample on a given aux input or map channel read at the matching output).
- Fixtures, CLAP (`tests/fake_*.clap`): a 5.1 main with a surround map; an ambisonic main (ACN/SN3D, order 1); an
  aux input of four channels; a plugin with two `audio-ports-config`s (mono-in/stereo-out, stereo/stereo) and one
  with `configurable-audio-ports`; a surround port whose `is_channel_mask_supported` answers false to the mask
  `request_layout` asks with.
- `clap_host_test`: the description of each fake; `run_ports` with six buffers; `run` on the two-leg lane refusing
  the 5.1 fake with `wider-than-strip`; mono-in/stereo-out through `run` writing both legs, stereo-in/mono-out
  writing both legs; `request_layout` selecting the stereo config and the declared fallback.
- JACK e2e (`test-jack-lv2`, `test-jack-synth`): `effect_<N>` has `in_1..in_6`/`out_1..out_6` for the 5.1 fake and
  a signal on `in_1` is heard on `out_1`; `aux_in_1_1` connected to a source changes the output of the side-chain
  fixture; mono-in/stereo-out reverb fixture heard on both outputs from one input.
- Sabotage: restore the `main_in != main_out` refusal (the 19 mono-in/stereo-out plugins red); drop rule 3's
  N→N main (the surround meters red); drop the aux binding (the side-chain e2e red).
- Coverage: the lane runner over the 975, expected 823 + 84 = 907, the delta named plugin by plugin in the PR's
  first comment; what remains below mod-host's 915 is listed with its code.
- Bit-identity: `test-identity-lv2` unchanged for every mono and stereo plugin.

Left to the builder
-------------------

- The internal representation of the description (arrays of a port struct in `omx_clap_host_in` and
  `struct lv2_plugin`) and how the stage/shim bind functions change shape; the `in_ptrs[2]` arrays become
  per-channel arrays sized from the limits.
- The order of the stacked PRs. A sensible one: (1) limits and codes in `data/`, the description model and the
  grown bounce; (2) the main pair of different widths and `run_ports`; (3) the LV2 rules 1–5 and the group-type
  table; (4) surround and ambisonic; (5) `request_layout`; (6) omx-clap-host ports, docs rows, coverage. Each PR
  carries its Codes line and the fixtures it adds; the coverage count goes on the last.
- Whether `aux_in_<p>_<k>` carries the port's name as JACK metadata (`pretty-name`): nice, not required.

Decisions taken here, for the operator to check
-----------------------------------------------

- **A new soname major.** The stage and instance structs resize (`in_ptrs[2]`, `aux_in_bound[8][2]`, the aux
  arrays), and `omx_clap_host_bind_aux_input` changes shape; the repository's own rule (`clap_host.h`,
  `tests/abi-check.sh`, `abi-layout.sh`) calls that a new major: `CORE_MAJOR` 1, `OLDEST` bumped, both baselines
  re-recorded, and the console's `pipewire-native` rebuilt in the same deploy, which the settle gate does. The
  alternative, carrying the old entry points and fixed arrays beside the new, is the back-compat the house rules
  forbid.
- **Base**: the lane stacks on #210's branch (`fix/184-generators-no-input`) until #210 is on main, then main
  is merged in; its first PR edits the same functions and the same codes table, and the coverage baseline is
  #210's 823.
- **`lane_channels`** in the host config, default 2, so the console is safe with the library alone.
- **Masks and configurations are asked only in `request_layout`**, with the carrier's wish; the open-time
  description never asks the plugin whether it likes its own layout.
- **A native CLAP plugin with no main output but aux outputs stays refused** (`no-audio-output`) in this lane.

- Rule 3 for ungrouped ports (N→N is a wide main; otherwise the first two are the main and the rest mono aux).
  mod-host presents ungrouped ports flat, so any rule is ours; this one keeps every N→N multichannel tool whole
  and never widens a main past what the other side has.
- The library stops raising `wider-than-strip`; the two-leg carriers raise it. The console's catalog already
  judges width from `audioOutputs`; it should read the main port's channels instead of the port count (the console's
  admission reads the main port's channel count, not the port count, and names `wider-than-strip` for a main
  wider than the strip; and allows a main input and output of different widths).
- `extra-inputs-fed-silence` is retired rather than kept for the 65th aux port; the new `too-many-ports` says
  what happened.
- The limit 64 everywhere, one number for ports per side and channels per port.
- Aux outputs get a binding API now (crossover bands, per-track instrument outputs) because the JACK host
  exposes them as ports anyway; the console does nothing with them yet.
- An LV2 full-order ambisonic group is presented as a CLAP ambisonic port with ACN/SN3D (AmbiX); mixed-order
  groups are discrete channels with their ACN designations named. LV2 declares no normalisation, so the SN3D is
  an assumption the description marks.

The console's two follow-ups, which this note leaves to the console: its admission reads the main port's channel
count (not the port count) and names `wider-than-strip` for a main wider than the strip, and allows a main input and
output of different widths; and binding auxiliary inputs from the console's UI (patching a side chain), a console
feature (a strip's insert exposes its aux inputs for patching, unpatched = silence).
