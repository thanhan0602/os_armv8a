#ifndef KERNEL_SCHED_H
#define KERNEL_SCHED_H

struct mm_context; /* defined in kernel/mmu.h */

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
    struct task *next;
};

void sched_init(void);
struct task *task_create(task_fn_t entry, const char *name);

#ifdef CONFIG_KERNEL_VIRTUAL
/*
 * User-space virtual address layout for Stage 9 EL0 tasks.
 * Each user task gets a single code page and a single stack page mapped
 * in its own lower-half address space (TTBR0).
 */
#define USER_CODE_VA    0x00010000UL    /* user code page start VA */
#define USER_STACK_TOP  0x00020000UL    /* user stack top (grows down) */

/*
 * Create a task that enters EL0 on first run.
 * entry_va : user-space virtual address of the first instruction.
 * user_sp  : initial SP_EL0 (must be within mm's mapped stack page).
 * mm       : mm_context with the user code and stack pages already mapped.
 * name     : task name for logging.
 */
struct task *task_create_user(unsigned long entry_va,
                               unsigned long user_sp,
                               struct mm_context *mm,
                               const char *name);
#endif

void schedule(void);
void task_exit(void);
struct task *sched_current(void);

extern void switch_context(struct task_context *old_ctx,
                           struct task_context *new_ctx);

#ifdef CONFIG_KERNEL_VIRTUAL
extern void el0_entry_trampoline(void);
#endif

#endif
