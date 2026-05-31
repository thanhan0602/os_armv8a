/*
 * Stage 9 EL0 demo task.
 *
 * This code runs entirely at EL0 using SVC to invoke kernel syscalls.
 * It is compiled into the kernel image, then copied verbatim to a
 * freshly-allocated user page and mapped into the user lower-half
 * address space.  All instructions are position-independent (only
 * PC-relative adr for the message string and immediate operands).
 *
 * Syscall ABI (Linux-compatible for AArch64):
 *   x8  = syscall number
 *   x0  = first argument
 *   x1  = second argument (sys_write: buf VA)
 *   x2  = third argument  (sys_write: length)
 *   x0  = return value after svc #0 returns
 */

.section .text

.global user_task_entry
.global user_task_entry_end
.type user_task_entry, %function

user_task_entry:
    mov     x19, xzr            /* iteration counter = 0 */

.Lloop:
    add     x19, x19, #1        /* counter++ */

    /* sys_write(fd=1, buf=.Lmsg, len=22) */
    mov     x8, #64             /* SYS_write */
    mov     x0, #1              /* fd = stdout */
    adr     x1, .Lmsg           /* buf = PC-relative address of message */
    mov     x2, #22             /* len = 22 bytes ("[user] hello from EL0\n") */
    svc     #0

    /* sys_yield() */
    mov     x8, #124            /* SYS_sched_yield */
    svc     #0

    cmp     x19, #3
    b.lo    .Lloop              /* repeat while counter < 3 */

    /*
     * Stage 10 user heap smoke test.
     * brk(0) returns the current program break.  Grow it by 14 bytes,
     * copy a string into the new heap space, then write from that buffer.
     */
    mov     x8, #214            /* SYS_brk */
    mov     x0, xzr
    svc     #0
    mov     x20, x0             /* heap base */

    mov     x8, #214            /* SYS_brk */
    add     x0, x20, #14
    svc     #0
    cmp     x0, x20
    b.eq    .Lfault_test

    adr     x1, .Lheap_msg
    mov     x2, #14
    mov     x3, x20

.Lcopy_heap:
    ldrb    w4, [x1], #1
    strb    w4, [x3], #1
    subs    x2, x2, #1
    b.ne    .Lcopy_heap

    mov     x8, #64             /* SYS_write */
    mov     x0, #1
    mov     x1, x20
    mov     x2, #14
    svc     #0

    /*
     * Stage 10 fault test: deliberately store to an unmapped VA.
     * This triggers a Data Abort from EL0 (EC=0x24).  The kernel
     * exception handler logs the faulting address and kills this task.
     */
.Lfault_test:
    mov     x0, #0xDEAD
    lsl     x0, x0, #16         /* x0 = 0x00000000DEAD0000 (unmapped) */
    str     x0, [x0]            /* → Data Abort: FAR=0xDEAD0000 */

    /* unreachable: fault handler kills the task before here */
1:  b       1b

.Lmsg:
    .ascii  "[user] hello from EL0\n"  /* 22 bytes, no NUL */

.Lheap_msg:
    .ascii  "[user] brk ok\n"          /* 14 bytes, no NUL */

user_task_entry_end:
