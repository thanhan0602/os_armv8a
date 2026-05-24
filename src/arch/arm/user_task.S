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
    mov     x8, #1              /* SYS_WRITE */
    mov     x0, #1              /* fd = stdout */
    adr     x1, .Lmsg           /* buf = PC-relative address of message */
    mov     x2, #22             /* len = 22 bytes ("[user] hello from EL0\n") */
    svc     #0

    /* sys_yield() */
    mov     x8, #2              /* SYS_YIELD */
    svc     #0

    cmp     x19, #3
    b.lo    .Lloop              /* repeat while counter < 3 */

    /* sys_exit(code=0) */
    mov     x8, #3              /* SYS_EXIT */
    mov     x0, xzr             /* exit code */
    svc     #0

1:  b       1b                  /* unreachable: spin for safety */

.Lmsg:
    .ascii  "[user] hello from EL0\n"  /* 22 bytes, no NUL */

user_task_entry_end:
