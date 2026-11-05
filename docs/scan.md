The scanner's JSON
==================

`omx-clap-scan --json [<path>...]` is the program interface to what is in a CLAP file: a consumer that needs a plugin's
id, its parameters, its ports or its presets reads this document and does not load the plugin itself, so a plugin that
crashes takes down the scanner's child process for that one file and nothing else. The text listing (no `--json`) is the
same data for a person; this page is about the JSON.

- Run it as `omx-clap-scan --json file.clap` or `omx-clap-scan --json /usr/lib64/clap`; with no path it reads
  `CLAP_PATH`, then `~/.clap`, `/usr/lib64/clap` and `/usr/lib/clap`.
- Stdout is one JSON document. A diagnostic goes to stderr, never into the document.
- The exit status is 0 when at least one path could be read, 1 when none could, 2 for a bad command line. A file that
  gave no listing is not an exit status: it is an entry in the document with a `code` (below).
- `--json` takes paths only: `--fields` has no JSON form.

The schema
----------

The document is described by `omx-clap-scan.schema.json` (JSON Schema, 2020-12), installed by the omx-clap-host package
as `/usr/share/omx-clap-host/omx-clap-scan.schema.json` and kept in this tree as `schema/omx-clap-scan.schema.json`.
The schema declares every field the scanner writes and no other: a field added to the scanner without the schema is a
failure of `make test-scan-json`, and a consumer may validate with it. The version of the document is the scanner's:
`version` holds the omx-clap-host version, and a field is added in a release and never changes meaning inside it.

The document
------------

    {"scanner":"omx-clap-scan","version":"0.28.1","files":[ <file>, ... ]}

A `file` is one of two shapes. Files are in the order of the paths given, a directory's in the order it was walked, a
file reached twice listed once, and the files that gave no listing last.

- listed: `{"path", "plugins":[<plugin>...], "presets":[<preset>...]}`. `presets` is absent when the file has no
  preset-discovery factory.
- failed: `{"path", "code", "error"}`. `code` is one of `timeout` (the scan of the file took too long), `crash:<signal>`
  (the child died of that signal number), `dlopen` (the loader refused the file), `entry` (the file loaded and its
  clap_entry refused), `unreadable` (a path that cannot be stat'ed), `pipe`, `fork` and `no-result`. `error` is the reason in words.

A `plugin` is its descriptor, `id`, `name`, `vendor`, `version`, `description`, `url` and `features`, and either an
`error` (the plugin failed to create or initialise: nothing else is listed) or what the core read from it:

| field | what it holds |
| --- | --- |
| `params` | each `{id, name, module, min, max, default, flags}`; a bound the plugin leaves open is `null`; `flags` are the CLAP parameter flags by name; an enum parameter has `labels`, `[{value, label}]`, the plugin's own text at every whole value of its range (none when the range is open or wider than 1024) |
| `remote_controls` | the plugin's remote-control pages, each `{id, section, name, params:[8 ids, null for an empty slot], forPreset}` |
| `audio_ports` | `{inputs, outputs}`, each port `{id, name, role: main or aux, channels, kind, jack}`; `kind` is mono, stereo, surround, ambisonic or unknown; a surround port has `map`, the channel ids; an ambisonic one `ordering` and `normalization`, and `assumed` when the plugin did not say; `jack` holds the port names omx-clap-host registers for it, and is absent for a plugin the host refuses. `passthrough: "host"` is present for a tool with no audio port, which the host places as a pass-through |
| `note_ports` | `{inputs, outputs}`, counts |
| `note_names` | what a drum machine calls its keys, `[{name, port, key, channel}]`, -1 for every one; absent when the plugin names none |
| `latency` | the frames the plugin reports; absent when it reports none or 0 |
| `voice_info` | `{count, capacity, overlapping_notes}` from `clap.voice-info`; absent when the plugin offers none |
| `resource_hints` | what the plugin declares (`org.openmixer.resource-hints.draft/0`): `declared` and one `{unit, figure}` for each hint by omx-contract's word, `figure` `null` when unknown and never 0, or `refused` and the reason; absent when the plugin states none |
| `panel` | `null` for a plugin that serves no panel description, else `{family, fingerprint, sections, controls, roles, displays, placementDefaults}`, each table its entries or `{refused:"too-many", count, max}` |

A `preset` is `{name, location_kind: plugin or file, location, load_key, provider, plugins, flags}`.

Strings are UTF-8; a byte that is not UTF-8 is written `?`, and a control character as `\u00xx`.

The same data as the text
-------------------------

Every line of the text listing has its field in the JSON, with the same value, and the check holds it: `make
test-scan-json` runs the scanner as text and as JSON over a set of fixture plugins (`tests/fake.clap`, `fake_synth.clap`,
`fake_ext.clap`, `fake_panel.clap`, `fake_tool.clap` and a plugin that crashes), parses the text back to data, and fails when
a parameter, a port, a note name, a latency, a voice figure, a resource hint, a panel count, a preset, a failure or a
plugin differs, or when the JSON does not validate. `--self-test` breaks each of seventeen things in the JSON after the scan and
requires the check to name each. What only the JSON carries (`description`, `url`, `remote_controls`, enum `labels`, the
panel's tables, a port's `id`) is held by the schema alone.
