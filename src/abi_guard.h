/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/*
 * A call into a plugin that cannot break the host's frame. The calling convention gives a callee registers it must
 * hand back as it found them (aarch64: x19..x28, d8..d15, FPCR; x86-64: rbx, rbp, r12..r15, MXCSR's control bits and
 * the x87 control word). A plugin that returns with one changed corrupts whatever its host kept there across the call:
 * on a Raspberry Pi 5 the LSP limiters' run() returned with d15 changed, and the measurer that kept its sample rate in
 * d15 opened its next instance at 0 Hz.
 *
 * omx_abi_call6 (src/abi_guard.S) saves those registers, calls, compares, puts them back and says which ones changed.
 * The host's frame is safe after it; another plugin's state in the same process is not, so a plugin that changes one
 * is run only in a worker of its own.
 */
#ifndef OMX_ABI_GUARD_H
#define OMX_ABI_GUARD_H

#include <stdatomic.h>
#include <stdint.h>

/* the LV2 adapter's archive carries its own copy under its own name, so a host links both (src/abi_guard.S) */
#if defined(OMX_ABI_LV2)
#define omx_abi_call6 omx_lv2_abi_call6
#define omx_abi_call6d omx_lv2_abi_call6d
#define omx_abi_call6f omx_lv2_abi_call6f
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

/* fn(a0, ..., a5), its return value, with the callee-saved registers kept for the caller; r may be NULL. The
 * floating-point argument registers reach fn as the caller left them (omx_abi_call6d). */
uint64_t omx_abi_call6(struct omx_abi_report *r, const void *fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5);

/* the same code with fn's one floating-point argument d, which reaches fn in its first floating-point register (an LV2
 * instantiate's sample rate), and with fn's double result */
uint64_t omx_abi_call6d(struct omx_abi_report *r, const void *fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, double d);
double omx_abi_call6f(struct omx_abi_report *r, const void *fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5, double d);

/* One plugin's guard: its report, the first call that changed a register, and the block count that names where. The
 * audio thread writes it with no lock and no log: the first clobber's call and block, then its mask with a release
 * store, once; a control thread reads the mask with an acquire load and then the rest (omx_abi_guard_first). */
struct omx_abi_guard
{
    struct omx_abi_report report;   /* every call's, through this guard */
    uint64_t block;                 /* the process or run calls made so far: the caller counts them */
    _Atomic uint32_t first_mask;    /* the first clobber's registers, 0 until one */
    uint64_t first_block;           /* the block it happened in */
    const char *first_call;         /* the plugin function it happened in: "process", "run", "activate", ... */
};

/* What a reader is given of the first clobber. */
struct omx_abi_clobber
{
    uint32_t mask;                  /* the registers, OMX_ABI_* bits; 0: none so far */
    uint64_t block;                 /* the block, counted from the plugin's first */
    const char *call;               /* the plugin function, a static string */
};

/* the guard's note of a call just made: the first clobber is kept, later ones only counted in the report */
static inline void omx_abi_guard_note(struct omx_abi_guard *g, const char *call)
{
    if (g->report.last && !atomic_load_explicit(&g->first_mask, memory_order_relaxed))
    {
        g->first_call = call;
        g->first_block = g->block;
        atomic_store_explicit(&g->first_mask, g->report.last, memory_order_release);
    }
}

/* the first clobber, for any thread: 1 and *out filled when there was one, 0 otherwise */
static inline int omx_abi_guard_first(const struct omx_abi_guard *g, struct omx_abi_clobber *out)
{
    const uint32_t mask = atomic_load_explicit(&((struct omx_abi_guard *)g)->first_mask, memory_order_acquire);

    if (!mask)
        return 0;
    out->mask = mask;
    out->block = g->first_block;
    out->call = g->first_call;
    return 1;
}

#define OMX_ABI_A(x) ((uint64_t)(uintptr_t)(x))

/* a plugin call through guard g (NULL: guarded, not recorded), named `call` for the record */
static inline uint64_t omx_abi_g(struct omx_abi_guard *g, const char *call, const void *fn, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    const uint64_t v = omx_abi_call6(g ? &g->report : NULL, fn, a0, a1, a2, a3, a4, a5);

    if (g)
        omx_abi_guard_note(g, call);
    return v;
}

/* the same with fn's one double argument (a sample rate) */
static inline uint64_t omx_abi_gd(struct omx_abi_guard *g, const char *call, const void *fn, uint64_t a0, uint64_t a1,
                                  uint64_t a2, uint64_t a3, double d)
{
    const uint64_t v = omx_abi_call6d(g ? &g->report : NULL, fn, a0, a1, a2, a3, 0, 0, d);

    if (g)
        omx_abi_guard_note(g, call);
    return v;
}

/* a CLAP or LV2 bool result: only the low byte of the return register is defined */
#define OMX_ABI_BOOL(v) ((uint8_t)(v) != 0)

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
