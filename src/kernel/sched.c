#include <kernel/sched.h>

#include <kernel/log.h>
#include <kernel/ipc.h>
#include <kernel/mmu.h>
#include <kernel/mutex.h>
#include <kernel/semaphore.h>
#include <kernel/page_alloc.h>
#include <kernel/process.h>
#include <kernel/vm.h>
#include <kernel/console.h>
#include <kernel/condvar.h>
#include <kernel/spinlock.h>
#include <drivers/interrupt/gic.h>
#include <arch/arm/cpu.h>

extern void task_entry_trampoline(void);

struct task tasks[MAX_TASKS];
static unsigned long next_task_id;
static struct spinlock sched_lock = SPINLOCK_INITIALIZER;
static unsigned long switch_count[SCHED_MAX_CPUS];
static unsigned long init_task_id;
static unsigned int task_slot_exhaustion_reported;
static unsigned int next_enqueue_cpu;

struct sched_run_queue {
    struct task *head;
    struct task *tail;
    unsigned int nr_running;
    unsigned int need_resched;
    unsigned int irq_depth;
};

static struct sched_run_queue run_queues[SCHED_MAX_CPUS];

#define SCHED_NO_PARENT (~0UL)

static void sched_rq_init(struct sched_run_queue *rq)
{
    rq->head = (struct task *)0;
    rq->tail = (struct task *)0;
    rq->nr_running = 0U;
    rq->need_resched = 0U;
    rq->irq_depth = 0U;
}

static void sched_rq_enqueue_locked(struct task *task, unsigned int cpu)
{
    struct sched_run_queue *rq;

    if (task == (struct task *)0 || task->id < SCHED_MAX_CPUS ||
        task->on_rq != 0U || cpu >= SCHED_MAX_CPUS) {
        return;
    }

    rq = &run_queues[cpu];
    task->rq_cpu = cpu;
    task->rq_next = (struct task *)0;
    task->on_rq = 1U;
    if (rq->tail == (struct task *)0) {
        rq->head = task;
        rq->tail = task;
    } else {
        rq->tail->rq_next = task;
        rq->tail = task;
    }
    rq->nr_running++;
}

static struct task *sched_rq_dequeue_locked(unsigned int cpu)
{
    struct sched_run_queue *rq = &run_queues[cpu];
    struct task *task = rq->head;

    if (task == (struct task *)0) {
        return (struct task *)0;
    }

    rq->head = task->rq_next;
    if (rq->head == (struct task *)0) {
        rq->tail = (struct task *)0;
    }
    task->rq_next = (struct task *)0;
    task->on_rq = 0U;
    if (rq->nr_running > 0U) {
        rq->nr_running--;
    }
    return task;
}

static int sched_rq_remove_locked(struct task *task)
{
    struct sched_run_queue *rq;
    struct task *previous = (struct task *)0;
    struct task *current;

    if (task == (struct task *)0 || task->on_rq == 0U ||
        task->rq_cpu >= SCHED_MAX_CPUS) {
        return 0;
    }

    rq = &run_queues[task->rq_cpu];
    current = rq->head;
    while (current != (struct task *)0) {
        if (current == task) {
            if (previous == (struct task *)0) {
                rq->head = current->rq_next;
            } else {
                previous->rq_next = current->rq_next;
            }
            if (rq->tail == current) {
                rq->tail = previous;
            }
            current->rq_next = (struct task *)0;
            current->on_rq = 0U;
            if (rq->nr_running > 0U) {
                rq->nr_running--;
            }
            return 1;
        }
        previous = current;
        current = current->rq_next;
    }

    task->on_rq = 0U;
    task->rq_next = (struct task *)0;
    return 0;
}

