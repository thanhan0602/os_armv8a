#include <kernel/sched.h>

#include <kernel/log.h>
#include <kernel/ipc.h>
#include <kernel/mmu.h>
#include <kernel/page_alloc.h>
#include <kernel/process.h>
#include <kernel/vm.h>
#include <kernel/console.h>
#include <kernel/spinlock.h>
#include <drivers/interrupt/gicv2.h>
#include <arch/arm/cpu.h>

extern void task_entry_trampoline(void);

struct task tasks[MAX_TASKS];
static unsigned long next_task_id;
static struct spinlock sched_lock = SPINLOCK_INITIALIZER;
static unsigned long switch_count;

static const char *sched_state_name(unsigned long state)
{
    if (state == TASK_STATE_RUNNING) {
        return "running";
    }
    if (state == TASK_STATE_READY) {
        return "ready";
    }
    if (state == TASK_STATE_BLOCKED) {
        return "blocked";
    }
    if (state == TASK_STATE_DEAD) {
        return "dead";
    }
    if (state == TASK_STATE_ZOMBIE) {
        return "zombie";
    }

    return "unknown";
}

static void sched_clear_task(struct task *task)
{
    task->context.x19 = 0UL;
    task->context.x20 = 0UL;
    task->context.x21 = 0UL;
    task->context.x22 = 0UL;
    task->context.x23 = 0UL;
    task->context.x24 = 0UL;
    task->context.x25 = 0UL;
    task->context.x26 = 0UL;
    task->context.x27 = 0UL;
    task->context.x28 = 0UL;
    task->context.x29 = 0UL;
    task->context.x30 = 0UL;
    task->context.sp = 0UL;
    task->id = 0UL;
    task->state = 0UL;
    task->stack_base = (void *)0;
    task->stack_size = 0UL;
    task->name = (const char *)0;
    task->mm = (struct mm_context *)0;
    task->process = (struct process *)0;
    task->next = (struct task *)0;
}

static struct task *sched_allocate_task_slot(void)
{
    unsigned long index;

    for (index = 4UL; index < MAX_TASKS; index++) {
        if (tasks[index].name == (const char *)0) {
            return &tasks[index];
        }
    }

    return (struct task *)0;
}

void sched_init(void)
{
    struct task *idle;

    next_task_id = 0;
    switch_count = 0;

    /*
     * CPUs 0-3 get idle tasks in slots 0-3.
     */
    for (unsigned int i = 0; i < 4; i++) {
        idle = &tasks[i];
        idle->id = next_task_id++;
        idle->state = TASK_STATE_RUNNING;
        idle->stack_base = (void *)0;
        idle->stack_size = 0;
        
        char *name = (char *)page_alloc_contiguous(1); // Leak but it's init
        pa_to_va(name); // not really needed as it is PA=VA for now or we use pa_to_va
        /* Actually simpler just use static names */
        static const char *idle_names[] = {"idle0", "idle1", "idle2", "idle3"};
        idle->name = idle_names[i];
        
        /* Circular list setup: 0->1->2->3->0 */
        idle->next = &tasks[(i + 1) % 4];

        for (int j = 0; j < MAX_FILES_PER_TASK; j++) {
            idle->files[j] = (struct file *)0;
        }
    }

    arch_set_current_task(&tasks[0]);
}

struct task *task_create(task_fn_t entry, const char *name)
{
    struct task *t;
    void *stack_pa;
    unsigned char *stack_va;
    unsigned long sp;

    unsigned long flags = spin_lock_irqsave(&sched_lock);

    t = sched_allocate_task_slot();
    if (t == (struct task *)0) {
        spin_unlock_irqrestore(&sched_lock, flags);
        KER_INFO("task_create: max tasks reached");
        return (struct task *)0;
    }

    /*
     * Allocate guard page + usable stack contiguously.
     */
    stack_pa = page_alloc_contiguous(TASK_TOTAL_PAGES);
    if (stack_pa == (void *)0) {
        spin_unlock_irqrestore(&sched_lock, flags);
        KER_INFO("task_create: stack alloc failed");
        return (struct task *)0;
    }

    stack_va = (unsigned char *)pa_to_va(stack_pa);
    sp = (unsigned long)(stack_va + TASK_TOTAL_PAGES * PAGE_SIZE);

    /* AArch64 requires 16-byte aligned SP */
    sp &= ~0xFUL;

    t->id = next_task_id;
    t->state = TASK_STATE_READY;
    t->stack_base = stack_va;
    t->stack_size = TASK_TOTAL_PAGES * PAGE_SIZE;
    t->name = name;
    t->mm = (struct mm_context *)0;
    t->process = (struct process *)0;

