#include <kernel/sched.h>

#include <kernel/log.h>
#include <kernel/page_alloc.h>
#include <kernel/vm.h>

extern void task_entry_trampoline(void);

static struct task tasks[MAX_TASKS];
static unsigned long next_task_id;
static struct task *current;
static unsigned long switch_count;

void sched_init(void)
{
    struct task *idle;

    next_task_id = 0;
    switch_count = 0;

    /*
     * Task 0 is the idle / boot task.  It uses the boot stack and is
     * already running, so its context will be filled by the first
     * switch_context call that switches away from it.
     */
    idle = &tasks[0];
    idle->id = next_task_id++;
    idle->state = TASK_STATE_RUNNING;
    idle->stack_base = (void *)0;
    idle->stack_size = 0;
    idle->name = "idle";
    idle->next = idle;

    current = idle;

    log_info("scheduler initialized");
}

struct task *task_create(task_fn_t entry, const char *name)
{
    struct task *t;
    void *stack_pa;
    unsigned char *stack_va;
    unsigned long sp;

    if (next_task_id >= MAX_TASKS) {
        log_info("task_create: max tasks reached");
        return (struct task *)0;
    }

    /*
     * Allocate guard page + usable stack contiguously.
     * Layout: [guard page (low)] [usable stack (high)]
     * The guard page is allocated but never written to, so a stack
     * overflow will corrupt only the guard rather than another
     * allocation.  (Hardware-enforced guard would require splitting
     * L2 block mappings into L3 entries, deferred to a later stage.)
     */
    stack_pa = page_alloc_contiguous(TASK_TOTAL_PAGES);
    if (stack_pa == (void *)0) {
        log_info("task_create: stack alloc failed");
        return (struct task *)0;
    }

    stack_va = (unsigned char *)pa_to_va(stack_pa);
    sp = (unsigned long)(stack_va + TASK_TOTAL_PAGES * PAGE_SIZE);

    /* AArch64 requires 16-byte aligned SP */
    sp &= ~0xFUL;

    t = &tasks[next_task_id];
    t->id = next_task_id;
    t->state = TASK_STATE_READY;
    t->stack_base = stack_va;
    t->stack_size = TASK_TOTAL_PAGES * PAGE_SIZE;
    t->name = name;

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
    t->next = current->next;
    current->next = t;

    next_task_id++;

    log_write("[sched] created task ");
    log_write(name);
    log_write(" id=");
    log_write_u64(t->id);
    log_write(" stack=");
    log_write_hex((unsigned long)stack_va);
    log_putc('\n');

    return t;
}

/*
 * Reclaim resources from dead tasks.  Called at the start of schedule()
 * so we are running on the current (live) task's stack, never on a dead
 * task's stack that is about to be freed.
 */
static void sched_reap_dead(void)
{
    struct task *prev;
    struct task *t;

    prev = current;
    t = current->next;

    while (t != current) {
        if (t->state == TASK_STATE_DEAD) {
            prev->next = t->next;

            if (t->stack_base != (void *)0) {
                log_write("[sched] reap task ");
                log_write(t->name);
                log_write(" id=");
                log_write_u64(t->id);
                log_putc('\n');
                page_free_contiguous(
                    (void *)va_to_pa(t->stack_base),
                    TASK_TOTAL_PAGES);
                t->stack_base = (void *)0;
            }

            t = prev->next;
        } else {
            prev = t;
            t = t->next;
        }
    }
}

void schedule(void)
{
    struct task *prev;
    struct task *next;

    sched_reap_dead();

    prev = current;
    next = prev->next;

    /* Round-robin: find next ready task, skip dead ones */
    while (next != prev) {
        if (next->state == TASK_STATE_READY) {
            break;
        }
        next = next->next;
    }

    if (next == prev) {
        return;
    }

    if (prev->state == TASK_STATE_RUNNING) {
        prev->state = TASK_STATE_READY;
    }
    next->state = TASK_STATE_RUNNING;
    current = next;

    switch_count++;
    if (switch_count <= 8UL) {
        log_write("[sched] ");
        log_write(prev->name);
        log_write(" -> ");
        log_write(next->name);
        log_write(" (#");
        log_write_u64(switch_count);
        log_write(")\n");
    }

    switch_context(&prev->context, &next->context);
}

void task_exit(void)
{
    log_write("[sched] task ");
    log_write(current->name);
    log_write(" exited\n");

    current->state = TASK_STATE_DEAD;
    schedule();

    while (1) {
        __asm__ volatile("wfe");
    }
}

struct task *sched_current(void)
{
    return current;
}