static unsigned int sched_least_loaded_cpu_locked(void)
{
    unsigned int best = next_enqueue_cpu;
    unsigned int best_load = run_queues[best].nr_running;

    /*
     * Start each search at a rotating CPU. A queue length alone does not
     * include the task currently running on that CPU, so several rapid task
     * creations can otherwise all choose CPU 0 after it dequeues each task.
     * Rotating equal-load choices preserves least-loaded placement while
     * distributing bursts across all CPUs.
     */
    for (unsigned int offset = 1U; offset < SCHED_MAX_CPUS; offset++) {
        unsigned int cpu = (next_enqueue_cpu + offset) % SCHED_MAX_CPUS;
        unsigned int load = run_queues[cpu].nr_running;

        if (load < best_load) {
            best = cpu;
            best_load = load;
        }
    }

    next_enqueue_cpu = (best + 1U) % SCHED_MAX_CPUS;
    return best;
}

static struct task *sched_pick_next_locked(unsigned int cpu)
{
    struct task *task;
    unsigned int busiest = cpu;

    /* Prefer work already assigned to this CPU. Discard stale queue entries. */
    do {
        task = sched_rq_dequeue_locked(cpu);
    } while (task != (struct task *)0 && task->state != TASK_STATE_READY);

    if (task != (struct task *)0) {
        return task;
    }

    /* Idle CPUs steal one runnable task from the busiest remote queue. */
    for (unsigned int other = 0U; other < SCHED_MAX_CPUS; other++) {
        if (run_queues[other].nr_running > run_queues[busiest].nr_running) {
            busiest = other;
        }
    }

    if (busiest == cpu || run_queues[busiest].nr_running == 0U) {
        return (struct task *)0;
    }

    do {
        task = sched_rq_dequeue_locked(busiest);
    } while (task != (struct task *)0 && task->state != TASK_STATE_READY);

    if (task != (struct task *)0) {
        task->rq_cpu = cpu;
    }
    return task;
}

static void sched_make_ready_locked(struct task *task)
{
    unsigned int cpu;

    if (task == (struct task *)0 || task->id < SCHED_MAX_CPUS) {
        return;
    }
    task->state = TASK_STATE_READY;
    cpu = sched_least_loaded_cpu_locked();
    sched_rq_enqueue_locked(task, cpu);
    run_queues[cpu].need_resched = 1U;
}

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
    init_task_id = SCHED_NO_PARENT;
    task_slot_exhaustion_reported = 0U;
    next_enqueue_cpu = 0U;

    for (unsigned int cpu = 0U; cpu < SCHED_MAX_CPUS; cpu++) {
        sched_rq_init(&run_queues[cpu]);
        switch_count[cpu] = 0UL;
    }
    
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
        idle->rq_cpu = i;
        idle->on_rq = 0U;
        idle->preempt_count = 0U;
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
    unsigned int target_cpu;
    unsigned int current_cpu;

    unsigned long flags = spin_lock_irqsave(&sched_lock);

    t = sched_allocate_task_slot();
    if (t == (struct task *)0) {
        spin_unlock_irqrestore(&sched_lock, flags);
        if (task_slot_exhaustion_reported == 0U) {
            task_slot_exhaustion_reported = 1U;
            KER_INFO("task_create: max tasks reached (suppressing repeats)");
        }
        return (struct task *)0;
    }
    task_slot_exhaustion_reported = 0U;

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
    t->rq_cpu = TASK_NO_CPU;
    t->on_rq = 0U;
    t->preempt_count = 0U;
    t->stack_base = stack_va;
    t->stack_size = TASK_TOTAL_PAGES * PAGE_SIZE;
    t->name = name;
    t->mm = (struct mm_context *)0;
    t->process = (struct process *)0;
    /*
     * Kernel tasks are parentless unless a caller explicitly assigns a
     * parent. Leaving this field zero accidentally makes idle0 their parent,
     * so exited stress tasks remain uncollectable ZOMBIEs and eventually
     * exhaust MAX_TASKS.
     */
    t->parent_id = SCHED_NO_PARENT;

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

    sched_make_ready_locked(t);
    target_cpu = t->rq_cpu;
    next_task_id++;

    spin_unlock_irqrestore(&sched_lock, flags);

    /*
     * A newly queued task may have been assigned to a remote CPU that is
     * sleeping in WFE. The full regression suite generated enough unrelated
     * interrupts to hide this omission, while the focused scheduler test
     * exposed it. Kick the selected CPU after dropping sched_lock so it can
     * immediately observe its non-empty local run queue.
     */
    current_cpu = arch_get_cpu_id();
    if (target_cpu < SCHED_MAX_CPUS && target_cpu != current_cpu) {
        gic_send_ipi(1U << target_cpu, 0U);
    }

    return t;
}

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
    t->rq_cpu = TASK_NO_CPU;
    t->on_rq = 0U;
    t->preempt_count = 0U;
    t->stack_base = stack_va;
    t->stack_size = TASK_TOTAL_PAGES * PAGE_SIZE;
    t->name = name;
    t->mm = process->mm;
    t->process = process;
    /* Boot services and cloned tasks assign their parent explicitly. */
    t->parent_id = SCHED_NO_PARENT;

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
        struct process *process;
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
                /*
                 * A task can become DEAD while it is still queued, for
                 * example when a blocked or READY task is killed remotely.
                 * Remove it before clearing and reusing the task slot so no
                 * per-CPU run queue retains a stale pointer to the victim.
                 */
                (void)sched_rq_remove_locked(t);
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
        process = victim->process;
        victim->process = (struct process *)0;
        victim->mm = (struct mm_context *)0;
        spin_unlock_irqrestore(&sched_lock, flags);

        ipc_detach_task(victim);
        mutex_detach_task(victim);
        semaphore_detach_task(victim);
        condvar_detach_task(victim);

        if (stack_base != (void *)0) {
            page_free_contiguous((void *)va_to_pa(stack_base),
                                 TASK_TOTAL_PAGES);
        }

        if (process != (struct process *)0) {
            process_destroy(process);
        }

        flags = spin_lock_irqsave(&sched_lock);
        sched_clear_task(victim);
        spin_unlock_irqrestore(&sched_lock, flags);
    }
}

void sched_new_task_kickoff(void)
{
    spin_unlock(&sched_lock);
}

void sched_request_reschedule(unsigned int cpu_id)
{
    unsigned long flags;

    if (cpu_id >= SCHED_MAX_CPUS) {
        return;
    }

    flags = spin_lock_irqsave(&sched_lock);
    run_queues[cpu_id].need_resched = 1U;
    spin_unlock_irqrestore(&sched_lock, flags);
}

void sched_preempt_disable(void)
{
    struct task *task = sched_current();

    if (task != (struct task *)0) {
        task->preempt_count++;
    }
}

void sched_preempt_enable(void)
{
    struct task *task = sched_current();

    if (task != (struct task *)0 && task->preempt_count != 0U) {
        task->preempt_count--;
    }
}

int sched_preemptible(void)
{
    struct task *task = sched_current();
    unsigned int cpu = arch_get_cpu_id();

    return task != (struct task *)0 && task->preempt_count == 0U &&
           cpu < SCHED_MAX_CPUS && run_queues[cpu].irq_depth == 0U;
}

void sched_irq_enter(void)
{
    unsigned int cpu = arch_get_cpu_id();

    if (cpu < SCHED_MAX_CPUS) {
        run_queues[cpu].irq_depth++;
    }
}

void sched_irq_exit(void)
{
    struct task *task = sched_current();
    unsigned int cpu = arch_get_cpu_id();
    unsigned int depth;

    if (cpu >= SCHED_MAX_CPUS) {
        return;
    }

    if (run_queues[cpu].irq_depth == 0U) {
        return;
    }
    run_queues[cpu].irq_depth--;
    depth = run_queues[cpu].irq_depth;
    if (depth == 0U && run_queues[cpu].need_resched != 0U &&
        task != (struct task *)0 && task->preempt_count == 0U) {
        schedule();
    }
}

