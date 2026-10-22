#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
"""Stands in for jalv in lv2_parity.py's --self-test (never the real plugin host). Three fixtures, by a URI
lv2_parity.py's own tests use (no real plugin is named this), each print `sha <hex>` on stdout; a URI with no
fixture (the self-test's fourth case, `urn:third-host:fault`) exits 1, the fault run_jalv must report."""
import sys

FIXTURE_SHAS = {
    'urn:third-host:agree-with-mod-host': 'aaaaaaaaaaaaaaaa',
    'urn:third-host:agree-with-adapter': 'bbbbbbbbbbbbbbbb',
    'urn:third-host:neither': 'cccccccccccccccc',
}


def main(argv):
    if argv == ['--version']:
        print('jalv 1.6.9 (self-test stub)')
        return 0
    uri = argv[0] if argv else ''
    sha = FIXTURE_SHAS.get(uri)
    if sha is None:
        print(f'jalv: fatal: the self-test stub has no fixture for {uri!r}', file=sys.stderr)
        return 1
    print(f'sha {sha}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
