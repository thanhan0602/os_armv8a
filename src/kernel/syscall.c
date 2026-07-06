#include <kernel/syscall.h>

#include <kernel/ipc.h>
#include <kernel/log.h>
#include <kernel/mmu.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/fs.h>
#include <kernel/ramfs.h>
#include <kernel/heap.h>
#include <arch/arm/cpu.h>

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

static unsigned long sys_read(unsigned long fd,
                               unsigned long buf_va,
                               unsigned long len)
{
    struct task *task;
    struct file *file;
    char kbuf[128];
    unsigned long remaining = len;
    unsigned long total_read = 0;

    task = sched_current();
    if (task == (struct task *)0) return (unsigned long)-1;

    if (fd >= MAX_FILES_PER_TASK || task->files[fd] == (struct file *)0) {
        return (unsigned long)-1;
    }

    file = task->files[fd];
    KER_LOGF("[syscall] sys_read: fd=%lu, buf=%p, len=%lu\n", fd, (void*)buf_va, len);

    while (remaining > 0) {
        unsigned long chunk = (remaining < sizeof(kbuf)) ? remaining : sizeof(kbuf);
        unsigned long nread = fs_read(file, kbuf, chunk);
        if (nread == 0) break;

        if (!mmu_copy_to_user(task->mm, buf_va + total_read, kbuf, nread)) {
            return (total_read != 0UL) ? total_read : (unsigned long)-1;
        }

        total_read += nread;
        remaining -= nread;
        if (nread < chunk) break;
    }

    KER_LOGF("[syscall] sys_read: returns %lu\n", total_read);
    return total_read;
}

static unsigned long sys_open(unsigned long path_va)
{
    struct task *task;
    char path[128];
    int fd = -1;
    struct file *file;

    task = sched_current();
    if (task == (struct task *)0) return (unsigned long)-1;

    /* Copy path from user space */
    unsigned long i = 0;
    for (i = 0; i < sizeof(path) - 1; i++) {
        char c;
        if (!mmu_copy_from_user(task->mm, &c, path_va + i, 1)) {
            KER_LOGF("[syscall] sys_open: copy_from_user failed at i=%lu\n", i);
            return (unsigned long)-1;
        }
        path[i] = c;
        if (c == '\0') break;
    }
    path[i] = '\0';
    
    KER_LOGF("[syscall] sys_open: path=\"%s\"\n", path);

    /* Find a free FD */
    for (int j = 0; j < (int)MAX_FILES_PER_TASK; j++) {
        if (task->files[j] == (struct file *)0) {
            fd = j;
            break;
        }
    }

    if (fd == -1) return (unsigned long)-1;

    file = (struct file *)kmalloc(sizeof(struct file));
    if (!file) return (unsigned long)-1;

    if (fs_open(path, file) == 0) {
        kfree(file);
        return (unsigned long)-1;
    }

    task->files[fd] = file;
    return (unsigned long)fd;
}

/*
 * sys_yield()
 *
 * Voluntarily gives up the CPU.  schedule() will pick the next ready
 * task; when this task is re-scheduled the syscall returns 0.
 */
