#ifndef KERNEL_SCHED_H
#define KERNEL_SCHED_H

#include <kernel/fs.h>

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
    unsigned long tpidr_el0;   /* EL0 thread pointer (TLS), swapped on context switch */
};

#define TASK_STATE_RUNNING  0UL
#define TASK_STATE_READY    1UL
#define TASK_STATE_BLOCKED  2UL
#define TASK_STATE_DEAD     3UL
#define TASK_STATE_ZOMBIE   4UL
#define TASK_STATE_REAPING  5UL

#define TASK_NO_CPU         0xffffffffU
#define SCHED_MAX_CPUS      4U

#define TASK_GUARD_PAGES    1UL
#define TASK_STACK_PAGES    4UL
#define TASK_TOTAL_PAGES    (TASK_GUARD_PAGES + TASK_STACK_PAGES)
#define MAX_TASKS           32UL
#define MAX_FILES_PER_TASK  16UL

struct task {
    struct task_context context;
    unsigned long id;
    unsigned long state;
    void *stack_base;
    unsigned long stack_size;
    unsigned long sleep_ticks;
    unsigned int kill_pending;
    unsigned int wake_pending;
    unsigned int current_cpu;
    unsigned int rq_cpu;
    unsigned int on_rq;
    unsigned int preempt_count;
    const char *name;
    struct mm_context *mm;
    struct process *process;
    unsigned long parent_id;
    int exit_status;
    struct task *next;
    struct task *rq_next;
    struct task *wait_next;
    struct file *files[MAX_FILES_PER_TASK];
};

void sched_init(void);
struct task *task_create(task_fn_t entry, const char *name);

/*
 * Create a task that enters EL0 on first run.
 * process  : process descriptor owning the mm_context and user VA layout.
 * name     : task name for logging.
 */
struct task *task_create_user(struct process *process,
                               const char *name);

void schedule(void);
void sched_request_reschedule(unsigned int cpu_id);
void sched_preempt_disable(void);
void sched_preempt_enable(void);
int sched_preemptible(void);
void sched_irq_enter(void);
void sched_irq_exit(void);
void sched_tick(void);
void sched_new_task_kickoff(void);
void task_exit(void);
struct task *sched_current(void);
void sched_block_task(struct task *task);
void sched_wake_task(struct task *task);
int sched_park_task(struct task *task);
void sched_unpark_task(struct task *task);
int sched_sleep_current(unsigned long ticks);
void sched_set_current_exit_status(int status);
unsigned long sched_wait4(long pid, unsigned long status_ptr);
int sched_register_init_task(struct task *task);
int sched_adopt_task(struct task *task);
#ifdef CONFIG_SMP_REGRESSION_TESTS
int sched_test_set_parent(struct task *task, unsigned long parent_id);
int sched_test_cpu_snapshot(unsigned int cpu_id,
                            unsigned int *nr_running,
                            unsigned long *switches);
#endif
void sched_dump_tasks(void);
int sched_kill_task(unsigned long task_id);
int sched_kill_task_sync(unsigned long task_id);
int sched_task_snapshot(unsigned long task_id,
                        unsigned long *state,
                        unsigned int *current_cpu,
                        unsigned int *kill_pending);

extern void switch_context(struct task_context *old_ctx,
                           struct task_context *new_ctx);

extern void el0_entry_trampoline(void);
extern void fork_child_exit(void);

#endif
