#include <kernel/sched.h>

#include <kernel/log.h>
#include <kernel/ipc.h>
#include <kernel/mmu.h>
#include <kernel/mutex.h>
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
    if (state == TASK_STATE_REAPING) {
        return "reaping";
    }

    return "unknown";
}

static void sched_clear_task(struct task *task)
{
    if (!task) return;
    for (unsigned long i = 0; i < sizeof(struct task); i++) {
        ((unsigned char *)task)[i] = 0;
    }
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
    
    /* Zero out all tasks */
    for (unsigned long i = 0; i < MAX_TASKS; i++) {
        sched_clear_task(&tasks[i]);
    }

    /*
     * CPUs 0-3 get idle tasks in slots 0-3.
     */
    for (unsigned int i = 0; i < 4; i++) {
        idle = &tasks[i];
        idle->id = next_task_id++;
        idle->state = TASK_STATE_RUNNING;
        idle->current_cpu = i;
        idle->stack_base = (void *)0;
        idle->stack_size = 0;
        
        char *name = (char *)page_alloc_contiguous(1); // Leak but it's init
        pa_to_va(name); // not really needed as it is PA=VA for now or we use pa_to_va
        /* Actually simpler just use static names */
        static const char *idle_names[] = {"idle0", "idle1", "idle2", "idle3"};
        idle->name = idle_names[i];
        
        /* Circular list setup: 0->1->2->3->0 */
        idle->next = &tasks[(i + 1) % 4];

        for (unsigned int j = 0; j < MAX_FILES_PER_TASK; j++) {
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
    t->current_cpu = TASK_NO_CPU;
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

    t->state = TASK_STATE_READY;
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
    t->current_cpu = TASK_NO_CPU;
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
    t->context.tpidr_el0 = 0;

    struct task *curr = arch_get_current_task();
    t->next = curr->next;
    curr->next = t;

    t->state = TASK_STATE_BLOCKED;
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
    for (;;) {
        struct task *victim = (struct task *)0;
        struct task *prev;
        struct task *t;
        void *stack_base;
#ifdef CONFIG_KERNEL_VIRTUAL
        struct process *process;
#endif
        unsigned long flags = spin_lock_irqsave(&sched_lock);

        /*
         * Detach one dead task while holding only sched_lock. Cleanup is
         * deliberately performed after dropping sched_lock so IPC, mutex,
         * process, MM and allocator locks can never form a reverse edge back
         * into the scheduler.
         */
        prev = &tasks[0];
        t = prev->next;
        while (t != &tasks[0]) {
            if (t->state == TASK_STATE_DEAD &&
                t->current_cpu == TASK_NO_CPU) {
                prev->next = t->next;
                t->state = TASK_STATE_REAPING;
                victim = t;
                break;
            }
            prev = t;
            t = t->next;
        }

        if (victim == (struct task *)0) {
            spin_unlock_irqrestore(&sched_lock, flags);
            return;
        }

        stack_base = victim->stack_base;
        victim->stack_base = (void *)0;
#ifdef CONFIG_KERNEL_VIRTUAL
        process = victim->process;
        victim->process = (struct process *)0;
        victim->mm = (struct mm_context *)0;
#endif
        spin_unlock_irqrestore(&sched_lock, flags);

        ipc_detach_task(victim);
        mutex_detach_task(victim);

        if (stack_base != (void *)0) {
            page_free_contiguous((void *)va_to_pa(stack_base),
                                 TASK_TOTAL_PAGES);
        }

#ifdef CONFIG_KERNEL_VIRTUAL
        if (process != (struct process *)0) {
            process_destroy(process);
        }
#endif

        flags = spin_lock_irqsave(&sched_lock);
        sched_clear_task(victim);
        spin_unlock_irqrestore(&sched_lock, flags);
    }
}

void sched_new_task_kickoff(void)
{
    spin_unlock(&sched_lock);
}

void sched_tick(void)
{
    unsigned long flags = spin_lock_irqsave(&sched_lock);
    int woken = 0;

    for (unsigned long i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->name && t->state == TASK_STATE_BLOCKED && t->sleep_ticks > 0) {
            t->sleep_ticks--;
            if (t->sleep_ticks == 0) {
                t->state = TASK_STATE_READY;
                woken = 1;
            }
        }
    }

    spin_unlock_irqrestore(&sched_lock, flags);

    if (woken) {
        /* Send IPI to other cores to inform them a new task is READY */
        unsigned int current_cpu = arch_get_cpu_id();
        unsigned int target_mask = 0;
        for (unsigned int i = 0; i < 4; i++) {
            if (i != current_cpu) {
                target_mask |= (1 << (unsigned char)i);
            }
        }
        gicv2_send_ipi(target_mask, 0);
    }
}

void schedule(void)
{
    struct task *prev;
    struct task *next;
    unsigned int cpu_id = arch_get_cpu_id();
    
    /* Reaping performs subsystem cleanup without sched_lock held. */
    sched_reap_dead();

    unsigned long flags = spin_lock_irqsave(&sched_lock);

    prev = arch_get_current_task();

    /*
     * A remote kill only sets kill_pending while the task is RUNNING. The
     * target CPU consumes it here, after entering the scheduler on its own
     * stack, and only then makes the task eligible for reaping.
     */
    if (prev->id >= 4UL && prev->kill_pending != 0U) {
        prev->kill_pending = 0U;
        prev->state = TASK_STATE_DEAD;
    }
    next = prev->next;

    /* Round-robin: find next ready task */
    while (next != prev) {
        if (next->state == TASK_STATE_READY) {
            /* 
             * Task 0-3 are idle tasks for CPU 0-3.
             * Only allow a CPU to pick its own idle task.
             */
            if (next->id < 4) {
                if (next->id == cpu_id) {
                    break;
                }
            } else {
                break;
            }
        }
        next = next->next;
    }

    /* 
     * If we didn't search and find a DIFFERENT ready task:
     * Check if the current task can keep running or if we must switch to idle.
     */
    if (next == prev) {
        /* If current is READY/RUNNING and is not a pinned idle task of another CPU, just keep it. */
        if (prev->state == TASK_STATE_RUNNING || prev->state == TASK_STATE_READY) {
            if (prev->id >= 4 || prev->id == cpu_id) {
                spin_unlock_irqrestore(&sched_lock, flags);
                return;
            }
        }
        
        /* Current task blocked or belongs to another CPU. Switch to our idle task. */
        next = &tasks[cpu_id];
        if (next == prev) {
            spin_unlock_irqrestore(&sched_lock, flags);
            return;
        }
    }

    if (prev->state == TASK_STATE_RUNNING) {
        prev->state = TASK_STATE_READY;
    }
    if (prev->id >= 4UL) {
        prev->current_cpu = TASK_NO_CPU;
    }
    next->state = TASK_STATE_RUNNING;
    next->current_cpu = cpu_id;
    
    /* 
    KER_LOGF("[sched] CPU %u: %s -> %s\n", arch_get_cpu_id(), prev->name, next->name);
    */

    arch_set_current_task(next);
    switch_count++;

#ifdef CONFIG_KERNEL_VIRTUAL
    mmu_context_switch(next->mm);
#endif

    /* 
     * IMPORTANT: We hold the spinlock across switch_context!
     * The task we switch to will release the lock in spin_unlock_irqrestore below
     * OR in sched_new_task_kickoff for new tasks.
     */
    switch_context(&prev->context, &next->context);
    
    /* When we are switched back to, we release the lock */
    spin_unlock_irqrestore(&sched_lock, flags);
}

void task_exit(void)
{
    struct task *curr = arch_get_current_task();
    
    KER_LOGF("[sched] task %s exiting\n", curr->name);

    unsigned long flags = spin_lock_irqsave(&sched_lock);
    curr->state = TASK_STATE_ZOMBIE;

    /* Wake up parent if it's waiting */
    for (unsigned long i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->name && t->id == curr->parent_id && t->state == TASK_STATE_BLOCKED) {
            t->state = TASK_STATE_READY;
            
            /* Send IPI to other cores */
            unsigned int current_cpu = arch_get_cpu_id();
            unsigned int target_mask = 0;
            for (unsigned int j = 0; j < 4; j++) {
                if (j != current_cpu) {
                    target_mask |= (1 << (unsigned char)j);
                }
            }
            gicv2_send_ipi(target_mask, 0);
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
    if (task == (struct task *)0) {
        return;
    }

    unsigned long flags = spin_lock_irqsave(&sched_lock);
    if (task->id >= 4UL &&
        task->state != TASK_STATE_DEAD &&
        task->state != TASK_STATE_ZOMBIE &&
        task->state != TASK_STATE_REAPING &&
        task->kill_pending == 0U) {
        task->state = TASK_STATE_BLOCKED;
    }
    spin_unlock_irqrestore(&sched_lock, flags);
}

void sched_wake_task(struct task *task)
{
    int woken = 0;

    if (task == (struct task *)0) {
        return;
    }

    unsigned long flags = spin_lock_irqsave(&sched_lock);
    if (task->state == TASK_STATE_BLOCKED && task->kill_pending == 0U) {
        task->state = TASK_STATE_READY;
        woken = 1;
    }
    spin_unlock_irqrestore(&sched_lock, flags);

    if (!woken) {
        return;
    }

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

/*
 * Park/unpark form a one-bit wakeup handshake for subsystem wait queues.
 * A producer may unpark before the consumer reaches sched_park_task(); the
 * pending token is then consumed instead of blocking. This lets IPC and
 * mutex code publish/remove waiters under their own locks without calling
 * into the scheduler while those locks are held.
 */
int sched_park_task(struct task *task)
{
    unsigned long flags;
    int blocked = 0;

    if (task == (struct task *)0 || task->id < 4UL) {
        return 0;
    }

    flags = spin_lock_irqsave(&sched_lock);
    if (task->state != TASK_STATE_DEAD &&
        task->state != TASK_STATE_ZOMBIE &&
        task->state != TASK_STATE_REAPING &&
        task->kill_pending == 0U) {
        if (task->wake_pending != 0U) {
            task->wake_pending = 0U;
        } else {
            task->state = TASK_STATE_BLOCKED;
            blocked = 1;
        }
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    return blocked;
}

void sched_unpark_task(struct task *task)
{
    unsigned long flags;
    int woken = 0;

    if (task == (struct task *)0) {
        return;
    }

    flags = spin_lock_irqsave(&sched_lock);
    if (task->state == TASK_STATE_BLOCKED && task->kill_pending == 0U) {
        task->state = TASK_STATE_READY;
        woken = 1;
    } else if (task->state != TASK_STATE_DEAD &&
               task->state != TASK_STATE_ZOMBIE &&
               task->state != TASK_STATE_REAPING &&
               task->kill_pending == 0U) {
        task->wake_pending = 1U;
    }
    spin_unlock_irqrestore(&sched_lock, flags);

    if (woken) {
        unsigned int current_cpu = arch_get_cpu_id();
        unsigned int target_mask = 0U;
        for (unsigned int i = 0U; i < 4U; i++) {
            if (i != current_cpu) {
                target_mask |= (1U << i);
            }
        }
        gicv2_send_ipi(target_mask, 0U);
    }
}

int sched_sleep_current(unsigned long ticks)
{
    struct task *task = sched_current();
    unsigned long flags;

    if (task == (struct task *)0 || task->id < 4UL) {
        return 0;
    }

    flags = spin_lock_irqsave(&sched_lock);
    if (task->state == TASK_STATE_DEAD ||
        task->state == TASK_STATE_ZOMBIE ||
        task->state == TASK_STATE_REAPING ||
        task->kill_pending != 0U) {
        spin_unlock_irqrestore(&sched_lock, flags);
        return 0;
    }

    task->sleep_ticks = ticks;
    task->state = (ticks == 0UL) ? TASK_STATE_READY : TASK_STATE_BLOCKED;
    spin_unlock_irqrestore(&sched_lock, flags);
    return 1;
}

void sched_set_current_exit_status(int status)
{
    struct task *task = sched_current();
    unsigned long flags;

    if (task == (struct task *)0) {
        return;
    }

    flags = spin_lock_irqsave(&sched_lock);
    task->exit_status = status;
    spin_unlock_irqrestore(&sched_lock, flags);
}

unsigned long sched_wait4(long pid, unsigned long status_ptr)
{
    struct task *curr = sched_current();

    for (;;) {
        int found_child = 0;
        unsigned long flags = spin_lock_irqsave(&sched_lock);

        for (unsigned long i = 0; i < MAX_TASKS; i++) {
            struct task *t = &tasks[i];
            if (t->name && t->parent_id == curr->id && (pid <= 0 || (long)t->id == pid)) {
                found_child = 1;
                if (t->state == TASK_STATE_ZOMBIE) {
                    unsigned long child_id = t->id;
                    int status = t->exit_status;
                    t->state = TASK_STATE_DEAD; /* Mark for reaping by schedule() */
                    spin_unlock_irqrestore(&sched_lock, flags);

                    if (status_ptr != 0) {
                        extern int mmu_copy_to_user(const struct mm_context *mm, unsigned long va, const void *src, unsigned long len);
                        mmu_copy_to_user(curr->mm, status_ptr, &status, sizeof(int));
                    }
                    return child_id;
                }
            }
        }

        if (!found_child) {
            spin_unlock_irqrestore(&sched_lock, flags);
            return (unsigned long)-1;
        }

        curr->state = TASK_STATE_BLOCKED;
        spin_unlock_irqrestore(&sched_lock, flags);
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

        KER_LOGF("[shell]   id=%lu state=%s cpu=%u name=%s mode=%s",
                 task->id,
                 sched_state_name(task->state),
                 task->current_cpu,
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
    unsigned int target_cpu = TASK_NO_CPU;
    int killed = 0;
    unsigned long flags = spin_lock_irqsave(&sched_lock);

    for (index = 0UL; index < MAX_TASKS; index++) {
        struct task *task;

        task = &tasks[index];
        if (task->name == (const char *)0 || task->id != task_id) {
            continue;
        }

        if (task == sched_current() || task->id < 4UL ||
            task->state == TASK_STATE_DEAD ||
            task->state == TASK_STATE_ZOMBIE ||
            task->state == TASK_STATE_REAPING) {
            break;
        }

        if (task->state == TASK_STATE_RUNNING &&
            task->current_cpu != TASK_NO_CPU) {
            task->kill_pending = 1U;
            target_cpu = task->current_cpu;
        } else {
            task->state = TASK_STATE_DEAD;
            task->sleep_ticks = 0UL;
            task->wake_pending = 0U;
        }
        killed = 1;
        break;
    }

    spin_unlock_irqrestore(&sched_lock, flags);

    if (killed && target_cpu != TASK_NO_CPU) {
        gicv2_send_ipi(1U << target_cpu, 0U);
    }

    return killed;
}
