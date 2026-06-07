#include <kernel/syscall.h>

#include <kernel/ipc.h>
#include <kernel/log.h>
#include <kernel/mmu.h>
#include <kernel/process.h>
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
    struct task *task;
    struct file *file;
    char kbuf[128];
    unsigned long remaining = len;
    unsigned long total_written = 0;

    task = sched_current();
    if (task == (struct task *)0) {
        return (unsigned long)-1;
    }

    if (fd >= MAX_FILES_PER_TASK || task->files[fd] == (struct file *)0) {
        return (unsigned long)-1;
    }

    file = task->files[fd];

    while (remaining > 0) {
        unsigned long chunk = (remaining < sizeof(kbuf)) ? remaining : sizeof(kbuf);
        if (!mmu_copy_from_user(task->mm, kbuf, buf_va + total_written, chunk)) {
            return (total_written != 0UL) ? total_written : (unsigned long)-1;
        }

        unsigned long written = fs_write(file, kbuf, chunk);
        if (written == 0) {
            break;
        }
        total_written += written;
        remaining -= written;
        if (written < chunk) break;
    }

    return total_written;
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
    struct task *task = sched_current();
    if (task != (struct task *)0) {
        task->exit_status = (int)code;
    }
    KER_LOGF("[syscall] user task exited code=%lu\n", code);
    task_exit();
    return 0UL;
}

static unsigned long sys_brk(unsigned long new_break)
{
    struct task *task;

    task = sched_current();
    if (task == (struct task *)0 || task->process == (struct process *)0) {
        return 0UL;
    }

    return process_brk(task->process, new_break);
}

static unsigned long sys_munmap(unsigned long addr, unsigned long len)
{
    struct task *t = sched_current();
    if (t == (struct task *)0 || t->mm == (struct mm_context *)0) {
        return (unsigned long)-1;
    }
    return (unsigned long)(mmu_unmap_user_range(t->mm, addr, len) ? 0 : -1);
}

static unsigned long sys_wait4(long pid, unsigned long status_ptr)
{
    return sched_wait4(pid, status_ptr);
}

static unsigned long sys_ipc_send(unsigned long channel_id,
                                  unsigned long buf_va,
                                  unsigned long len)
{
    struct task *task;
    unsigned char buffer[IPC_MESSAGE_MAX];
    unsigned long index;
    long result;

    if (len == 0UL || len > IPC_MESSAGE_MAX) {
        return (unsigned long)-1;
    }

    task = sched_current();
    if (task == (struct task *)0 || task->mm == (struct mm_context *)0) {
        return (unsigned long)-1;
    }

    for (index = 0UL; index < len; index++) {
        if (!mmu_copy_from_user(task->mm, &buffer[index], buf_va + index, 1UL)) {
            return (unsigned long)-1;
        }
    }

    result = ipc_send(channel_id, buffer, len);
    return (result < 0L) ? (unsigned long)-1 : (unsigned long)result;
}

static unsigned long sys_ipc_recv(unsigned long channel_id,
                                  unsigned long buf_va,
                                  unsigned long capacity)
{
    struct task *task;
    unsigned char buffer[IPC_MESSAGE_MAX];
    long result;

    if (capacity == 0UL || capacity > IPC_MESSAGE_MAX) {
        return (unsigned long)-1;
    }

    task = sched_current();
    if (task == (struct task *)0 || task->mm == (struct mm_context *)0) {
        return (unsigned long)-1;
    }

    for (;;) {
        unsigned long index;

        result = ipc_receive(channel_id, task, buffer, capacity);
        if (result == IPC_RESULT_BLOCKED) {
            schedule();
            continue;
        }

        if (result < 0L) {
            return (unsigned long)-1;
        }

        for (index = 0UL; index < (unsigned long)result; index++) {
            if (!mmu_copy_to_user(task->mm, buf_va + index, &buffer[index], 1UL)) {
                return (unsigned long)-1;
            }
        }

        return (unsigned long)result;
    }
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
    case SYS_BRK:
        ret = sys_brk(ctx->gpr[0]);
        break;
    case SYS_MUNMAP:
        ret = sys_munmap(ctx->gpr[0], ctx->gpr[1]);
        break;
    case SYS_FORK:
        ret = process_fork(ctx);
        break;
    case SYS_WAIT4:
        ret = sys_wait4(ctx->gpr[0], ctx->gpr[1]);
        break;
    case SYS_IPC_SEND:
        ret = sys_ipc_send(ctx->gpr[0], ctx->gpr[1], ctx->gpr[2]);
        break;
    case SYS_IPC_RECV:
        ret = sys_ipc_recv(ctx->gpr[0], ctx->gpr[1], ctx->gpr[2]);
        break;
    default:
        KER_LOGF("[syscall] unknown nr=%lu\n", nr);
        ret = (unsigned long)-1;
        break;
    }

    /* Write return value into the saved x0 slot; restore_context loads it. */
    ctx->gpr[0] = ret;
}
