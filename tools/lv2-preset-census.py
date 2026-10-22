#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
"""
lv2-preset-census: the presets each installed LV2 plugin offers, read with lilv.

    lv2-preset-census.py [--user-dir DIR]

Every plugin lilv's world finds (LV2_PATH, else lilv's default path) gives one TSV line on stdout:

    uri  name  bundle  shipped_presets  shipped_banks  shipped_with_port_values  shipped_with_state
    user_presets  user_with_state  error

A preset is a pset:Preset related to the plugin (lv2:appliesTo, what lilv's get_related reads, and the LV2 adapter's
lv2_bundle_presets lists). It is SHIPPED when the file that describes it (its rdfs:seeAlso, else the plugin's own
bundle) lies outside the user directory (default ~/.lv2), and USER when it lies inside: a preset another host saved.
shipped_banks counts the distinct pset:bank values. with_port_values counts presets that set at least one lv2:port
value, with_state the ones that carry a state:state body. Nothing is instantiated.
"""

import os
import sys
import urllib.parse

import lilv

PSET = "http://lv2plug.in/ns/ext/presets#"
STATE = "http://lv2plug.in/ns/ext/state#"
LV2 = "http://lv2plug.in/ns/lv2core#"
RDFS = "http://www.w3.org/2000/01/rdf-schema#"


def path_of(node):
    """A file: URI node as a path, or None."""
    if node is None:
        return None
    text = str(node)
    if not text.startswith("file://"):
        return None
    return urllib.parse.unquote(urllib.parse.urlparse(text).path)


def main(argv):
    user_dir = os.path.expanduser("~/.lv2")
    if len(argv) == 3 and argv[1] == "--user-dir":
        user_dir = argv[2]
    elif len(argv) != 1:
        sys.stderr.write("usage: lv2-preset-census.py [--user-dir DIR]\n")
        return 2
    user_dir = os.path.realpath(user_dir) + os.sep

    world = lilv.World()
    world.load_all()
    n_preset = world.new_uri(PSET + "Preset")
    n_bank = world.new_uri(PSET + "bank")
    n_state = world.new_uri(STATE + "state")
    n_port = world.new_uri(LV2 + "port")
    n_see_also = world.new_uri(RDFS + "seeAlso")
    n_type = world.new_uri("http://www.w3.org/1999/02/22-rdf-syntax-ns#type")

    print("uri\tname\tbundle\tshipped_presets\tshipped_banks\tshipped_with_port_values\tshipped_with_state\t"
          "user_presets\tuser_with_state\terror")
    rows = []
    loaded = set()
    for plugin in world.get_all_plugins():
        uri = str(plugin.get_uri())
        error = ""
        try:
            node = plugin.get_name()
            name = str(node) if node is not None else ""
        except Exception as e:  # a plugin with no name is still counted
            name, error = "", f"name: {e}"
        bundle = path_of(plugin.get_bundle_uri()) or ""
        shipped = banks = shipped_values = shipped_state = user = user_state = 0
        bank_set = set()
        try:
            presets = plugin.get_related(n_preset)
            for preset in presets:
                if not world.ask(preset, n_type, n_preset):
                    continue
                files = [path_of(n) for n in world.find_nodes(preset, n_see_also, None)]
                files = [f for f in files if f]
                where = files[0] if files else bundle
                is_user = os.path.realpath(where).startswith(user_dir)
                # a preset's statements live in its seeAlso files; each file is parsed once and kept, since a bundle
                # often describes hundreds of presets in one file
                key = tuple(sorted(files))
                if key not in loaded:
                    world.load_resource(preset)
                    loaded.add(key)
                has_state = world.get(preset, n_state, None) is not None
                has_values = world.get(preset, n_port, None) is not None
                bank = world.get(preset, n_bank, None)
                if is_user:
                    user += 1
                    user_state += has_state
                else:
                    shipped += 1
                    shipped_values += has_values
                    shipped_state += has_state
                    if bank is not None:
                        bank_set.add(str(bank))
            banks = len(bank_set)
        except Exception as e:
            error = (error + "; " if error else "") + f"presets: {e}"
        rows.append((uri, name.replace("\t", " "), bundle, shipped, banks, shipped_values, shipped_state, user, user_state,
                     error.replace("\t", " ").replace("\n", " ")))
    for row in sorted(rows):
        print("\t".join(str(v) for v in row))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
