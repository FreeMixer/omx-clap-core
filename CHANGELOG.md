# Changelog

What changed in each release of omx-clap-host, in plain words. The RPM and Debian changelogs and the
GitHub release notes are generated from this file.

## Unreleased

- New development package, omx-clap-lv2-devel (libomx-clap-lv2-dev on Debian): a library that
  lets a CLAP host run LV2 plugins. An LV2 plugin with one or two audio channels in and out is
  presented as a CLAP plugin, with its controls as parameters, its reported latency, its default
  settings and its background worker.
- A plugin the library cannot run yet (MIDI or other event ports, CV ports, other channel layouts,
  a feature it does not provide) is refused when it is created, with a reason the host can log.

## 0.3.1 - 2026-10-08

- A plugin may have at most 2048 parameters. One that reports more is served its first 2048, and
  the host's log says so once, when the plugin is opened; the library no longer sizes its tables or
  its scans from whatever count the plugin gives. Its layout cannot be pinned.
- A plugin whose parameter table cannot be allocated is refused when it is opened, instead of being
  opened with the table missing.
- A parameter name the plugin did not terminate is read no further than its own buffer.

## 0.3.0 - 2026-10-07

- A plugin whose parameter list has a hole is refused whole, never served short; the whole list
  comes in one call.
- A host can give an instance its tempo: the plugin reads it from the transport of every block.
- The list of host extensions has a header of its own, `clap_host_extensions.h`.
- The qualifier's fault plugins come with the tests (`make fixtures`).
- A plugin re-engaged after a bypass is reset on the control thread, never in the audio path.
- The worker message ring an in-process plugin shares with its worker thread ships as
  `omx_msgring.h`.
- The library's ABI number is 2: a program built against the 0.2 headers keeps working, and one
  built against these is refused by an older library.

## 0.2.0 - 2026-10-03

- The library names the threads that carry audio with a test the host supplies, so every worker
  of a split processing chain counts as an audio thread.

## 0.1.1 - 2026-10-01

- Plugin meters are reported the way mod-host reports its output meters.
- Before a plugin is activated, the host checks the layout that plugin-hostd expects.
- Answers the new plugin information commands: track_info, remote_pages, remote_page_get and
  param_info.
- Built against plugin-hostd 0.1.2.

## 0.1.0 - 2026-09-29

- First package: the CLAP host, and the library it is built on with its development files.
