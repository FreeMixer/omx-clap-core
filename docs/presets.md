Presets in the worker
=====================

What omx-clap-host does with presets, for CLAP plugins and for LV2 plugins through the adapter: list a plugin's own
presets, load one, save the instance into a preset file and load that file again. The ruling is plugin-hostd#131's
comment "PRESETS IN THE HOST" (2026-10-22); this page is the worker's side of it (omx-clap-core#219). The host never
decides a path or a name: both arrive as arguments. Where files live, user banks, rename and delete are the consumer's.

Verbs
-----

    preset_list <N>                    resp <count>
    preset_get <N> <index>             resp 0 <load_key> <name>
    preset_load <N> <load_key | file>  resp 0
    preset_save <N> <file> <name>      resp 0
    preset_show <N> <load_key | file>  resp 0 <json>

- `preset_list` counts the plugin's own presets: a CLAP plugin's through its file's `clap.preset-discovery-factory`
  (every provider, every location, as `omx_clap_host_presets` reads them), an LV2 plugin's through the adapter's entry,
  which lists the bundle's `pset:Preset` resources by lilv. Only presets that name the instance's plugin count. A
  plugin that offers none answers `resp 0`.
- `preset_get` answers one of them, 0 to count - 1, in the listing's order: the key `preset_load` and `preset_show`
  take, and its name (the rest of the line). The key is the preset's URI for LV2, `file:<path>` for a CLAP `FILE`
  location holding one preset, `file:<path>#<key>` for one inside a container, `plugin:<key>` for a `PLUGIN` location;
  a space and a `%` in it are written `%20` and `%25`. No key starts with `/`: an absolute path is a preset file of
  ours, which plugin-hostd reads before it reaches the worker. -902 for an index that is not one. The rows are those
  of the last `preset_list`; `preset_list` reads the plugin's presets again.
- `preset_load` takes a key of `preset_get` (loaded through `clap.preset-load`), an LV2 preset's URI, or the path of a
  preset file below. A file's parameters are written as their exact values (a double, never through a float) and its
  state body is loaded with `clap.state-context` (`preset` context) or `clap.state`.
- `preset_save` writes the instance into the file at `<file>` with the display name `<name>` (the rest of the line, or
  one quoted word). An existing preset file at that path keeps its `id`, `createdAt` and every field this host does not
  write; anything else at that path is refused.
- `preset_show` answers what a load would apply, on one line of JSON: for a file, the document with each state body's
  `bytes` replaced by its decoded `size`; for a listed preset, its `name`, `loadKey` and `location`.

After a load the host reports every parameter whose value the load moved as mod-host's `param_set <N> <symbol> <value>`
feedback line, the same line a value the plugin moves by itself gives (plugin-hostd relays it as the parameter-feedback
event); the plugin's own output events are flushed first, through `clap.params` flush.

The file
--------

The console's `org.openmixer.preset` format, extended additively. `org.openmixer.preset/1` is params-only and stays
valid; a file with a state body is `org.openmixer.preset/2`. Any other version is refused, never guessed.

    {
      "format": "org.openmixer.preset/2",
      "plugin": { "id": "<plugin id>", "version": "<descriptor version>", "fingerprint": "omx-layout/1:<sha256>" },
      "id": "<stable id>", "name": "<display name>",
      "createdAt": "2026-10-22T21:00:00Z", "modifiedAt": "2026-10-22T21:00:00Z",
      "carries": "params+state",
      "params": [ { "symbol": "<symbol>", "unit": "", "value": 0.25 } ],
      "state": { "encoding": "base64", "context": "preset", "bytes": "<base64>" }
    }

- `plugin.id` is the CLAP id, or the LV2 plugin's URI. `plugin.version` is the descriptor's version (for LV2 the
  adapter's `<lv2:minorVersion>.<lv2:microVersion>`). `plugin.fingerprint` is the plugin's parameter layout digest,
  the `omx-layout/1` digest of plugin-hostd's pin (`plugin-hostd/pin.h`): every parameter `clap.params` gives, its id,
  name, range, default and flags. A file whose id, version or fingerprint differs from the instance's is refused with
  `PRESET_PLUGIN_CHANGED` and nothing of it is applied.
- `id` is minted by the consumer; a save that finds none writes one (the plugin id and the save's time).
- `carries` is `params` when the plugin has only declared parameters (no `clap.state`), otherwise `params+state`: the
  values and the state body are both read. A `/1` file, or a `/2` that carries `params`, has no `state`.
- `params`: every parameter row, `symbol` the name `param_set` takes (an LV2 control port's symbol, else the CLAP id
  in decimal), `value` the plain value printed with 17 significant digits, so it reads back to the same double.
- `state.context` is `preset` for CLAP, `lv2` for an LV2 plugin through the adapter (the body is then its state as
  Turtle). `state.files` is reserved for LV2 state paths.
- Optional fields `alias` (12 characters at most), `bank`, `tags` and `description` are written by the consumer and
  kept on rewrite; so is any field this host does not know.

Refusals
--------

A refused preset verb answers `resp -104 <code>`, the code one of plugin-hostd's preset refusals:

| Code | When |
|---|---|
| `PRESET_NOT_OFFERED` | the key names no preset of the plugin, or the plugin offers no `clap.preset-load` for a key of `preset_get` |
| `PRESET_INVALID` | the file cannot be read or written, is not JSON, lacks a field, or has a state body that is not base64 |
| `PRESET_NEEDS_STATE` | the file carries state and the plugin has no `clap.state` to take it |
| `PRESET_PLUGIN_CHANGED` | the file's plugin id, version or fingerprint is not the instance's; an unknown format version also answers it with `PRESET_INVALID` |
| `LOAD_REFUSED` | the plugin refused the load (or the save) of its state or preset |

An unknown instance answers mod-host's `-1`.

Tests
-----

`make test-jack-presets` (`tests/jack_presets_e2e.sh`): through the host over its socket in a private PipeWire, a
CLAP plugin with state and parameters (`tests/fake_mover.clap`) and real plugins of the worker image when present:
save then load into a fresh instance reproduces every parameter value exactly, compared as values; a changed version
and a changed fingerprint refuse; a params-only plugin writes `carries` `params`; a plugin that ships factory presets
lists a nonzero count, and a listed one loads. Each check has a sabotage arm.
