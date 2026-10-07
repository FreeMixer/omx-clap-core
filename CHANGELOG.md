# Changelog

What changed in each release of omx-clap-host, in plain words. The RPM and Debian changelogs and the
GitHub release notes are generated from this file.

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
