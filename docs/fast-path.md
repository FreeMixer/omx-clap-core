<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> -->
# The per-block fast path

What a hosted plugin costs per block comes from the same few places in every format: audio copied into and out of the
host, a clamp written over every output sample of every member, and event lists built when there are no events. This
document is the design for removing that cost, for CLAP plugins, for LV2 plugins through the adapter in `lv2/`, and for a
chain that mixes both. It has four items, built in this order, each in its own change, each with the proof at the end:

1. The LV2 adapter copies no audio.
2. A chain runs its members on one another's buffers.
3. A block with no events does no event work.
4. One clamp per chain.

For where the cost sits today, see the technical note
[The LV2 adapter against mod-host and jalv](https://openmixer.org/notes/the-lv2-adapter-against-mod-host-and-jalv).

## What the stage promises today, and what replaces it

`src/clap_stage.h` opens with "Out of place, always": the lane is copied into private bounce inputs, the plugin writes
private bounce outputs, and `omx_hosted_stage_init` refuses a bounce whose four buffers are not distinct. That promise
gives three things, and each must survive it:

- a plugin that cannot process in place is never asked to;
- the dry block is at hand whenever the output turns out to be unusable, or a change of body needs a crossfade;
- nothing a plugin does to its input reaches the caller's buffers.

From item 2 on, the first line of the stage header reads instead:

> **In place only where declared; a copy only where something needs one.** A plugin's input and output are distinct
> buffers unless its main ports declare `in_place_pair`. Its input is the buffer it is handed, the lane or the previous
> member's output, and its output is the buffer the next reader takes, with no private copy on either side. The dry block
> is kept as "The dry block" below says.

The bounce buffers stay: a transition block, a lane that must be written back and a plugin narrower than its lane still
use them, and `omx_hosted_stage_init` still requires them distinct. Item 1 changes none of this: under it the stage still
copies, and the LV2 adapter connects to what the stage hands it.

## Item 1: the LV2 adapter copies no audio

Each block, the adapter connects the plugin's audio ports (main and auxiliary) to the buffers in `clap_process_t` and runs
it, as jalv connects a JACK buffer every cycle. `connect_port` is called when a port's buffer differs from the one it
holds, not on every block: a port already on the right buffer costs one comparison, and LV2 allows both. A plugin is told
of a new buffer before the `run()` that uses it, so a plugin that keeps the pointer it was given is correct.

A port stays on memory the instance owns, copied as it is today, in these cases.

| case | why | what happens |
|---|---|---|
| the plugin does not state `lv2:hardRTCapable` | LV2 only promises `connect_port` is safe on the audio thread for those that do | all its audio ports connected once, on the main thread |
| the plugin has a worker interface | a plugin may read a port's memory from `work()`, between two `run()` calls, and the host's buffer is not held for it then | as above |
| the plugin requires or supports `lv2:inPlaceBroken`, and an output would alias an input | LV2's meaning of the flag | that output goes on the instance's memory and is copied out after `run()`; its inputs stay on the host's buffers |
| an output would alias another output, or alias an input of another channel, or partly overlap any input, whatever the plugin states | no plugin is written for it, and the result depends on the plugin's write order | that output goes on the instance's memory |
| the host hands no buffer for a channel | a plugin may dereference any port it declares | instance's memory: silence on an input, discarded on an output |
| the buffer is not aligned to 16 bytes | the instance's memory always was, and a plugin may rely on what it has always been given | instance's memory |
| a control, atom, event or CV port | never audio: CV is copied as before, and its one user in a block is the plugin | connected once, as today |

An output channel past the plugin's own (a mono plugin on a stereo buffer) is still copied from its leg 0: no plugin
port writes it.

The main thread ends the arrangement: `lv2_instance_deactivate` puts every audio port back on the instance's memory
before LV2 `deactivate`, since the host's buffers need not outlive processing. `activate`, re-instantiation and a rate
change start from the instance's memory too. No allocation or lock is added on the audio thread.

What a plugin may not rely on any more, none of it promised by LV2, is that its output memory still holds last block's
samples, that writing to an input is harmless, and that its output memory is written by nobody else. Plugins that did
would differ before and after: `tests/lv2_direct_identity.sh` renders every installed plugin through both builds and lists
them, and overwrites the output buffer before each block so such a plugin cannot hide.

## Item 2: a chain runs its members on one another's buffers

`omx_clap_chain_process` already walks two blocks in turn, each member's output the next one's input, but each member
then copies through its stage's bounce (`omx_clap_run_ports_locked`). The stage stops copying and hands `from` and `to`
in `clap_audio_buffer_t` directly, by writing the channel pointers of the process structure for the block. The rules are
the same for both formats, and a mixed chain needs nothing more.

- **CLAP:** in place only when `audio-ports` gives the main input port and main output port as an `in_place_pair`. Any
  other member gets distinct buffers, its input still being the previous member's output, with no copy.
- **LV2:** the adapter declares the pair on its main ports only for a plugin item 1 connects directly and that is not
  `lv2:inPlaceBroken`. Before it does, the aliased arm of `tests/lv2_direct_identity.sh` runs over every installed plugin,
  because a plugin can be wrong in place without saying so; those it finds are listed in `docs/lv2-compatibility.md` and
  stay out of place.
- **Where the chain can do it:** in place needs a buffer the chain owns as both input and output, so only a member between
  the first and the last can. The first reads the caller's input, which it may not write, and the last writes the caller's
  output. After an in-place member the walk does not change blocks.
- A mono member keeps its sum and its two copies: it has no other way onto a wider chain. A bypassed member forwards the
  block pointer and costs nothing.

### The dry block

The wet/dry crossfade and a discarded block both need the input as it was before the plugin ran. It is kept like this.

- **An out-of-place member needs no copy.** Its input buffer is still intact when it returns. A crossfade writes into the
  output from it, and a discarded block copies it over the output, on that block only.
- **An in-place member on a transition block is run out of place,** on the stage's bounce output. Whether a block is a
  transition is known before `process()`: the commanded body differs from `rendered_wet`. A plugin that declares
  `in_place_pair` must handle distinct buffers as well, so this costs one crossfade and no copy of the dry block.
- **An in-place member's discarded block has no dry block.** A non-finite output or `CLAP_PROCESS_ERROR` is only known
  afterwards, by when the input is gone. That block is written as silence, never as what the plugin left. It is counted
  and struck exactly as today, and the next block fades back in. The lane is finite and bounded; it is one block of
  silence where today it is one dry block, on a plugin already failing.

### What the plugin can touch

The first member of a chain now reads the caller's buffer, which is declared `const`. A plugin that writes its input, out
of contract in CLAP and LV2 alike, would write the caller's buffer, where today it only writes the bounce. A member later
in the walk writes only the chain's own block. `tests/lv2_direct_identity.sh` reports any LV2 plugin that does this; the
stage keeps the bounce for a member named in the same list, at the cost of the copy.

## Item 3: a block with no events does no event work

- **CLAP:** when the stage's drain finds nothing in the ring and the block holds no notes, it hands `process()` one shared,
  read-only, empty `clap_input_events_t`, with `size` answering 0 and no context, and skips building, sorting and draining
  the list. One shared list suits every instance because it holds no state. The output sink is untouched: a plugin's own
  output events are still taken.
- **LV2:** when the input list is empty the adapter skips the CLAP-event-to-control-port walk: every control port keeps
  its value, which is what an empty walk produces, and only the per-block duties the walk does not own stay (trigger reset,
  an atom input's empty sequence header, the time port).
- Smoothing inside a plugin is the plugin's and is not touched.

## Item 4: one clamp per chain

Today every member's output is scanned for non-finite samples, then clamped, in two passes, the second of them writing
every sample. Taken away without a replacement, that is unsafe: a NaN from one member would enter a later member with
state, a reverb or a delay, and stay in its feedback for good. A finite but enormous sample does the same in a filter that
squares it. The chain's last clamp cannot undo either.

So the guard stays per member, but as a per-block flag, not a per-sample write.

- After each member, one read-only pass over its output, `omx_hosted_in_bounds`: does every sample satisfy
  `|x| <= ceiling`? One comparison per sample, written so it vectorises, with no store. A NaN fails the comparison as well,
  so this one pass replaces both of today's.
- **In bounds, the usual case:** nothing is written, and the next member reads the block as it stands. This is exactly what
  the clamp would have left, because the clamp changes no in-bounds sample.
- **Out of bounds:** today's two passes run, on this member's block and on this block only. Non-finite: discarded, counted
  and struck. Finite above the ceiling: clamped and counted. So the next member reads what it reads today, and every
  counter reads what it reads today.
- So no later member ever receives a sample the per-member clamp would not have let through. The chain's output is held by
  the same check on its last member: that is the "one clamp" a block runs at most for each member at fault, and none for a
  healthy chain.
- Mixed chains need nothing more: the check sits in the stage, which both formats go through.

A member may skip the pass only if it has declared finite, bounded output through the plugin's own contract, which is
reserved for this library's own plugins and is not part of this item.

## An empty event list, a clamp and a crossfade on the same block

They compose without any special case. A transition block runs out of place, the check runs on its output, the crossfade
reads the dry block, and the event list is whichever the drain chose. An out-of-bounds transition block is discarded or
clamped first, as it is today.

## Proof each item owes

- Outputs bit-identical to before: the oracle suites (the LV2 and stage tests), and
  `tests/lv2_direct_identity.sh` on every installed plugin for items that touch the LV2 path.
- The sabotage of each new guard goes red.
- Nanoseconds per sample before and after, 48000 Hz, 256 frames, a 24-plugin chain, CLAP, LV2 and mixed
  (`tests/hosted_cost_bench.c`), on x86 and on the Pi.
- Item 2: a plugin with no `in_place_pair` never sees one buffer as input and output; a transition block keeps the
  crossfade's exact samples; a discarded in-place block is silence, struck once.
- Item 3: a parameter arriving after any run of empty blocks takes effect on the same block as today.
- Item 4: NaN and a huge finite value injected mid-chain ahead of a stateful member leave that member's state finite, and
  the test goes red with the per-member check removed.
