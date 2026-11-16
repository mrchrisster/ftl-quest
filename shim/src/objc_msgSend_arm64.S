    .text
    .align 4

    .extern objc_lookup_imp_for_send
    .extern objc_lookup_imp_for_super2

// id objc_msgSend(id self, SEL op, ...)
    .global objc_msgSend
    .type objc_msgSend, %function
objc_msgSend:
    cbz x0, .Lmsg_ret_nil
    cbz x1, .Lmsg_ret_nil

    // Save x0..x8, x29, x30 and q0..q7 (total 224 bytes, 16-byte aligned)
    sub sp, sp, #224
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x29, [sp, #64]
    str x30, [sp, #80]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    stp q4, q5, [sp, #160]
    stp q6, q7, [sp, #192]

    mov x2, x30
    bl objc_lookup_imp_for_send
    mov x16, x0

    ldp q6, q7, [sp, #192]
    ldp q4, q5, [sp, #160]
    ldp q2, q3, [sp, #128]
    ldp q0, q1, [sp, #96]
    ldr x30, [sp, #80]
    ldp x8, x29, [sp, #64]
    ldp x6, x7, [sp, #48]
    ldp x4, x5, [sp, #32]
    ldp x2, x3, [sp, #16]
    ldp x0, x1, [sp, #0]
    add sp, sp, #224

    cbz x16, .Lmsg_ret_nil
    br x16

.Lmsg_ret_nil:
    mov x0, #0
    fmov d0, xzr
    fmov d1, xzr
    fmov d2, xzr
    fmov d3, xzr
    ret

// id objc_msgSendSuper2(struct objc_super2 *super, SEL op, ...)
    .global objc_msgSendSuper2
    .type objc_msgSendSuper2, %function
objc_msgSendSuper2:
    cbz x0, .Lmsg_ret_nil
    cbz x1, .Lmsg_ret_nil

    sub sp, sp, #224
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x29, [sp, #64]
    str x30, [sp, #80]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    stp q4, q5, [sp, #160]
    stp q6, q7, [sp, #192]

    bl objc_lookup_imp_for_super2
    mov x16, x0

    ldp q6, q7, [sp, #192]
    ldp q4, q5, [sp, #160]
    ldp q2, q3, [sp, #128]
    ldp q0, q1, [sp, #96]
    ldr x30, [sp, #80]
    ldp x8, x29, [sp, #64]
    ldp x6, x7, [sp, #48]
    ldp x4, x5, [sp, #32]
    ldp x2, x3, [sp, #16]
    ldp x0, x1, [sp, #0]
    add sp, sp, #224

    // Replace super_struct pointer with super_struct->receiver
    ldr x0, [x0]
    cbz x16, .Lmsg_ret_nil
    br x16

// int darwin_setjmp(void *env) -- fits inside Darwin 192-byte jmp_buf
    .global darwin_setjmp
    .type darwin_setjmp, %function
darwin_setjmp:
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    mov x9, sp
    stp x29, x30, [x0, #80]
    str x9,       [x0, #96]
    stp d8,  d9,  [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]
    mov w0, #0
    ret

// void darwin_longjmp(void *env, int val)
    .global darwin_longjmp
    .type darwin_longjmp, %function
darwin_longjmp:
    ldp x19, x20, [x0, #0]
    ldp x21, x22, [x0, #16]
    ldp x23, x24, [x0, #32]
    ldp x25, x26, [x0, #48]
    ldp x27, x28, [x0, #64]
    ldp x29, x30, [x0, #80]
    ldr x9,       [x0, #96]
    mov sp, x9
    ldp d8,  d9,  [x0, #104]
    ldp d10, d11, [x0, #120]
    ldp d12, d13, [x0, #136]
    ldp d14, d15, [x0, #152]
    cmp w1, #0
    csinc w0, w1, wzr, ne
    ret

// Darwin ARM64 variadic function trampolines (variadic args at [sp])
    .global darwin_open
    .type darwin_open, %function
darwin_open:
    ldr w2, [sp]
    b darwin_open_va

    .global darwin_fcntl
    .type darwin_fcntl, %function
darwin_fcntl:
    ldr x2, [sp]
    b darwin_fcntl_va

    .global darwin_sprintf_chk
    .type darwin_sprintf_chk, %function
darwin_sprintf_chk:
    mov x4, sp
    b darwin_sprintf_chk_va

    .global darwin_asprintf
    .type darwin_asprintf, %function
darwin_asprintf:
    mov x2, sp
    b darwin_asprintf_va