static unsigned long sys_mmap(unsigned long addr, unsigned long len,
                              unsigned long prot, unsigned long flags,
                              unsigned long fd, unsigned long offset)
{
    struct task *task = sched_current();
    if (!task || !task->process) return (unsigned long)-1;

    /* Simplified mmap: we just add a region */
    /* In a real OS, we would look for a free range if addr is 0 */
    /* and handle file vs anonymous mappings */
    
    struct file *file = (struct file *)0;
    if (!(flags & 0x20)) { /* NOT MAP_ANONYMOUS (Linux value is 0x20) */
        if (fd < MAX_FILES_PER_TASK) {
            file = task->files[fd];
        }
    }

    /* We only support mapping at specific addresses for now (fixed or hint) */
    unsigned long start = addr;
    if (start == 0) {
        /* Find a free spot. For now, just use somewhere high in heap area */
        static unsigned long next_mmap_addr = 0x7000000000UL;
        start = next_mmap_addr;
        next_mmap_addr += (len + 0xFFFUL) & ~0xFFFUL;
    }

    unsigned long mmu_flags = MMU_USER_PAGE_NORMAL | MMU_USER_PAGE_AF;
    if (prot & 0x1) mmu_flags |= MMU_USER_PAGE_AP_RO; /* PROT_READ */
    if (prot & 0x2) mmu_flags = (mmu_flags & ~MMU_USER_PAGE_AP_RO) | MMU_USER_PAGE_AP_RW; /* PROT_WRITE */
    if (!(prot & 0x4)) mmu_flags |= MMU_USER_PAGE_UXN; /* NO PROT_EXEC */

    /* For internal simplify, we just use VM_TYPE_ELF to reuse lazy loader if it's a file */
    /* Actually we should have a VM_TYPE_FILE */
    
    if (file) {
        /* File mapping - we need to pass the image pointer if it's in ramfs */
        /* Currently our ramfs files are just pointers to data */
        /* We'll need a way to get the data pointer from the file */
        extern void *ramfs_get_data_ptr(struct file *file); // I'll add this
        void *data = ramfs_get_data_ptr(file);
        if (!data) return (unsigned long)-1;

        if (!process_add_region(task->process, start, start + len,
                               VM_TYPE_ELF, mmu_flags,
                               (const unsigned char *)data, offset, file->size - offset)) {
            return (unsigned long)-1;
        }
    } else {
        /* Anonymous mapping */
        if (!process_add_region(task->process, start, start + len,
                               VM_TYPE_HEAP, mmu_flags,
                               (const unsigned char *)0, 0, 0)) {
            return (unsigned long)-1;
        }
    }

    return start;
}

static unsigned long sys_yield(void)
{
    schedule();
    return 0UL;
}

static unsigned long sys_getpid(void)
{
    struct task *task = sched_current();
    if (task == (struct task *)0) {
        return (unsigned long)-1;
    }
    return task->id;
}

static unsigned long sys_getcpu(void)
{
    return (unsigned long)arch_get_cpu_id();
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

static unsigned long sys_nanosleep(unsigned long seconds)
{
    struct task *t = sched_current();
    if (!t) return (unsigned long)-1;

    /* Our timer frequency is currently 2Hz (timer_interval = frequency / 2) */
    /* So 1 second = 2 ticks */
    t->sleep_ticks = seconds * 2;
    t->state = TASK_STATE_BLOCKED;
    
    schedule();
    
    return 0;
}

void syscall_dispatch(unsigned long nr, struct exception_context *ctx)
{
    unsigned long ret;

    switch (nr) {
    case SYS_WRITE:
        ret = sys_write(ctx->gpr[0], ctx->gpr[1], ctx->gpr[2]);
        break;
    case SYS_READ:
        ret = sys_read(ctx->gpr[0], ctx->gpr[1], ctx->gpr[2]);
        break;
    case SYS_OPEN:
        ret = sys_open(ctx->gpr[0]);
        break;
    case SYS_MMAP:
        ret = sys_mmap(ctx->gpr[0], ctx->gpr[1], ctx->gpr[2], ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]);
        break;
    case SYS_YIELD:
        ret = sys_yield();
        break;
    case SYS_GETPID:
        ret = sys_getpid();
        break;
    case SYS_GETCPU:
        ret = sys_getcpu();
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
    case SYS_NANOSLEEP:
        ret = sys_nanosleep(ctx->gpr[0]);
        break;
    default:
        KER_LOGF("[syscall] unknown nr=%lu\n", nr);
        ret = (unsigned long)-1;
        break;
    }

    /* Write return value into the saved x0 slot; restore_context loads it. */
    ctx->gpr[0] = ret;
}
