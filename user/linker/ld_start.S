.section .text.start
.global _start
.hidden _start
.hidden ld_self_relocate
.hidden ld_main

_start:
    /* 
     * Khi Kernel nạp ld.so, SP đang trỏ tới Stack chứa AUX Vectors, Env, Args.
     */
    
    /* Tính toán base address thực tế */
    /* Một cách khác: ADR lấy địa chỉ thực, còn ldr x1, .Llink_start lấy hằng số từ data */
    adr x0, _start
    adrp x1, .Llink_start
    ldr x1, [x1, :lo12:.Llink_start]
    sub x0, x0, x1  /* x0 = load_bias (e.g., 0x1000) */
    
    /* Lấy địa chỉ RUNTIME của mảng _DYNAMIC */
    adrp x1, _DYNAMIC
    add x1, x1, :lo12:_DYNAMIC

    /* Gọi hàm self-relocation trực tiếp */
    bl ld_self_relocate

    /* Sau khi relocate xong, gọi main của linker */
    mov x0, sp
    bl ld_main

    /* Linker không bao giờ nên quay lại đây */
1:  wfe
    b 1b

.section .data
.align 3
.Llink_start:
    .quad _start

.section .text
.global ld_plt_resolver
.hidden ld_plt_resolver
ld_plt_resolver:
    /*
     * AArch64 lazy binding entry. On entry (branched here from libc PLT0):
     *   x16          = &GOT[3]  (so GOT[2]=obj is at [x16, #-8])
     *   [sp, #0]     = &GOT[n]  (the function's own GOT slot, pushed by PLT0)
     *   [sp, #8]     = x30      (the original caller's return address)
     *   x17          = address of this resolver (unused here)
     *
     * We must resolve the symbol, patch GOT[n], restore the argument
     * registers, unwind the PLT0 frame and tail-call the real function.
     * We preserve x0-x9 and q0-q7 to be 100% ABI compliant for all arg types.
     */

    /* Save SIMD/FP registers (q0-q7) for float/vector arguments */
    stp q0, q1, [sp, #-32]!
    stp q2, q3, [sp, #-32]!
    stp q4, q5, [sp, #-32]!
    stp q6, q7, [sp, #-32]!

    /* Save integer argument / indirect-result registers (x0-x8) and x9. */
    stp x0, x1, [sp, #-16]!
    stp x2, x3, [sp, #-16]!
    stp x4, x5, [sp, #-16]!
    stp x6, x7, [sp, #-16]!
    stp x8, x9, [sp, #-16]!

    /* 
     * Arguments for ld_lazy_resolve(struct ld_object *obj, unsigned long *got_entry_va)
     *
     * In AArch64 PLT0 (on entry to ld_plt_resolver):
     *   stp x16, x30, [sp, #-16]!  <-- App's PLT0 pushed &GOT[n] and LR
     * 
     * Total bytes pushed here: 128 (Q) + 80 (X) = 208 bytes.
     * Original PLT0 frame is now at [sp, #208].
     */
    ldr x0, [x16, #-8]     /* x0 = obj = GOT[2] (Linker-internal data) */
    ldr x1, [sp, #208]     /* x1 = &GOT[n] (saved by PLT0) */

    bl ld_lazy_resolve

    /* Result in x0 is the resolved function address */
    mov x16, x0

    /* Restore integer argument / indirect-result registers */
    ldp x8, x9, [sp], #16
    ldp x6, x7, [sp], #16
    ldp x4, x5, [sp], #16
    ldp x2, x3, [sp], #16
    ldp x0, x1, [sp], #16

    /* Restore SIMD/FP registers */
    ldp q6, q7, [sp], #32
    ldp q4, q5, [sp], #32
    ldp q2, q3, [sp], #32
    ldp q0, q1, [sp], #32

    /* 
     * Unwind the PLT0 frame.
     * PLT0 pushed [&GOT[n], x30]. x30 is the original LR of the caller.
     */
    ldr x30, [sp, #8]
    add sp, sp, #16

    br x16
