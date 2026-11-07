<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> -->
# Unreleased

Lines for the next release; the release moves them into debian/changelog and the spec.

- The host reads a plugin's meters at the rate omx-dsp declares for it (50 Hz, `hostReadHz`), taken from the installed
  omx-dsp definitions at build time; `IDLE_INTERVAL_MS` is no number of its own.
- A meter flagged `OMX_CLAP_METER_IS_HELD` is one the plugin holds until it is read; a burst of 10 ms between two reads
  reaches `output_set`. A meter that holds nothing is named unheld.
- A gain reduction is stepped by `omx_meter_gr_step` over the time elapsed between reads, on the deepest value since the last one.
- A level meter flagged `OMX_CLAP_METER_HAS_CLIP` carries a clip word after its channels; the host sends it as `<name> clip`,
  keeps it lit until it is cleared, and never reads it for a stopped monitor.
- `effects_monitor_output_off` stops reading an output. A monitor ends with the connection that set it. With nothing
  monitored the plugin is read zero times.
- docs/meters.md: the plugin holds, the reader recovers.