void sched_tick(void)
{
    unsigned long flags = spin_lock_irqsave(&sched_lock);
    unsigned int target_mask = 0U;
    unsigned int current_cpu = arch_get_cpu_id();

    /* CPU 0 owns the global sleep clock so ticks are not decremented once
     * per core. Every CPU still marks its own run queue for preemption. */
    if (current_cpu == 0U) {
        for (unsigned long i = 0; i < MAX_TASKS; i++) {
            struct task *t = &tasks[i];
            if (t->name && t->state == TASK_STATE_BLOCKED &&
                t->sleep_ticks > 0) {
                t->sleep_ticks--;
                if (t->sleep_ticks == 0) {
                    sched_make_ready_locked(t);
                    if (t->rq_cpu < SCHED_MAX_CPUS &&
                        t->rq_cpu != current_cpu) {
                        target_mask |= 1U << t->rq_cpu;
                    }
                }
            }
        }
    }

    if (current_cpu < SCHED_MAX_CPUS) {
        run_queues[current_cpu].need_resched = 1U;
    }

    spin_unlock_irqrestore(&sched_lock, flags);

    if (target_mask != 0U) {
        gic_send_ipi(target_mask, 0);
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

    if (cpu_id >= SCHED_MAX_CPUS) {
        spin_unlock_irqrestore(&sched_lock, flags);
        return;
    }

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

    run_queues[cpu_id].need_resched = 0U;

    /* A runnable outgoing task returns to its local queue for round-robin. */
    if (prev->id >= SCHED_MAX_CPUS) {
        if (prev->state == TASK_STATE_RUNNING) {
            prev->state = TASK_STATE_READY;
            sched_rq_enqueue_locked(prev, cpu_id);
        }
        prev->current_cpu = TASK_NO_CPU;
    }

    next = sched_pick_next_locked(cpu_id);
    if (next == (struct task *)0) {
        next = &tasks[cpu_id];
    }

    if (next == prev) {
        prev->state = TASK_STATE_RUNNING;
        prev->current_cpu = cpu_id;
        spin_unlock_irqrestore(&sched_lock, flags);
        return;
    }

    next->state = TASK_STATE_RUNNING;
    next->current_cpu = cpu_id;
    
    /* 
    KER_LOGF("[sched] CPU %u: %s -> %s\n", arch_get_cpu_id(), prev->name, next->name);
    */

    arch_set_current_task(next);
    switch_count[cpu_id]++;

    mmu_context_switch(next->mm);

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
    struct task *parent = (struct task *)0;
    
    KER_LOGF("[sched] task %s exiting\n", curr->name);

    unsigned long flags = spin_lock_irqsave(&sched_lock);
    /* Reparent live children before publishing this task's final state. */
    for (unsigned long i = 0; i < MAX_TASKS; i++) {
        struct task *child = &tasks[i];

        if (child->name != (const char *)0 && child->parent_id == curr->id &&
            child != curr) {
            child->parent_id = init_task_id;
        }
    }

    for (unsigned long i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].name != (const char *)0 &&
            tasks[i].id == curr->parent_id) {
            parent = &tasks[i];
            break;
        }
    }

    /* Parentless tasks and init itself cannot be waited on, so reap them. */
    if (parent == (struct task *)0 || curr->id == init_task_id) {
        curr->state = TASK_STATE_DEAD;
    } else {
        curr->state = TASK_STATE_ZOMBIE;
    }

    /* Wake up parent if it's waiting */
    if (parent != (struct task *)0 && parent->state == TASK_STATE_BLOCKED) {
            sched_make_ready_locked(parent);
            
            /* Send IPI to other cores */
            unsigned int current_cpu = arch_get_cpu_id();
            unsigned int target_mask = 0;
            for (unsigned int j = 0; j < 4; j++) {
                if (j != current_cpu) {
                    target_mask |= (1 << (unsigned char)j);
                }
            }
            gic_send_ipi(target_mask, 0);
    }
    
    spin_unlock_irqrestore(&sched_lock, flags);

    schedule();

    while (1) {
        cpu_wfe();
    }
}

int sched_register_init_task(struct task *task)
{
    unsigned long flags;

    if (task == (struct task *)0 || task->id < 4UL) {
        return 0;
    }

    flags = spin_lock_irqsave(&sched_lock);
    if (init_task_id != SCHED_NO_PARENT) {
        spin_unlock_irqrestore(&sched_lock, flags);
        return 0;
    }
    init_task_id = task->id;
    task->parent_id = SCHED_NO_PARENT;
    spin_unlock_irqrestore(&sched_lock, flags);
    return 1;
}

