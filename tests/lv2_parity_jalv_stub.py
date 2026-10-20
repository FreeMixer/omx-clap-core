#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
"""Stub for tests/lv2_parity.py's --self-test only: stands in for run_jalv()'s contract (a URI, a rate, a frame
count and an output path at argv[1:5], writing raw float32 samples there or exiting non-zero) under
LV2_PARITY_JALV_STUB_MODE in agree-mod, agree-adapter, neither, fault. Reads the reference recordings from
LV2_PARITY_JALV_STUB_MOD_REF / _OMX_REF (raw float32) so the fixture's agreement is byte-for-byte with whatever
the self-test itself used as mod-host's and omx-clap-host's recordings. Never driven for real: real jalv has no
such protocol; a real driving wrapper is a later step."""
import os
import sys

import numpy as np

_, uri, rate, frames, outfile = sys.argv[:5]
mode = os.environ['LV2_PARITY_JALV_STUB_MODE']
if mode == 'fault':
    sys.exit('stub jalv: fault by request')
mod_ref = np.fromfile(os.environ['LV2_PARITY_JALV_STUB_MOD_REF'], dtype=np.float32)
omx_ref = np.fromfile(os.environ['LV2_PARITY_JALV_STUB_OMX_REF'], dtype=np.float32)
if mode == 'agree-mod':
    out = mod_ref
elif mode == 'agree-adapter':
    out = omx_ref
elif mode == 'neither':
    out = mod_ref + np.float32(1.0)
else:
    sys.exit(f'stub jalv: unknown LV2_PARITY_JALV_STUB_MODE {mode!r}')
out.tofile(outfile)
