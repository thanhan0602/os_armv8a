.section .text.vectors
.align 11

.equ GPR_CTX_SIZE, 256
.equ ELR_OFFSET,  248   /* ELR_EL1 saved here */
.equ SPSR_OFFSET, 256   /* SPSR_EL1 saved here */
.equ SP_EL0_OFFSET, 264 /* SP_EL0 saved here */
.equ FPCR_OFFSET, 272
.equ SIMD_OFFSET, 288   /* was 272 */
.equ CTX_SIZE,    800   /* was 784 (+16 for ELR+SPSR) */

.global exception_vector_table
.type exception_vector_table, %function

.macro vector_slot label
    .balign 128
    b \label
.endm

.macro save_context
    sub sp, sp, #CTX_SIZE
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    /* Save ELR_EL1 and SPSR_EL1 so they survive task switches. */
    mrs x9, elr_el1
    mrs x10, spsr_el1
    stp x9, x10, [sp, #ELR_OFFSET]  /* ELR at 248, SPSR at 256 */
    mrs x9, sp_el0
    str x9, [sp, #SP_EL0_OFFSET]
    mrs x9, fpcr
    mrs x10, fpsr
    stp x9, x10, [sp, #FPCR_OFFSET]
    stp q0, q1, [sp, #SIMD_OFFSET + 0]
    stp q2, q3, [sp, #SIMD_OFFSET + 32]
    stp q4, q5, [sp, #SIMD_OFFSET + 64]
    stp q6, q7, [sp, #SIMD_OFFSET + 96]
    stp q8, q9, [sp, #SIMD_OFFSET + 128]
    stp q10, q11, [sp, #SIMD_OFFSET + 160]
    stp q12, q13, [sp, #SIMD_OFFSET + 192]
    stp q14, q15, [sp, #SIMD_OFFSET + 224]
    stp q16, q17, [sp, #SIMD_OFFSET + 256]
    stp q18, q19, [sp, #SIMD_OFFSET + 288]
    stp q20, q21, [sp, #SIMD_OFFSET + 320]
    stp q22, q23, [sp, #SIMD_OFFSET + 352]
    stp q24, q25, [sp, #SIMD_OFFSET + 384]
    stp q26, q27, [sp, #SIMD_OFFSET + 416]
    stp q28, q29, [sp, #SIMD_OFFSET + 448]
    stp q30, q31, [sp, #SIMD_OFFSET + 480]
.endm

.macro restore_context
    ldp q0, q1, [sp, #SIMD_OFFSET + 0]
    ldp q2, q3, [sp, #SIMD_OFFSET + 32]
    ldp q4, q5, [sp, #SIMD_OFFSET + 64]
    ldp q6, q7, [sp, #SIMD_OFFSET + 96]
    ldp q8, q9, [sp, #SIMD_OFFSET + 128]
    ldp q10, q11, [sp, #SIMD_OFFSET + 160]
    ldp q12, q13, [sp, #SIMD_OFFSET + 192]
    ldp q14, q15, [sp, #SIMD_OFFSET + 224]
    ldp q16, q17, [sp, #SIMD_OFFSET + 256]
    ldp q18, q19, [sp, #SIMD_OFFSET + 288]
    ldp q20, q21, [sp, #SIMD_OFFSET + 320]
    ldp q22, q23, [sp, #SIMD_OFFSET + 352]
    ldp q24, q25, [sp, #SIMD_OFFSET + 384]
    ldp q26, q27, [sp, #SIMD_OFFSET + 416]
    ldp q28, q29, [sp, #SIMD_OFFSET + 448]
    ldp q30, q31, [sp, #SIMD_OFFSET + 480]
    ldp x9, x10, [sp, #FPCR_OFFSET]
    msr fpcr, x9
    msr fpsr, x10
    /* Restore ELR_EL1 and SPSR_EL1 before eret. */
    ldp x9, x10, [sp, #ELR_OFFSET]
    msr elr_el1, x9
    msr spsr_el1, x10
    ldr x9, [sp, #SP_EL0_OFFSET]
    msr sp_el0, x9
    isb
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    add sp, sp, #CTX_SIZE
.endm

.macro sync_handler label, id
\label:
    save_context
    mov x0, #\id
    b exception_sync_entry
.endm

.macro irq_handler label, id
\label:
    save_context
    mov x0, #\id
    b exception_irq_entry
.endm

exception_vector_table:
    vector_slot vector_0_sync
    vector_slot vector_1_irq
    vector_slot vector_2_sync
    vector_slot vector_3_sync
    vector_slot vector_4_sync
    vector_slot vector_5_irq
    vector_slot vector_6_sync
    vector_slot vector_7_sync
    vector_slot vector_8_sync
    vector_slot vector_9_irq
    vector_slot vector_10_sync
    vector_slot vector_11_sync
    vector_slot vector_12_sync
    vector_slot vector_13_irq
    vector_slot vector_14_sync
    vector_slot vector_15_sync

sync_handler vector_0_sync, 0
irq_handler vector_1_irq, 1
sync_handler vector_2_sync, 2
sync_handler vector_3_sync, 3
sync_handler vector_4_sync, 4

irq_handler vector_5_irq, 5
sync_handler vector_6_sync, 6
sync_handler vector_7_sync, 7
sync_handler vector_8_sync, 8
irq_handler vector_9_irq, 9
sync_handler vector_10_sync, 10
sync_handler vector_11_sync, 11
sync_handler vector_12_sync, 12
irq_handler vector_13_irq, 13
sync_handler vector_14_sync, 14
sync_handler vector_15_sync, 15

exception_sync_entry:
    mrs x1, esr_el1
    mrs x2, elr_el1
    mrs x3, spsr_el1
    mrs x4, far_el1
    mov x5, sp
    bl exception_handle_sync
    /* Non-zero return value means the exception was handled; eret back. */
    cbz x0, 1f
    restore_context
    eret

.global fork_child_exit
fork_child_exit:
    restore_context
    eret

1:
    wfe
    b 1b

exception_irq_entry:
    mrs x1, esr_el1
    mrs x2, elr_el1
    mrs x3, spsr_el1
    mrs x4, far_el1
    mov x5, sp
    bl exception_handle_irq
    restore_context
    eret