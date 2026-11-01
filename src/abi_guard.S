/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/*
 * omx_abi_call4: a call into a plugin that the plugin cannot break the host's frame with (src/abi_guard.h).
 *
 *   uint64_t omx_abi_call4(struct omx_abi_report *r, const void *fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
 *
 * calls fn(a0, a1, a2, a3) and returns what it returned. The registers the calling convention says a callee preserves
 * are saved before the call, compared after it and put back, so a plugin that returns with one changed changes nothing
 * of the host's; r->last is the mask of those it changed (OMX_ABI_*), r->ever their union, r->calls the count of calls
 * that changed any. The floating-point argument registers are passed through untouched (an LV2 instantiate's rate).
 *
 * In assembly because no C function can do it: the compiler owns these registers inside any function it writes.
 */

#if defined(OMX_ABI_LV2)
#define OMX_ABI_CALL4 omx_lv2_abi_call4     /* the LV2 adapter's archive carries its own copy, so both link in one host */
#define OMX_ABI_CALL4D omx_lv2_abi_call4d
#define OMX_ABI_CALL4F omx_lv2_abi_call4f
#else
#define OMX_ABI_CALL4 omx_abi_call4
#define OMX_ABI_CALL4D omx_abi_call4d
#define OMX_ABI_CALL4F omx_abi_call4f
#endif

/* the same code under the names of its other C signatures (src/abi_guard.h): one with a double argument, which arrives
 * in the first floating-point register and is left there for fn, and one returning fn's double */
#define OMX_ABI_ALIASES \
    .globl OMX_ABI_CALL4D; .set OMX_ABI_CALL4D, OMX_ABI_CALL4; \
    .globl OMX_ABI_CALL4F; .set OMX_ABI_CALL4F, OMX_ABI_CALL4

#if defined(__aarch64__)

/* the frame, from sp: x29 x30 | x19..x28 | d8..d15 | report fpcr */
    .text
    .p2align 2
    .globl OMX_ABI_CALL4
#if defined(OMX_ABI_LV2)
    .hidden OMX_ABI_CALL4
#endif
    .type OMX_ABI_CALL4, %function
