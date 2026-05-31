#ifndef KERNEL_SCHED_H
#define KERNEL_SCHED_H

struct mm_context; /* defined in kernel/mmu.h */
struct process;

typedef void (*task_fn_t)(void);

/*
 * Callee-saved register context for switch_context().
 * Only x19-x30 and SP are saved here — the full GPR/SIMD frame is
 * handled by save_context/restore_context in the exception vectors.
 */
struct task_context {
    unsigned long x19;
    unsigned long x20;
    unsigned long x21;
    unsigned long x22;
    unsigned long x23;
    unsigned long x24;
    unsigned long x25;
    unsigned long x26;
    unsigned long x27;
    unsigned long x28;
    unsigned long x29;
    unsigned long x30;
    unsigned long sp;
};

#define TASK_STATE_RUNNING  0UL
#define TASK_STATE_READY    1UL
#define TASK_STATE_DEAD     2UL

#define TASK_GUARD_PAGES    1UL
#define TASK_STACK_PAGES    1UL
#define TASK_TOTAL_PAGES    (TASK_GUARD_PAGES + TASK_STACK_PAGES)
#define MAX_TASKS           16UL

struct task {
    struct task_context context;
    unsigned long id;
    unsigned long state;
    void *stack_base;
    unsigned long stack_size;
    const char *name;
    struct mm_context *mm;
    struct process *process;
    struct task *next;
};

void sched_init(void);
struct task *task_create(task_fn_t entry, const char *name);

#ifdef CONFIG_KERNEL_VIRTUAL
/*
 * Create a task that enters EL0 on first run.
 * process  : process descriptor owning the mm_context and user VA layout.
 * name     : task name for logging.
 */
struct task *task_create_user(struct process *process,
                               const char *name);
#endif

void schedule(void);
void task_exit(void);
struct task *sched_current(void);
void sched_dump_tasks(void);
int sched_kill_task(unsigned long task_id);

extern void switch_context(struct task_context *old_ctx,
                           struct task_context *new_ctx);

#ifdef CONFIG_KERNEL_VIRTUAL
extern void el0_entry_trampoline(void);
#endif

#endif
