// insn_test's arm64 side: int run_test(int k, const struct tstate *in, struct tstate *out) runs
// test k of insn_table.S (generated: 16 bytes a test, clrex, the instruction, br x18, nop) with
// every register, sp and the flags from in (but x16, which holds the test's address: no test uses
// it), and stores them all into out. x18 is the way back (no instruction uses it: Android reserves
// it).
// struct tstate { uint64_t x[31]; uint64_t sp; uint64_t nzcv; uint8_t v[32][16]; }
    .text
    .global run_test
    .type run_test, %function
run_test:
    stp x29, x30, [sp, #-176]!
    stp x18, x19, [sp, #16]
    stp x20, x21, [sp, #32]
    stp x22, x23, [sp, #48]
    stp x24, x25, [sp, #64]
    stp x26, x27, [sp, #80]
    stp x28, xzr, [sp, #96]
    stp d8, d9, [sp, #112]
    stp d10, d11, [sp, #128]
    stp d12, d13, [sp, #144]
    stp d14, d15, [sp, #160]
    adrp x16, t_save
    add x16, x16, :lo12:t_save
    mov x17, sp
    str x17, [x16]
    str x2, [x16, #8]
    adrp x17, insn_table
    add x17, x17, :lo12:insn_table
    add x17, x17, x0, lsl #4
    str x17, [x16, #16]
    mov x0, x1
    add x17, x0, #264
    ld1 {v0.16b, v1.16b, v2.16b, v3.16b}, [x17], #64
    ld1 {v4.16b, v5.16b, v6.16b, v7.16b}, [x17], #64
    ld1 {v8.16b, v9.16b, v10.16b, v11.16b}, [x17], #64
    ld1 {v12.16b, v13.16b, v14.16b, v15.16b}, [x17], #64
    ld1 {v16.16b, v17.16b, v18.16b, v19.16b}, [x17], #64
    ld1 {v20.16b, v21.16b, v22.16b, v23.16b}, [x17], #64
    ld1 {v24.16b, v25.16b, v26.16b, v27.16b}, [x17], #64
    ld1 {v28.16b, v29.16b, v30.16b, v31.16b}, [x17], #64
    ldr x17, [x0, #256]
    msr nzcv, x17
    ldr x17, [x0, #248]
    mov sp, x17
    adr x18, 1f
    ldr x16, [x16, #16]
    ldp x2, x3, [x0, #16]
    ldp x4, x5, [x0, #32]
    ldp x6, x7, [x0, #48]
    ldp x8, x9, [x0, #64]
    ldp x10, x11, [x0, #80]
    ldp x12, x13, [x0, #96]
    ldp x14, x15, [x0, #112]
    ldr x19, [x0, #152]
    ldp x20, x21, [x0, #160]
    ldp x22, x23, [x0, #176]
    ldp x24, x25, [x0, #192]
    ldp x26, x27, [x0, #208]
    ldp x28, x29, [x0, #224]
    ldr x30, [x0, #240]
    ldr x17, [x0, #136]
    ldp x0, x1, [x0]
    br x16
1:
    adrp x18, t_save
    add x18, x18, :lo12:t_save
    ldr x18, [x18, #8]
    stp x0, x1, [x18]
    stp x2, x3, [x18, #16]
    stp x4, x5, [x18, #32]
    stp x6, x7, [x18, #48]
    stp x8, x9, [x18, #64]
    stp x10, x11, [x18, #80]
    stp x12, x13, [x18, #96]
    stp x14, x15, [x18, #112]
    stp x16, x17, [x18, #128]
    stp xzr, x19, [x18, #144]
    stp x20, x21, [x18, #160]
    stp x22, x23, [x18, #176]
    stp x24, x25, [x18, #192]
    stp x26, x27, [x18, #208]
    stp x28, x29, [x18, #224]
    str x30, [x18, #240]
    mov x0, sp
    str x0, [x18, #248]
    mrs x0, nzcv
    str x0, [x18, #256]
    add x0, x18, #264
    st1 {v0.16b, v1.16b, v2.16b, v3.16b}, [x0], #64
    st1 {v4.16b, v5.16b, v6.16b, v7.16b}, [x0], #64
    st1 {v8.16b, v9.16b, v10.16b, v11.16b}, [x0], #64
    st1 {v12.16b, v13.16b, v14.16b, v15.16b}, [x0], #64
    st1 {v16.16b, v17.16b, v18.16b, v19.16b}, [x0], #64
    st1 {v20.16b, v21.16b, v22.16b, v23.16b}, [x0], #64
    st1 {v24.16b, v25.16b, v26.16b, v27.16b}, [x0], #64
    st1 {v28.16b, v29.16b, v30.16b, v31.16b}, [x0], #64
    adrp x16, t_save
    add x16, x16, :lo12:t_save
    ldr x17, [x16]
    mov sp, x17
    ldp x18, x19, [sp, #16]
    ldp x20, x21, [sp, #32]
    ldp x22, x23, [sp, #48]
    ldp x24, x25, [sp, #64]
    ldp x26, x27, [sp, #80]
    ldr x28, [sp, #96]
    ldp d8, d9, [sp, #112]
    ldp d10, d11, [sp, #128]
    ldp d12, d13, [sp, #144]
    ldp d14, d15, [sp, #160]
    ldp x29, x30, [sp], #176
    mov w0, #0
    ret
    .size run_test, .-run_test

    .bss
    .align 3
t_save:
    .space 24

#include "insn_table.S"