    /* Initial callee-saved context: x19=entry, x30=trampoline, SP=top */
    t->context.x19 = (unsigned long)entry;
    t->context.x20 = 0;
    t->context.x21 = 0;
    t->context.x22 = 0;
    t->context.x23 = 0;
    t->context.x24 = 0;
    t->context.x25 = 0;
    t->context.x26 = 0;
    t->context.x27 = 0;
    t->context.x28 = 0;
    t->context.x29 = 0;
    t->context.x30 = (unsigned long)task_entry_trampoline;
    t->context.sp = sp;

    /* Insert into circular list after current task */
    struct task *curr = arch_get_current_task();
    t->next = curr->next;
    curr->next = t;

    next_task_id++;

    spin_unlock_irqrestore(&sched_lock, flags);

    return t;
}

#ifdef CONFIG_KERNEL_VIRTUAL
struct task *task_create_user(struct process *process,
                               const char *name)
{
    struct task *t;
    void *stack_pa;
    unsigned char *stack_va;
    unsigned long sp;

    if (process == (struct process *)0 || process->mm == (struct mm_context *)0) {
        KER_INFO("task_create_user: invalid process");
        return (struct task *)0;
    }

    unsigned long flags = spin_lock_irqsave(&sched_lock);

    t = sched_allocate_task_slot();
    if (t == (struct task *)0) {
        spin_unlock_irqrestore(&sched_lock, flags);
        KER_INFO("task_create_user: max tasks reached");
        return (struct task *)0;
    }

    stack_pa = page_alloc_contiguous(TASK_TOTAL_PAGES);
    if (stack_pa == (void *)0) {
        spin_unlock_irqrestore(&sched_lock, flags);
        KER_INFO("task_create_user: kernel stack alloc failed");
        return (struct task *)0;
    }

    stack_va = (unsigned char *)pa_to_va(stack_pa);
    sp = (unsigned long)(stack_va + TASK_TOTAL_PAGES * PAGE_SIZE);
    sp &= ~0xFUL;

    t->id = next_task_id;
    t->state = TASK_STATE_READY;
    t->stack_base = stack_va;
    t->stack_size = TASK_TOTAL_PAGES * PAGE_SIZE;
    t->name = name;
    t->mm = process->mm;
    t->process = process;

    for (int i = 0; i < MAX_FILES_PER_TASK; i++) {
        t->files[i] = (struct file *)0;
    }
    t->files[1] = console_open_file();

    /* x19 = user entry VA, x20 = user SP_EL0, x30 = el0_entry_trampoline */
    t->context.x19 = process->entry_va;
    t->context.x20 = process->stack_top;
    t->context.x21 = 0;
    t->context.x22 = 0;
    t->context.x23 = 0;
    t->context.x24 = 0;
    t->context.x25 = 0;
    t->context.x26 = 0;
    t->context.x27 = 0;
    t->context.x28 = 0;
    t->context.x29 = 0;
    t->context.x30 = (unsigned long)el0_entry_trampoline;
    t->context.sp = sp;

    struct task *curr = arch_get_current_task();
    t->next = curr->next;
    curr->next = t;

    next_task_id++;

    spin_unlock_irqrestore(&sched_lock, flags);

    return t;
}
#endif /* CONFIG_KERNEL_VIRTUAL */

/*
 * Reclaim resources from dead tasks.  Called at the start of schedule()
 * so we are running on the current (live) task's stack, never on a dead
 * task's stack that is about to be freed.
 */
static void sched_reap_dead(void)
{
    struct task *prev;
    struct task *t;
    struct task *curr = arch_get_current_task();

    prev = curr;
    t = curr->next;

    while (t != curr) {
        if (t->state == TASK_STATE_DEAD) {
            prev->next = t->next;

            ipc_detach_task(t);

            if (t->stack_base != (void *)0) {
                page_free_contiguous(
                    (void *)va_to_pa(t->stack_base),
                    TASK_TOTAL_PAGES);
                t->stack_base = (void *)0;
            }

#ifdef CONFIG_KERNEL_VIRTUAL
            if (t->process != (struct process *)0) {
                process_destroy(t->process);
                t->process = (struct process *)0;
                t->mm = (struct mm_context *)0;
            }
#endif

            sched_clear_task(t);

            t = prev->next;
        } else {
            prev = t;
            t = t->next;
        }
    }
}

void sched_new_task_kickoff(void)
{
    spin_unlock(&sched_lock);
}

