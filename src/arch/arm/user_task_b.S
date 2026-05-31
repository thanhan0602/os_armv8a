/*
 * Stage 10 increment 3: second EL0 user task for isolation testing.
 *
 * Runs concurrently with user_task.S.  Each task has its own mm_context
 * (separate TTBR0 page tables), proving that a fault in one process
 * does not affect the other.
 *
 * This task prints "[user-b] hello from EL0\n" three times with yields,
 * then deliberately stores to unmapped VA 0xCAFE0000 to trigger a Data
 * Abort from EL0 and be killed cleanly by the fault handler.
 */

.section .text

.global user_task_b_entry
.global user_task_b_entry_end
.type user_task_b_entry, %function

user_task_b_entry:
    mov     x19, xzr            /* iteration counter = 0 */

.Lloop_b:
    add     x19, x19, #1        /* counter++ */

    /* sys_write(fd=1, buf=.Lmsg_b, len=24) */
    mov     x8, #64             /* SYS_write */
    mov     x0, #1              /* fd = stdout */
    adr     x1, .Lmsg_b         /* buf = PC-relative address of message */
    mov     x2, #24             /* len = 24 bytes ("[user-b] hello from EL0\n") */
    svc     #0

    /* sys_yield() */
    mov     x8, #124            /* SYS_sched_yield */
    svc     #0

    cmp     x19, #3
    b.lo    .Lloop_b            /* repeat while counter < 3 */

    /*
     * Deliberately write to the code page (USER_CODE_VA = 0x10000), which is
     * mapped EL0 read-only (AP[2:1]=11).  This generates a Data Abort with
     * FSC = permission fault at level 3 (0x0F), WnR=1 (write).
     * user_task_a's translation fault (0xDEAD0000) and this permission fault
     * together verify the fault classifier distinguishes both types.
     */
    mov     x0, #0x10000        /* USER_CODE_VA — mapped AP_RO */
    str     x0, [x0]            /* → Permission fault: FAR=0x10000, FSC=0x0F */

    /* unreachable: fault handler kills the task before here */
1:  b       1b

.Lmsg_b:
    .ascii  "[user-b] hello from EL0\n"  /* 24 bytes, no NUL */

user_task_b_entry_end:
