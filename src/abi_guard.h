/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/*
 * A call into a plugin that cannot break the host's frame. The calling convention gives a callee registers it must
 * hand back as it found them (aarch64: x19..x28, d8..d15, FPCR; x86-64: rbx, rbp, r12..r15, MXCSR's control bits and
 * the x87 control word). A plugin that returns with one changed corrupts whatever its host kept there across the call:
 * on a Raspberry Pi 5 the LSP limiters' run() returned with d15 changed, and the measurer that kept its sample rate in
 * d15 opened its next instance at 0 Hz.
 *
 * omx_abi_call4 (src/abi_guard.S) saves those registers, calls, compares, puts them back and says which ones changed.
 * The host's frame is safe after it; another plugin's state in the same process is not, so a plugin that changes one
 * is run only in a worker of its own.
 */
#ifndef OMX_ABI_GUARD_H
#define OMX_ABI_GUARD_H

#include <stdint.h>

/* the LV2 adapter's archive carries its own copy under its own name, so a host links both (src/abi_guard.S) */
#if defined(OMX_ABI_LV2)
#define omx_abi_call4 omx_lv2_abi_call4
#define omx_abi_call4d omx_lv2_abi_call4d
#define omx_abi_call4f omx_lv2_abi_call4f
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* the bits of omx_abi_report's masks */
#if defined(__aarch64__)
#define OMX_ABI_X19             (1u << 0)       /* x19..x28: bits 0..9 */
#define OMX_ABI_D8              (1u << 10)      /* d8..d15: bits 10..17 */
#define OMX_ABI_FPCR            (1u << 18)
#define OMX_ABI_FP              (1u << 19)      /* x29, the frame pointer */
#define OMX_ABI_REGS            20
#elif defined(__x86_64__)
#define OMX_ABI_RBX             (1u << 0)
#define OMX_ABI_RBP             (1u << 1)
#define OMX_ABI_R12             (1u << 2)       /* r12..r15: bits 2..5 */
#define OMX_ABI_MXCSR           (1u << 6)       /* its control bits: DAZ, the exception masks, rounding, FZ */
#define OMX_ABI_X87CW           (1u << 7)
#define OMX_ABI_REGS            8
#endif

/* What the calls made through one report changed. */
struct omx_abi_report
{
    uint32_t ever;          /* the union of every call's mask */
    uint32_t last;          /* the last call's mask: 0 when it changed nothing */
    uint64_t calls;         /* the calls that changed any */
};

/* fn(a0, a1, a2, a3), its return value, with the callee-saved registers kept for the caller; r may be NULL. The
 * floating-point argument registers reach fn as the caller left them (omx_abi_call4d). */
uint64_t omx_abi_call4(struct omx_abi_report *r, const void *fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);

/* the same code with fn's one floating-point argument d, which reaches fn in its first floating-point register (an LV2
 * instantiate's sample rate), and with fn's double result */
uint64_t omx_abi_call4d(struct omx_abi_report *r, const void *fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                        double d);
double omx_abi_call4f(struct omx_abi_report *r, const void *fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                      double d);

/* the register's name for bit `bit` of a mask, NULL past the last */
static inline const char *omx_abi_reg_name(unsigned bit)
{
#if defined(__aarch64__)
    static const char *const names[OMX_ABI_REGS] = {
        "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28",
        "d8", "d9", "d10", "d11", "d12", "d13", "d14", "d15", "fpcr", "x29",
    };
#elif defined(__x86_64__)
    static const char *const names[OMX_ABI_REGS] = { "rbx", "rbp", "r12", "r13", "r14", "r15", "mxcsr", "x87cw" };
#endif
    return bit < OMX_ABI_REGS ? names[bit] : 0;
}

#ifdef __cplusplus
}
#endif

#endif
