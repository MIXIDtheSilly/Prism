// void replay_call(const struct regs *in, struct regs *out, uint64_t fn): runs fn with every
// register (but x16-x18, x30) and sp from in, returning here; stores every register into out.
// struct regs { uint64_t x[31]; uint64_t sp; uint8_t v[32][16]; }
    .text
    .global replay_call
    .type replay_call, %function
replay_call:
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
    adrp x16, g_save
    add x16, x16, :lo12:g_save
    mov x17, sp
    str x17, [x16]
    str x1, [x16, #8]
    str x2, [x16, #16]
    add x17, x0, #256
    ld1 {v0.16b, v1.16b, v2.16b, v3.16b}, [x17], #64
    ld1 {v4.16b, v5.16b, v6.16b, v7.16b}, [x17], #64
    ld1 {v8.16b, v9.16b, v10.16b, v11.16b}, [x17], #64
    ld1 {v12.16b, v13.16b, v14.16b, v15.16b}, [x17], #64
    ld1 {v16.16b, v17.16b, v18.16b, v19.16b}, [x17], #64
    ld1 {v20.16b, v21.16b, v22.16b, v23.16b}, [x17], #64
    ld1 {v24.16b, v25.16b, v26.16b, v27.16b}, [x17], #64
    ld1 {v28.16b, v29.16b, v30.16b, v31.16b}, [x17], #64
    ldr x17, [x0, #248]
    mov sp, x17
    adr x30, 1f
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
    ldp x0, x1, [x0]
    br x16
1:
    adrp x16, g_save
    add x16, x16, :lo12:g_save
    ldr x16, [x16, #8]
    stp x0, x1, [x16]
    stp x2, x3, [x16, #16]
    stp x4, x5, [x16, #32]
    stp x6, x7, [x16, #48]
    stp x8, x9, [x16, #64]
    stp x10, x11, [x16, #80]
    stp x12, x13, [x16, #96]
    stp x14, x15, [x16, #112]
    stp xzr, xzr, [x16, #128]
    stp xzr, x19, [x16, #144]
    stp x20, x21, [x16, #160]
    stp x22, x23, [x16, #176]
    stp x24, x25, [x16, #192]
    stp x26, x27, [x16, #208]
    stp x28, x29, [x16, #224]
    str x30, [x16, #240]
    mov x17, sp
    str x17, [x16, #248]
    add x17, x16, #256
    st1 {v0.16b, v1.16b, v2.16b, v3.16b}, [x17], #64
    st1 {v4.16b, v5.16b, v6.16b, v7.16b}, [x17], #64
    st1 {v8.16b, v9.16b, v10.16b, v11.16b}, [x17], #64
    st1 {v12.16b, v13.16b, v14.16b, v15.16b}, [x17], #64
    st1 {v16.16b, v17.16b, v18.16b, v19.16b}, [x17], #64
    st1 {v20.16b, v21.16b, v22.16b, v23.16b}, [x17], #64
    st1 {v24.16b, v25.16b, v26.16b, v27.16b}, [x17], #64
    st1 {v28.16b, v29.16b, v30.16b, v31.16b}, [x17], #64
    adrp x16, g_save
    add x16, x16, :lo12:g_save
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
    ret
    .size replay_call, .-replay_call

// guest_stub_entry: a stubbed guest call (replay.c's stubs: x16 this, x17 the target). Saves every
// register into g_stub_regs, calls guest_stub(target) on a stack of its own (the replayed code's is
// memory replay compares), restores them, as guest_stub left them, and returns to x30, or goes on
// to the address guest_stub returned (the target's own code).
    .text
    .global guest_stub_entry
    .type guest_stub_entry, %function
guest_stub_entry:
    adrp x16, g_stub_regs
    add x16, x16, :lo12:g_stub_regs
    stp x0, x1, [x16]
    stp x2, x3, [x16, #16]
    stp x4, x5, [x16, #32]
    stp x6, x7, [x16, #48]
    stp x8, x9, [x16, #64]
    stp x10, x11, [x16, #80]
    stp x12, x13, [x16, #96]
    stp x14, x15, [x16, #112]
    stp xzr, x17, [x16, #128]
    stp x18, x19, [x16, #144]
    stp x20, x21, [x16, #160]
    stp x22, x23, [x16, #176]
    stp x24, x25, [x16, #192]
    stp x26, x27, [x16, #208]
    stp x28, x29, [x16, #224]
    str x30, [x16, #240]
    mov x0, sp
    str x0, [x16, #248]
    add x0, x16, #256
    st1 {v0.16b, v1.16b, v2.16b, v3.16b}, [x0], #64
    st1 {v4.16b, v5.16b, v6.16b, v7.16b}, [x0], #64
    st1 {v8.16b, v9.16b, v10.16b, v11.16b}, [x0], #64
    st1 {v12.16b, v13.16b, v14.16b, v15.16b}, [x0], #64
    st1 {v16.16b, v17.16b, v18.16b, v19.16b}, [x0], #64
    st1 {v20.16b, v21.16b, v22.16b, v23.16b}, [x0], #64
    st1 {v24.16b, v25.16b, v26.16b, v27.16b}, [x0], #64
    st1 {v28.16b, v29.16b, v30.16b, v31.16b}, [x0], #64
    adrp x0, g_stub_stack
    add x0, x0, :lo12:g_stub_stack
    add x0, x0, #16, lsl #12
    mov sp, x0
    mov x0, x17
    bl guest_stub
    mov x17, x0
    adrp x16, g_stub_regs
    add x16, x16, :lo12:g_stub_regs
    ldr x0, [x16, #248]
    mov sp, x0
    add x0, x16, #256
    ld1 {v0.16b, v1.16b, v2.16b, v3.16b}, [x0], #64
    ld1 {v4.16b, v5.16b, v6.16b, v7.16b}, [x0], #64
    ld1 {v8.16b, v9.16b, v10.16b, v11.16b}, [x0], #64
    ld1 {v12.16b, v13.16b, v14.16b, v15.16b}, [x0], #64
    ld1 {v16.16b, v17.16b, v18.16b, v19.16b}, [x0], #64
    ld1 {v20.16b, v21.16b, v22.16b, v23.16b}, [x0], #64
    ld1 {v24.16b, v25.16b, v26.16b, v27.16b}, [x0], #64
    ld1 {v28.16b, v29.16b, v30.16b, v31.16b}, [x0], #64
    ldp x0, x1, [x16]
    ldp x2, x3, [x16, #16]
    ldp x4, x5, [x16, #32]
    ldp x6, x7, [x16, #48]
    ldp x8, x9, [x16, #64]
    ldp x10, x11, [x16, #80]
    ldp x12, x13, [x16, #96]
    ldp x14, x15, [x16, #112]
    ldr x18, [x16, #144]
    ldr x19, [x16, #152]
    ldp x20, x21, [x16, #160]
    ldp x22, x23, [x16, #176]
    ldp x24, x25, [x16, #192]
    ldp x26, x27, [x16, #208]
    ldp x28, x29, [x16, #224]
    ldr x30, [x16, #240]
    cbz x17, 1f
    br x17
1:
    ret
    .size guest_stub_entry, .-guest_stub_entry

    .bss
    .align 3
g_save:
    .space 24
