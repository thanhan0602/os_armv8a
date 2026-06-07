.section .text

.global switch_context
.type switch_context, %function

/*
 * switch_context(struct task_context *old, struct task_context *new)
 *
 * Save callee-saved registers (x19-x30, SP) into *old,
 * restore them from *new, and return to new->x30.
 *
 * This is called from schedule() inside the timer IRQ handler.
 * The full GPR/SIMD frame is already on each task's stack courtesy
 * of save_context in exception_vectors.S.
 */
switch_context:
    /* Save callee-saved regs to old context (x0) */
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x9, sp
    str x9, [x0, #96]

    /* Restore callee-saved regs from new context (x1) */
    ldp x19, x20, [x1, #0]
    ldp x21, x22, [x1, #16]
    ldp x23, x24, [x1, #32]
    ldp x25, x26, [x1, #48]
    ldp x27, x28, [x1, #64]
    ldp x29, x30, [x1, #80]
    ldr x9, [x1, #96]
    mov sp, x9

    ret

.global task_entry_trampoline
.type task_entry_trampoline, %function

/*
 * First-run entry for newly created tasks.
 *
 * Reached via switch_context when a task is scheduled for the first
 * time (x30 was set to this address by task_create).
 *
 * x19 = task entry function pointer (set by task_create)
 *
 * Since we arrive here from inside an IRQ handler, PSTATE.I is set
 * (IRQs masked).  Unmask IRQs so the task can be preempted by future
 * timer interrupts.
 */
task_entry_trampoline:
    bl sched_new_task_kickoff
    msr daifclr, #2
    isb
    blr x19
    bl task_exit
1:
    wfe

.global el0_entry_trampoline
.type el0_entry_trampoline, %function

/*
 * First-run entry for EL0 user tasks.
 *
 * Reached via switch_context when a user task runs for the first time
 * (x30 was set to this address by task_create_user).
 *
 * x19 = EL0 entry virtual address (set by task_create_user)
 * x20 = EL0 user stack pointer    (set by task_create_user)
 *
 * Sets ELR_EL1, SPSR_EL1 (EL0t, DAIF=0 so IRQs are enabled at EL0),
 * and SP_EL0, then eret.  The kernel stack (SP_EL1) is left clean so
 * subsequent EL0->EL1 exceptions naturally use it for save_context.
 */
el0_entry_trampoline:
    bl sched_new_task_kickoff
    msr     elr_el1, x19        /* EL0 program counter */
    msr     sp_el0,  x20        /* EL0 stack pointer */
    mov     x0,  xzr
    msr     spsr_el1, x0        /* EL0t mode, DAIF=0 (all interrupts enabled) */
    isb
    /* Zero all caller-visible registers so user code starts clean. */
    mov     x0,  xzr
    mov     x1,  xzr
    mov     x2,  xzr
    mov     x3,  xzr
    mov     x4,  xzr
    mov     x5,  xzr
    mov     x6,  xzr
    mov     x7,  xzr
    mov     x8,  xzr
    mov     x9,  xzr
    mov     x10, xzr
    mov     x11, xzr
    mov     x12, xzr
    mov     x13, xzr
    mov     x14, xzr
    mov     x15, xzr
    mov     x16, xzr
    mov     x17, xzr
    mov     x18, xzr
    mov     x19, xzr
    mov     x20, xzr
    mov     x21, xzr
    mov     x22, xzr
    mov     x23, xzr
    mov     x24, xzr
    mov     x25, xzr
    mov     x26, xzr
    mov     x27, xzr
    mov     x28, xzr
    mov     x29, xzr
    mov     x30, xzr
    eret
    b 1b