int sched_adopt_task(struct task *task)
{
    unsigned long flags;

    if (task == (struct task *)0 || task->id < 4UL) {
        return 0;
    }

    flags = spin_lock_irqsave(&sched_lock);
    if (init_task_id == SCHED_NO_PARENT || task->id == init_task_id) {
        spin_unlock_irqrestore(&sched_lock, flags);
        return 0;
    }
    task->parent_id = init_task_id;
    spin_unlock_irqrestore(&sched_lock, flags);
    return 1;
}

#ifdef CONFIG_SMP_REGRESSION_TESTS
int sched_test_set_parent(struct task *task, unsigned long parent_id)
{
    unsigned long flags;

    if (task == (struct task *)0 || task->id < 4UL) {
        return 0;
    }

    flags = spin_lock_irqsave(&sched_lock);
    task->parent_id = parent_id;
    spin_unlock_irqrestore(&sched_lock, flags);
    return 1;
}

int sched_test_cpu_snapshot(unsigned int cpu_id,
                            unsigned int *nr_running,
                            unsigned long *switches)
{
    unsigned long flags;

    if (cpu_id >= SCHED_MAX_CPUS) {
        return 0;
    }

    flags = spin_lock_irqsave(&sched_lock);
    if (nr_running != (unsigned int *)0) {
        *nr_running = run_queues[cpu_id].nr_running;
    }
    if (switches != (unsigned long *)0) {
        *switches = switch_count[cpu_id];
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    return 1;
}
#endif

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
        (void)sched_rq_remove_locked(task);
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
        sched_make_ready_locked(task);
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
    gic_send_ipi(target_mask, 0);
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
        sched_make_ready_locked(task);
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
        gic_send_ipi(target_mask, 0U);
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
    if (ticks == 0UL) {
        sched_make_ready_locked(task);
    } else {
        (void)sched_rq_remove_locked(task);
        task->state = TASK_STATE_BLOCKED;
    }
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
        if (task->process != (struct process *)0) {
            KER_LOGF(" brk=%lx", task->process->brk);
        }
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
        gic_send_ipi(1U << target_cpu, 0U);
    }

    return killed;
}

int sched_kill_task_sync(unsigned long task_id)
{
    unsigned long state;
    unsigned int current_cpu;
    unsigned int kill_pending;

    if (!sched_kill_task(task_id)) {
        return 0;
    }

    /*
     * Wait until the owning CPU has consumed kill_pending and detached the
     * task from its RUNNING CPU. Disappearance is also a valid acknowledgement
     * because another CPU may reap the task before this caller observes DEAD.
     */
    for (;;) {
        if (!sched_task_snapshot(task_id, &state, &current_cpu,
                                 &kill_pending)) {
            return 1;
        }

        if (state == TASK_STATE_DEAD ||
            state == TASK_STATE_ZOMBIE ||
            state == TASK_STATE_REAPING ||
            current_cpu == TASK_NO_CPU) {
            return 1;
        }

        schedule();
    }
}

int sched_task_snapshot(unsigned long task_id,
                        unsigned long *state,
                        unsigned int *current_cpu,
                        unsigned int *kill_pending)
{
    unsigned long flags;
    int found = 0;

    flags = spin_lock_irqsave(&sched_lock);
    for (unsigned long index = 0UL; index < MAX_TASKS; index++) {
        struct task *task = &tasks[index];

        if (task->name == (const char *)0 || task->id != task_id) {
            continue;
        }

        if (state != (unsigned long *)0) {
            *state = task->state;
        }
        if (current_cpu != (unsigned int *)0) {
            *current_cpu = task->current_cpu;
        }
        if (kill_pending != (unsigned int *)0) {
            *kill_pending = task->kill_pending;
        }
        found = 1;
        break;
    }
    spin_unlock_irqrestore(&sched_lock, flags);

    return found;
}
