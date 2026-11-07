<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> -->
# Meters: one extension for one fact

A plugin's meters (its gain reduction, its levels) are one fact, and two CLAP extensions carried it:
`org.openmixer.meters/1`, this repository's vendor extension (`omx_clap_ext.h`), and `clap.meters/1`, the CLAP
meters extension (`include/clap/ext/draft/meters.h`, proposed to the CLAP SDK in free-audio/clap#521). The host read
only the vendor one. A plugin that wanted every host to draw its meters had to serve both and publish each number
twice.

## Decision

`clap.meters/1` stays. `org.openmixer.meters/1` is removed: the host no longer asks for it, and `omx_clap_ext.h` no
longer declares it. It is not kept as an alias, read second or under another name: a plugin that offers only the
vendor extension has no meters here.

Why `clap.meters/1`:

- It carries everything the vendor extension did: a stable id per meter, a name, a kind (a gain reduction in dB
  with CLAP's sign, a level in dBFS), a channel count, and a read that is safe from any thread.
- It carries what the vendor extension lacked: a display range, a flag promising that every channel of one read
  comes from the same block, an `other` kind, and a host callback for a meter list that changes.
- Any CLAP host can read it. The rule `omx_clap_ext.h` already states holds for meters too: what CLAP covers is
  read through CLAP. A vendor extension is for what CLAP has no word for, and CLAP has one for meters.

Why not the vendor one: only OpenMixer's plugins speak it, it has no display range and no coherence promise, and
keeping it beside the standard one leaves two answers to one question with the host choosing between them.

## What the host reads

For each plugin, at load, in this order:

1. `clap.meters/1`: every channel of every meter is an output symbol (the naming rule in `doc/reference.md` is
   unchanged). At the host read rate (below) the host calls `get_values()` once per monitored meter and sends the
   channels it wrote; a call that writes none (the plugin has not processed since it was activated) sends nothing.
2. Only when the plugin has no `clap.meters/1`: `clap.gain-adjustment-metering/0`, as before.

`get_values()` returns doubles; `output_set` carries the value as a float, as it carries every output. The display
range and the flags are the plugin's to declare and the console's to draw: mod-host's `output_set` has no field
for them, so the host does not forward them.

## Held meters: the plugin holds, the reader recovers

A meter is read between blocks, so what it shows depends on who keeps the value until the read. Two halves, and each
has one owner:

- **The plugin holds.** A meter whose `flags` carry `OMX_CLAP_METER_IS_HELD` (bit 8, `src/clap_host.h`) keeps the block
  value until the host reads it: the largest level of the blocks since the last read, the deepest gain reduction,
  and any clip. `get_values()` hands that over and starts again. It may be called from any thread but the audio role,
  and the plugin has one reader. A burst of 10 ms between two reads is in the next read. The audio thread does the
  keeping with one compare per block and nothing else; the host adds no work to it.
- **The reader recovers.** The host reads at the rate omx-dsp declares for it, `hostReadHz` in
  `data/primitives/meter.json`, taken from the installed omx-dsp definitions when the host is built (`src/meter_reader.h`,
  `src/main.c` has no number of its own for it): 50 Hz, an idle interval of 20 ms. A gain reduction is stepped by
  `omx_meter_gr_step` over the time elapsed since the last read, on the deepest value since then: a deeper one is taken
  at once, a shallower one waits out the hold and then lands. A level is sent as it was read.
- **A plugin that holds nothing** gives what it publishes: the last block it processed. Its meter is `unheld`
  (`effects_meter_held` answers 0): a burst between two reads is not in what it gives, and the host does not pretend
  otherwise. A foreign plugin's gain reduction is stepped the same way, on the one value it gives.
- **The clip word.** A level meter with `OMX_CLAP_METER_HAS_CLIP` (bit 9) writes one value after its channels: 1 when a
  clip ran since the last read (the plugin holds it like the peak), 0 otherwise. The host sends it as the output
  `<meter name> clip` (`level_clip` for a meter called `level`) and latches it as the strip's clip is: lit by a clip,
  lit until `effects_meter_clip_clear`, which the next read follows with a 0. It is a digital clip, not a measure of
  anything analogue.

### Monitors stop

`monitor_output_off <instance> <symbol>` ends a monitor: the output is not read again and nothing is sent for it. An
output not monitored stays so, and a symbol the instance does not have is refused as `monitor_output` refuses it. A
monitor also ends with the connection that set it: the host keeps the connection of every command (the socket's
cookie, so a later connection that takes the same descriptor number is another one), and an idle tick that finds a
monitor's connection gone stops it before reading. With nothing monitored the plugin is read zero times
(`effects_meter_reads` counts the values read, and stays where it was). A controller that reconnects monitors again;
plugin-hostd replays its ledger of monitors on a new connection.

## In process

A program that links libomx-clap-core and runs a plugin in its own process reads the same extension through the
library, never through the plugin itself:

- `omx_clap_host_meter_count(in)` and `omx_clap_host_meter_info(in, index, &meter)` answer the meters the plugin
  declared, read once when the instance is opened (`count()` and `get_info()` are main-thread calls). The answer
  carries everything `clap_meter_info_t` does: the id, the name, the kind, the flags, the channel count and the
  display range. The declarations are fixed for the instance's life, so any thread may read them.
- `omx_clap_host_meter_values(in, id, values, capacity)` calls the plugin's `get_values()`, which the extension
  declares thread-safe, lock-free and wait-free: a telemetry thread reads it at its own rate, never the audio thread.
  It answers how many channels it wrote, at most `capacity` and the meter's channel count, and 0 for an id the
  instance did not declare, or a plugin that has not processed since it was activated.

The order is the one above, without the second step: in process only `clap.meters/1` is read. A plugin that serves
none, or whose declarations cannot be read whole (a meter with no info, no name, no channel or more than
`OMX_CLAP_METER_CHANNELS_MAX`, or more meters than `OMX_CLAP_METER_COUNT_MAX`), has no meters there, and it is still
opened: a meter is something the console draws, never a reason to refuse the plugin. The library reads no meter on its
own and passes nothing to the audio thread; the caller decides what to read and when.

## While the draft is not in the SDK

No released CLAP SDK ships `clap/ext/draft/meters.h` yet. Until one does, `src/clap_meters.h` declares the draft's
id and structures exactly as the draft does, and includes the SDK's header instead as soon as the SDK has it, so
the host builds against either. If the extension's id or layout changes before it is merged, the host follows the
SDK's header, and `src/clap_meters.h` is removed once the oldest SDK the host builds against ships it.

## Tests

`tests/effects_meter_test.c` includes the host's `effects.c` and runs `outputs_notify` against the real core and omx-dsp:
two plugins process the same blocks (one holds, one is foreign), and a 10 ms burst between two reads reaches `output_set`
for the first and is missed, and named unheld, for the second; the reduction is held 300 ms and lands, at 20 ms ticks and at
4 ms ticks alike; the clip word stays lit until cleared; a stopped monitor and one whose connection ended are read zero
times, also when the new connection has the old descriptor number. `make test-effects-meter-sabotage` breaks each of
those guards in a copy and wants the test red. `tests/jack_meters_e2e.sh` repeats the connection end over the real socket.


`tests/fake_compressor.c` serves `clap.meters/1`, and one of its plugins serves only `org.openmixer.meters/1`.
`tests/jack_meters_e2e.sh` holds the host to the order above over jack: every meter of `clap.meters/1` answers
`monitor_output` and arrives as `output_set`, the standard gain adjustment is not an output beside it, and the
plugin that offers only the vendor extension has no output at all. `tests/clap_core_test.c` holds the in-process read
to the same declarations and values, and to a plugin whose declarations cannot be read whole having none.