OMX_ABI_CALL4:
    .cfi_startproc
    stp x29, x30, [sp, #-176]!
    .cfi_def_cfa_offset 176
    .cfi_offset 29, -176
    .cfi_offset 30, -168
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mrs x9, fpcr
    stp x0, x9, [sp, #160]
    mov x9, x1
    mov x0, x2
    mov x1, x3
    mov x2, x4
    mov x3, x5
    blr x9
    /* x0 and d0 hold what fn returned: only x9..x17 are used from here */
    mov x10, #0
    .macro omx_abi_x reg, off, bit
    ldr x11, [sp, #\off]
    cmp \reg, x11
    cset x12, ne
    orr x10, x10, x12, lsl #\bit
    .endm
    .macro omx_abi_d reg, off, bit
    ldr x11, [sp, #\off]
    fmov x13, \reg
    cmp x13, x11
    cset x12, ne
    orr x10, x10, x12, lsl #\bit
    .endm
    omx_abi_x x19, 16, 0
    omx_abi_x x20, 24, 1
    omx_abi_x x21, 32, 2
    omx_abi_x x22, 40, 3
    omx_abi_x x23, 48, 4
    omx_abi_x x24, 56, 5
    omx_abi_x x25, 64, 6
    omx_abi_x x26, 72, 7
    omx_abi_x x27, 80, 8
    omx_abi_x x28, 88, 9
    omx_abi_d d8, 96, 10
    omx_abi_d d9, 104, 11
    omx_abi_d d10, 112, 12
    omx_abi_d d11, 120, 13
    omx_abi_d d12, 128, 14
    omx_abi_d d13, 136, 15
    omx_abi_d d14, 144, 16
    omx_abi_d d15, 152, 17
    mrs x13, fpcr
    ldr x11, [sp, #168]
    cmp x13, x11
    cset x12, ne
    orr x10, x10, x12, lsl #18
    mov x11, sp
    cmp x29, x11
    cset x12, ne
    orr x10, x10, x12, lsl #19
    /* put back what the plugin was given to keep */
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x25, x26, [sp, #64]
    ldp x27, x28, [sp, #80]
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    ldp x11, x9, [sp, #160]
    msr fpcr, x9
    /* the report: last, ever |= last, calls += (last != 0); none when r is NULL */
    cbz x11, 1f
    str w10, [x11, #4]
    ldr w12, [x11]
    orr w12, w12, w10
    str w12, [x11]
    cbz x10, 1f
    ldr x12, [x11, #8]
    add x12, x12, #1
    str x12, [x11, #8]
1:
    ldp x29, x30, [sp], #176
    .cfi_def_cfa_offset 0
    ret
    .cfi_endproc
    .size OMX_ABI_CALL4, . - OMX_ABI_CALL4
    OMX_ABI_ALIASES
#if defined(OMX_ABI_LV2)
    .hidden OMX_ABI_CALL4D
    .hidden OMX_ABI_CALL4F
#endif

#elif defined(__x86_64__)

/* System V: rdi report, rsi fn, rdx rcx r8 r9 the arguments; xmm0..7 pass through. The frame, from rsp after the
 * pushes and the sub: [0] report [8] mxcsr [12] x87 control word [16..55] spare | r15 r14 r13 r12 rbx rbp | return */
    .text
    .p2align 4
    .globl OMX_ABI_CALL4
#if defined(OMX_ABI_LV2)
    .hidden OMX_ABI_CALL4
#endif
    .type OMX_ABI_CALL4, @function
OMX_ABI_CALL4:
    .cfi_startproc
    pushq %rbp
    .cfi_def_cfa_offset 16
    .cfi_offset %rbp, -16
    movq %rsp, %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $56, %rsp
    .cfi_def_cfa_offset 112
    movq %rdi, (%rsp)
    stmxcsr 8(%rsp)
    fnstcw 12(%rsp)
    movq %rsi, %rax
    movq %rdx, %rdi
    movq %rcx, %rsi
    movq %r8, %rdx
    movq %r9, %rcx
    callq *%rax
    /* rax and xmm0 hold what fn returned: only r10 r11 and the flags are used from here */
    xorl %r10d, %r10d
    .macro omx_abi_q reg, off, bit
    cmpq \off(%rsp), \reg
    setne %r11b
    movzbl %r11b, %r11d
    shll $\bit, %r11d
    orl %r11d, %r10d
    .endm
    omx_abi_q %rbx, 88, 0
    leaq 96(%rsp), %r11
    cmpq %r11, %rbp
    setne %r11b
    movzbl %r11b, %r11d
    shll $1, %r11d
    orl %r11d, %r10d
    omx_abi_q %r12, 80, 2
    omx_abi_q %r13, 72, 3
    omx_abi_q %r14, 64, 4
    omx_abi_q %r15, 56, 5
    /* MXCSR's control bits (6..15: DAZ, the masks, rounding, FZ) and the x87 control word are the callee's to keep */
    stmxcsr 16(%rsp)
    movl 16(%rsp), %r11d
    xorl 8(%rsp), %r11d
    testl $0xffc0, %r11d
    setne %r11b
    movzbl %r11b, %r11d
    shll $6, %r11d
    orl %r11d, %r10d
    fnstcw 20(%rsp)
    movzwl 20(%rsp), %r11d
    cmpw 12(%rsp), %r11w
    setne %r11b
    movzbl %r11b, %r11d
    shll $7, %r11d
    orl %r11d, %r10d
    ldmxcsr 8(%rsp)
    fldcw 12(%rsp)
    cld
    /* the report: last, ever |= last, calls += (last != 0); none when r is NULL */
    movq (%rsp), %r11
    testq %r11, %r11
    jz 1f
    movl %r10d, 4(%r11)
    orl %r10d, (%r11)
    testl %r10d, %r10d
    jz 1f
    incq 8(%r11)
1:
    addq $56, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    .cfi_def_cfa_offset 8
    retq
    .cfi_endproc
    .size OMX_ABI_CALL4, . - OMX_ABI_CALL4
    OMX_ABI_ALIASES
#if defined(OMX_ABI_LV2)
    .hidden OMX_ABI_CALL4D
    .hidden OMX_ABI_CALL4F
#endif

#else
#error "omx_abi_call4 has no implementation for this architecture"
#endif

    .section .note.GNU-stack, "", %progbits
