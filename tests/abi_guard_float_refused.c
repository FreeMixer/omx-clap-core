/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/* Must NOT compile: a double passed through OMX_ABI_A would be truncated into an integer register (make test-abi-guard). */
#include "../src/abi_guard.h"

uint64_t refused(void);
uint64_t refused(void) { return OMX_ABI_A(0.5); }
