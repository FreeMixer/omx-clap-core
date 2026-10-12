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
   unchanged). At the idle rate the host calls `get_values()` once per monitored meter and sends the channels it
   wrote; a call that writes none (the plugin has not processed since it was activated) sends nothing.
2. Only when the plugin has no `clap.meters/1`: `clap.gain-adjustment-metering/0`, as before.

`get_values()` returns doubles; `output_set` carries the value as a float, as it carries every output. The display
range and the flags are the plugin's to declare and the console's to draw: mod-host's `output_set` has no field
for them, so the host does not forward them.

## While the draft is not in the SDK

No released CLAP SDK ships `clap/ext/draft/meters.h` yet. Until one does, `src/clap_meters.h` declares the draft's
id and structures exactly as the draft does, and includes the SDK's header instead as soon as the SDK has it, so
the host builds against either. If the extension's id or layout changes before it is merged, the host follows the
SDK's header, and `src/clap_meters.h` is removed once the oldest SDK the host builds against ships it.

## Tests

`tests/fake_compressor.c` serves `clap.meters/1`, and one of its plugins serves only `org.openmixer.meters/1`.
`tests/jack_meters_e2e.sh` holds the host to the order above over jack: every meter of `clap.meters/1` answers
`monitor_output` and arrives as `output_set`, the standard gain adjustment is not an output beside it, and the
plugin that offers only the vendor extension has no output at all.
