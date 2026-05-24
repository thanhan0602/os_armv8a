#include <kernel/syscall.h>

#include <kernel/log.h>
#include <kernel/sched.h>

/*
 * sys_write(fd, buf_va, len)
 *
 * fd must be 1 (stdout).  buf_va is a user-space virtual address; because
 * the user's TTBR0 mapping is active when a SVC is taken (mmu_context_switch
 * ran before the task was scheduled), EL1 can read from it directly.
 * The bytes are forwarded to the kernel console one character at a time.
 */
static unsigned long sys_write(unsigned long fd,
                                unsigned long buf_va,
                                unsigned long len)
{
    const char *buf;
    unsigned long i;

    if (fd != 1UL) {
        return (unsigned long)-1;
    }

    buf = (const char *)buf_va;
    for (i = 0UL; i < len; i++) {
        log_putc(buf[i]);
    }

    return len;
}

/*
 * sys_yield()
 *
 * Voluntarily gives up the CPU.  schedule() will pick the next ready
 * task; when this task is re-scheduled the syscall returns 0.
 */
static unsigned long sys_yield(void)
{
    schedule();
    return 0UL;
}

/*
 * sys_exit(code)
 *
 * Marks the current task dead and yields.  task_exit() never returns
 * to this function; the (unreachable) return value is 0.
 */
static unsigned long sys_exit(unsigned long code)
{
    KER_LOGF("[syscall] user task exited code=%lu\n", code);
    task_exit();
    return 0UL;
}

void syscall_dispatch(unsigned long nr, struct exception_context *ctx)
{
    unsigned long ret;

    switch (nr) {
    case SYS_WRITE:
        ret = sys_write(ctx->gpr[0], ctx->gpr[1], ctx->gpr[2]);
        break;
    case SYS_YIELD:
        ret = sys_yield();
        break;
    case SYS_EXIT:
        ret = sys_exit(ctx->gpr[0]);
        ret = 0UL; /* unreachable, but suppresses missing-return warning */
        break;
    default:
        KER_LOGF("[syscall] unknown nr=%lu\n", nr);
        ret = (unsigned long)-1;
        break;
    }

    /* Write return value into the saved x0 slot; restore_context loads it. */
    ctx->gpr[0] = ret;
}
