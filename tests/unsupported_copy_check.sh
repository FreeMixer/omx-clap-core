#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# unsupported_copy_check.sh <mod-host.h> [src dir] | --self-test <mod-host.h>
#
# The mod-host verbs this host refuses are the protocol library's list (host_dispatch_register_unsupported): a
# registration in src/ of one of mod-host's command macros, as the protocol library's mod-host.h defines them, with
# host_dispatch_unsupported_cb is a copy of that list and fails. The macros are read from the header, never listed here.
# Exit 0 none; 1 a copy; 2 usage.
set -euo pipefail

check() { # <mod-host.h> <src dir>
  local header=$1 src=$2
  [ -f "$header" ] || { echo "unsupported_copy_check: no $header" >&2; return 2; }
  python3 -I - "$header" "$src" <<'PY'
import os, re, sys
header, src = sys.argv[1:]
macros = set(re.findall(r'^#define ([A-Z_][A-Z0-9_]*)\s+"[a-z_]+', open(header).read(), re.M))
bad = 0
for root, _, files in os.walk(src):
    for f in files:
        if not f.endswith('.c'):
            continue
        path = os.path.join(root, f)
        text = re.sub(r'/\*.*?\*/|//[^\n]*', '', open(path).read(), flags=re.S)
        for arg in re.findall(r'protocol_add_command\(\s*([^,]+?)\s*,\s*host_dispatch_unsupported_cb\s*\)', text):
            # a registration of one macro, or of each element of an array the loop walks (*command over commands[])
            names = [arg] if re.fullmatch(r'[A-Z_][A-Z0-9_]*', arg) else re.findall(r'[A-Z_][A-Z0-9_]*', ' '.join(re.findall(r'\[\]\s*=\s*\{(.*?)\}', text, re.S)))
            for n in names:
                if n in macros:
                    print(f'unsupported_copy_check: {path}: {n} is registered as refused here; the protocol library\'s list refuses it', file=sys.stderr)
                    bad = 1
if bad:
    sys.exit(1)
print("unsupported_copy_check: no mod-host verb is refused by a list of this host's own")
PY
}

if [ "${1:-}" = --self-test ]; then
  header=${2:?usage: unsupported_copy_check.sh --self-test <mod-host.h>}
  t=$(mktemp -d); trap 'rm -rf "$t"' EXIT; fail=0
  name=$(sed -n 's/^#define \([A-Z_][A-Z0-9_]*\)[[:space:]]\+"[a-z_]\+.*/\1/p' "$header" | head -1)
  [ -n "$name" ] || { echo "FAIL the header names no command macro"; exit 1; }
  mkdir -p "$t/clean" "$t/copy"
  printf 'static void f(void) { protocol_add_command(PRESET_SHOW_INSTANCE, host_dispatch_unsupported_cb); }\n' > "$t/clean/main.c"
  printf 'static void f(void) { protocol_add_command(%s, host_dispatch_unsupported_cb); }\n' "$name" > "$t/copy/main.c"
  mkdir -p "$t/array"
  printf 'static void f(void) { static const char *const commands[] = { %s, NULL }; const char *const *command;\n  for (command = commands; *command; command++) protocol_add_command(*command, host_dispatch_unsupported_cb); }\n' "$name" > "$t/array/main.c"
  if check "$header" "$t/clean" >/dev/null 2>&1; then echo "ok   a host's own form of a verb is green"; else echo "FAIL a host's own form of a verb is red"; fail=1; fi
  if check "$header" "$t/copy" >/dev/null 2>&1; then echo "FAIL a copy of the library's list ($name) is green"; fail=1; else echo "ok   a copy of the library's list ($name) is red"; fi
  if check "$header" "$t/array" >/dev/null 2>&1; then echo "FAIL a copy in an array the loop walks is green"; fail=1; else echo "ok   a copy in an array the loop walks is red"; fi
  [ "$fail" = 0 ] && echo "unsupported_copy_check self-test: every case as wanted"
  exit "$fail"
fi
check "${1:?usage: unsupported_copy_check.sh <mod-host.h> [src dir]}" "${2:-src}"