void schedule(void)
{
    struct task *prev;
    struct task *next;
    
    unsigned long flags = spin_lock_irqsave(&sched_lock);

    sched_reap_dead();

    prev = arch_get_current_task();
    next = prev->next;

    /* Round-robin: find next ready task, skip dead ones */
    while (next != prev) {
        if (next->state == TASK_STATE_READY) {
            break;
        }
        next = next->next;
    }

    if (next == prev) {
        spin_unlock_irqrestore(&sched_lock, flags);
        return;
    }

    if (prev->state == TASK_STATE_RUNNING) {
        prev->state = TASK_STATE_READY;
    }
    next->state = TASK_STATE_RUNNING;
    
    arch_set_current_task(next);
    switch_count++;

#ifdef CONFIG_KERNEL_VIRTUAL
    mmu_context_switch(next->mm);
#endif

    /* 
     * IMPORTANT: We hold the spinlock across switch_context!
     * The task we switch to will release the lock.
     * This is a standard pattern in SMP schedulers.
     */
    switch_context(&prev->context, &next->context);
    
    /* When we are switched back to, we release the lock */
    spin_unlock_irqrestore(&sched_lock, flags);
}

void task_exit(void)
{
    struct task *curr = arch_get_current_task();
    
    unsigned long flags = spin_lock_irqsave(&sched_lock);
    curr->state = TASK_STATE_ZOMBIE;

    /* Wake up parent if it's waiting */
    for (unsigned long i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->name && t->id == curr->parent_id && t->state == TASK_STATE_BLOCKED) {
            t->state = TASK_STATE_READY;
            break;
        }
    }
    
    spin_unlock_irqrestore(&sched_lock, flags);

    schedule();

    while (1) {
        cpu_wfe();
    }
}

struct task *sched_current(void)
{
    return arch_get_current_task();
}

void sched_block_task(struct task *task)
{
    if (task == (struct task *)0 || task->id < 4UL || task->state == TASK_STATE_DEAD) {
        return;
    }

    unsigned long flags = spin_lock_irqsave(&sched_lock);
    task->state = TASK_STATE_BLOCKED;
    spin_unlock_irqrestore(&sched_lock, flags);
}

void sched_wake_task(struct task *task)
{
    if (task == (struct task *)0 || task->state != TASK_STATE_BLOCKED) {
        return;
    }

    unsigned long flags = spin_lock_irqsave(&sched_lock);
    task->state = TASK_STATE_READY;
    spin_unlock_irqrestore(&sched_lock, flags);

    /* Send IPI to all other cores to trigger schedule() */
    unsigned int current_cpu = arch_get_cpu_id();
    unsigned int target_mask = 0;
    for (unsigned int i = 0; i < 4; i++) {
        if (i != current_cpu) {
            target_mask |= (1 << i);
        }
    }
    gicv2_send_ipi(target_mask, 0);
}

unsigned long sched_wait4(long pid, unsigned long status_ptr)
{
    struct task *curr = sched_current();

    for (;;) {
        int found_child = 0;
        for (unsigned long i = 0; i < MAX_TASKS; i++) {
            struct task *t = &tasks[i];
            if (t->name && t->parent_id == curr->id && (pid <= 0 || (long)t->id == pid)) {
                found_child = 1;
                if (t->state == TASK_STATE_ZOMBIE) {
                    unsigned long child_id = t->id;
                    if (status_ptr != 0) {
                        int status = t->exit_status;
                        extern int mmu_copy_to_user(const struct mm_context *mm, unsigned long va, const void *src, unsigned long len);
                        mmu_copy_to_user(curr->mm, status_ptr, &status, sizeof(int));
                    }
                    t->state = TASK_STATE_DEAD; /* Mark for reaping by schedule() */
                    return child_id;
                }
            }
        }

        if (!found_child) return (unsigned long)-1;

        curr->state = TASK_STATE_BLOCKED;
        schedule();
    }
}

void sched_dump_tasks(void)
{
    unsigned long index;

    log_write("[shell] task list:\n");
    for (index = 0UL; index < MAX_TASKS; index++) {
        struct task *task;

        task = &tasks[index];
        if (task->name == (const char *)0) {
            continue;
        }

        KER_LOGF("[shell]   id=%lu state=%s name=%s mode=%s",
                 task->id,
                 sched_state_name(task->state),
                 task->name,
                 task->process != (struct process *)0 ? "user" : "kernel");
#ifdef CONFIG_KERNEL_VIRTUAL
        if (task->process != (struct process *)0) {
            KER_LOGF(" brk=%lx", task->process->brk);
        }
#endif
        log_write("\n");
    }
}

int sched_kill_task(unsigned long task_id)
{
    unsigned long index;

    for (index = 0UL; index < MAX_TASKS; index++) {
        struct task *task;

        task = &tasks[index];
        if (task->name == (const char *)0 || task->id != task_id) {
            continue;
        }

        if (task == sched_current() || task->id < 4UL || task->state == TASK_STATE_DEAD) {
            return 0;
        }

        ipc_detach_task(task);
        task->state = TASK_STATE_DEAD;
        return 1;
    }

    return 0;
}
