/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/*
 * omx_abi_call6 (src/abi_guard.S) against plugins that break the calling convention: each one below, in raw assembly
 * since no C function can, returns with a callee-saved register changed. Through the guard the caller's value comes
 * back, the return value passes, and the report names the register; a plugin that keeps the convention reports none.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/abi_guard.h"

static int failures;
#define CHECK(cond, what) do { if (cond) printf("ok   %s\n", what); else { printf("FAIL %s\n", what); failures++; } } while (0)

uint64_t abi_keeps(uint64_t a);
uint64_t abi_breaks_int(uint64_t a);
uint64_t abi_breaks_fp(uint64_t a);
uint64_t abi_breaks_fpctl(uint64_t a);
double abi_doubles(double d);

#if defined(__aarch64__)
#define INT_REG "x19"
#define FP_REG "d15"
#define INT_BIT OMX_ABI_X19
#define FP_BIT (OMX_ABI_D8 << 7)
typedef double last_t;
#define LAST_VALUE 1.5
#define FPCTL_BIT OMX_ABI_FPCR
__asm__(".text\n.p2align 2\n"
        ".globl abi_keeps\nabi_keeps:\n add x0, x0, #1\n ret\n"
        ".globl abi_breaks_int\nabi_breaks_int:\n mov x19, #0xdead\n add x0, x0, #1\n ret\n"
        ".globl abi_breaks_fp\nabi_breaks_fp:\n fmov d15, xzr\n add x0, x0, #1\n ret\n"
        /* FZ, bit 24: the flush-to-zero a plugin sets for its denormals and leaves set */
        ".globl abi_breaks_fpctl\nabi_breaks_fpctl:\n mrs x9, fpcr\n orr x9, x9, #(1 << 24)\n msr fpcr, x9\n add x0, x0, #1\n ret\n"
        ".globl abi_doubles\nabi_doubles:\n fadd d0, d0, d0\n ret\n");
static uint64_t fpctl(void) { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
#elif defined(__x86_64__)
#define INT_REG "rbx"
#define INT_BIT OMX_ABI_RBX
#define FPCTL_BIT OMX_ABI_MXCSR
__asm__(".text\n.p2align 4\n"
        ".globl abi_keeps\nabi_keeps:\n leaq 1(%rdi), %rax\n ret\n"
        ".globl abi_breaks_int\nabi_breaks_int:\n movq $0xdead, %rbx\n leaq 1(%rdi), %rax\n ret\n"
        ".globl abi_breaks_fp\nabi_breaks_fp:\n movq $0xdead, %r15\n leaq 1(%rdi), %rax\n ret\n"
        /* FZ, bit 15: the flush-to-zero a plugin sets for its denormals and leaves set */
        ".globl abi_breaks_fpctl\nabi_breaks_fpctl:\n subq $8, %rsp\n stmxcsr (%rsp)\n orl $0x8000, (%rsp)\n ldmxcsr (%rsp)\n addq $8, %rsp\n leaq 1(%rdi), %rax\n ret\n"
        ".globl abi_doubles\nabi_doubles:\n addsd %xmm0, %xmm0\n ret\n");
#define FP_REG "r15"
#define FP_BIT (OMX_ABI_R12 << 3)   /* r15: x86-64 keeps no floating-point register across a call */
typedef uint64_t last_t;
#define LAST_VALUE 0xfeedfacecafebeefull
static uint64_t fpctl(void) { uint32_t v; __asm__ volatile("stmxcsr %0" : "=m"(v)); return v; }
#endif

/* six arguments in their places, and with a double beside them: LV2's work() takes five, CLAP's activate a double */
__attribute__((noinline)) static uint64_t six(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)
{
    return a + 10 * b + 100 * c + 1000 * d + 10000 * e + 100000 * f;
}
__attribute__((noinline)) static double six_and_rate(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e,
                                                     uint64_t f, double rate)
{
    return (double)six(a, b, c, d, e, f) + rate;
}

static void through(const char *name, uint64_t (*fn)(uint64_t), uint32_t expect)
{
    struct omx_abi_report r = { 0, 0, 0 };
    char what[160];
    /* known values in the registers the plugins change, as a host keeps its own there: a plugin that writes the value
     * already there changes nothing */
    register uint64_t kept __asm__(INT_REG) = 0x0123456789abcdefull;
#if defined(__aarch64__)
    register last_t last __asm__(FP_REG) = LAST_VALUE;
    __asm__ volatile("" : "+r"(kept), "+w"(last));
    const uint64_t got = omx_abi_call6(&r, (const void *)fn, 41, 0, 0, 0, 0, 0);
    __asm__ volatile("" : "+r"(kept), "+w"(last));
#else
    register last_t last __asm__(FP_REG) = LAST_VALUE;
    __asm__ volatile("" : "+r"(kept), "+r"(last));
    const uint64_t got = omx_abi_call6(&r, (const void *)fn, 41, 0, 0, 0, 0, 0);
    __asm__ volatile("" : "+r"(kept), "+r"(last));
#endif
    snprintf(what, sizeof what, "%s: the caller's " FP_REG " comes back", name);
    CHECK(last == LAST_VALUE, what);
    snprintf(what, sizeof what, "%s: returns its value through the guard", name);
    CHECK(got == 42, what);
    snprintf(what, sizeof what, "%s: the caller's " INT_REG " comes back", name);
    CHECK(kept == 0x0123456789abcdefull, what);
    snprintf(what, sizeof what, "%s: the report says %#x (got %#x), counted %s", name, expect, r.last, expect ? "once" : "never");
    CHECK(r.last == expect && r.ever == expect && r.calls == (expect ? 1u : 0u), what);
}

int main(void)
{
    through("a plugin that keeps the convention", abi_keeps, 0);
    through("a plugin that changes " INT_REG, abi_breaks_int, INT_BIT);
    through("a plugin that changes the last callee-saved register", abi_breaks_fp, FP_BIT);

    const uint64_t before = fpctl();
    through("a plugin that sets flush-to-zero and leaves it", abi_breaks_fpctl, FPCTL_BIT);
    CHECK(fpctl() == before, "the floating-point control is the caller's again after it");

    struct omx_abi_report r = { 0, 0, 0 };
    CHECK(omx_abi_call6f(&r, (const void *)abi_doubles, 0, 0, 0, 0, 0, 0, 24000.0) == 48000.0, "a double argument and result pass through");
    CHECK(r.ever == 0, "and report nothing");

    CHECK(omx_abi_call6(&r, (const void *)six, 1, 2, 3, 4, 5, 6) == 654321, "six arguments reach the plugin in their places");
    CHECK(omx_abi_call6f(&r, (const void *)six_and_rate, 1, 2, 3, 4, 5, 6, 0.5) == 654321.5,
          "and with a double beside them");
    CHECK(omx_abi_call6(NULL, (const void *)abi_breaks_int, 1, 0, 0, 0, 0, 0) == 2, "a NULL report is allowed");
    CHECK(strcmp(omx_abi_reg_name(0), INT_REG) == 0 && omx_abi_reg_name(OMX_ABI_REGS) == NULL, "the names of the bits");
    printf("%d failure(s)\n", failures);
    return failures != 0;
}
