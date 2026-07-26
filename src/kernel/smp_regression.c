#include <kernel/log.h>
#include <kernel/mmu.h>
#include <kernel/mutex.h>
#include <kernel/semaphore.h>
#include <kernel/condvar.h>
#include <kernel/sched.h>
#include <kernel/smp_regression.h>
#include <kernel/spinlock.h>
#include <arch/arm/cpu.h>
#include <arch/arm/psci.h>

#ifdef CONFIG_SMP_REGRESSION_TESTS

#define SMP_PASS_WAKE_BEFORE_PARK       (1U << 0)
#define SMP_PASS_REMOTE_KILL            (1U << 1)
#define SMP_PASS_MUTEX_OWNER_DETACH     (1U << 2)
#define SMP_PASS_MUTEX_WAITER_DETACH    (1U << 3)
#define SMP_PASS_MUTEX_DESTROY_REJECT   (1U << 4)
#define SMP_PASS_MM_DEFERRED_RELEASE    (1U << 5)
#define SMP_PASS_MUTEX_DESTROY_RACE     (1U << 6)
#define SMP_PASS_MM_MULTI_CPU_DETACH    (1U << 7)
#define SMP_PASS_MUTEX_STALE_HANDLE     (1U << 8)
#define SMP_PASS_MM_SHOOTDOWN_ACK       (1U << 9)
#define SMP_PASS_SEMAPHORE               (1U << 10)
#define SMP_PASS_CONDVAR                 (1U << 11)
#define SMP_PASS_INIT_REAP               (1U << 12)
#define SMP_PASS_ALL                    ((1U << 13) - 1U)
#define MUTEX_DESTROY_RACE_ROUNDS       512U

static volatile unsigned int smp_regression_pass_mask;
static struct spinlock smp_regression_pass_lock = SPINLOCK_INITIALIZER;

static void smp_regression_mark_pass(unsigned int bit)
{
    unsigned long flags;

    flags = spin_lock_irqsave(&smp_regression_pass_lock);
    smp_regression_pass_mask |= bit;
    spin_unlock_irqrestore(&smp_regression_pass_lock, flags);
    __asm__ volatile("sev" ::: "memory");
}

static unsigned int smp_regression_passes(void)
{
    unsigned long flags;
    unsigned int mask;

    flags = spin_lock_irqsave(&smp_regression_pass_lock);
    mask = smp_regression_pass_mask;
    spin_unlock_irqrestore(&smp_regression_pass_lock, flags);
    return mask;
}

static volatile struct task *wake_before_park_waiter;
static volatile unsigned int wake_before_park_done;
static volatile unsigned long remote_kill_target_id;
static volatile unsigned int remote_kill_target_ready;
static volatile int mutex_detach_id;
static volatile unsigned long mutex_owner_id;
static volatile unsigned long mutex_waiter_id;
static volatile unsigned int mutex_owner_ready;
static volatile unsigned int mutex_waiter_started;
static volatile unsigned int mutex_waiter_acquired;
static volatile int mutex_waiter_kill_id;
static volatile unsigned long killed_waiter_id;
static volatile unsigned int waiter_kill_owner_ready;
static volatile unsigned int killed_waiter_started;
static volatile unsigned int waiter_kill_release_owner;
static volatile unsigned int waiter_kill_owner_done;
static volatile int mutex_destroy_race_id;
static volatile unsigned int mutex_destroy_race_held;
static volatile unsigned int mutex_destroy_race_probe_done;
static volatile unsigned int mutex_destroy_race_finished;
static volatile unsigned int mutex_destroy_race_failed;
static volatile struct mm_context *mm_lifetime_context;
static volatile unsigned int mm_lifetime_active;
static volatile unsigned int mm_lifetime_detach;
static volatile unsigned long mm_lifetime_release_baseline;
static volatile unsigned int mm_lifetime_second_active;
static volatile unsigned int mm_lifetime_first_detached;
static volatile unsigned int mm_lifetime_second_detach;
static volatile int sync_sem_handle;
static volatile int sync_cond_handle;
static volatile int sync_cond_mutex_handle;
static volatile unsigned int sync_sem_waiter_started;
static volatile unsigned int sync_sem_waiter_done;
static volatile unsigned int sync_cond_waiter_started;
static volatile unsigned int sync_cond_waiter_done;
static volatile unsigned int sync_cond_predicate;
static volatile unsigned long init_reap_child_id;
static volatile unsigned int init_reap_child_exit;
static volatile unsigned long init_reap_parent_id;
static volatile unsigned int init_reap_parent_done;

static void sync_regression_controller(void);

static void init_reap_child(void)
{
    while (init_reap_child_exit == 0U) {
        schedule();
    }
    task_exit();
}

static void init_reap_parent(void)
{
    struct task *child;

    init_reap_parent_id = sched_current()->id;
    child = task_create(init_reap_child, "stress-orphan-child");
    if (child == (struct task *)0 ||
        !sched_test_set_parent(child, init_reap_parent_id)) {
        KER_INFO("[stress] init-reap FAIL: child creation failed");
        task_exit();
    }

    init_reap_child_id = child->id;
    init_reap_parent_done = 1U;
    __asm__ volatile("dmb ish; sev" ::: "memory");
    task_exit();
}

static void mm_lifetime_holder(void)
{
    struct mm_context *mm = mmu_context_create();
    unsigned long irq_flags;

    if (mm == (struct mm_context *)0) {
        KER_INFO("[stress] mm-deferred-release FAIL: context creation failed");
        task_exit();
    }

    mm_lifetime_release_baseline = mmu_context_test_release_count();
    mm_lifetime_context = mm;

    /*
     * Keep this task on its current CPU until the controller has observed
     * the DYING context. Otherwise a timer interrupt may enter schedule(),
     * switch to another task, and legitimately detach the context before the
     * controller takes its post-put snapshot.
     */
    irq_flags = arch_local_irq_save();
    mmu_context_switch(mm);
    __asm__ volatile("dmb ish" ::: "memory");
    mm_lifetime_active = 1U;
    __asm__ volatile("dmb ish; sev" ::: "memory");

    while (mm_lifetime_detach == 0U) {
        /*
         * This deterministic holder masks timer IRQs so schedule() cannot
         * detach the test context before the controller observes it. Poll
         * the shootdown handler explicitly so a synchronous request can
         * still complete while ordinary IRQ delivery remains masked.
         */
        mmu_handle_shootdown_ipi();
        __asm__ volatile("dmb ish; yield" ::: "memory");
    }

    mmu_context_switch((struct mm_context *)0);
    mm_lifetime_first_detached = 1U;
    __asm__ volatile("dmb ish; sev" ::: "memory");
    arch_local_irq_restore(irq_flags);
    task_exit();
}

static void mm_lifetime_run_controller(void)
{
    struct mm_context *mm;
    unsigned int refs = 1U;
    unsigned int dying = 0U;
    unsigned int active_mask = 0U;
    unsigned long baseline;
    unsigned long attempts;

    for (attempts = 0UL; attempts < 100000UL; attempts++) {
        __asm__ volatile("dmb ish" ::: "memory");
        if (mm_lifetime_active != 0U &&
            mm_lifetime_second_active != 0U) {
            break;
        }
        schedule();
    }

    mm = (struct mm_context *)mm_lifetime_context;
    baseline = mm_lifetime_release_baseline;
    if (mm_lifetime_active == 0U || mm_lifetime_second_active == 0U ||
        mm == (struct mm_context *)0 ||
        !mmu_context_test_snapshot(mm, &refs, &dying, &active_mask) ||
        refs != 1U || dying != 0U || active_mask == 0U ||
        (active_mask & (active_mask - 1U)) == 0U) {
        KER_INFO("[stress] mm-multi-cpu-detach FAIL: context not active on two CPUs");
        return;
    }

    /*
     * Both holder CPUs currently have this context installed. A synchronous
     * shootdown must invalidate the ASID locally and remotely, wait for every
     * SGI acknowledgement, and leave both active-context bits intact.
     */
    if (!mmu_context_shootdown(mm) ||
        !mmu_context_test_snapshot(mm, &refs, &dying, &active_mask) ||
        refs != 1U || dying != 0U || active_mask == 0U ||
        (active_mask & (active_mask - 1U)) == 0U) {
        KER_INFO("[stress] mm-shootdown-ack FAIL: acknowledgement incomplete");
        return;
    }

    KER_INFO("[stress] mm-shootdown-ack PASS");
    smp_regression_mark_pass(SMP_PASS_MM_SHOOTDOWN_ACK);

    mmu_context_put(mm);
    if (!mmu_context_test_snapshot(mm, &refs, &dying, &active_mask) ||
        refs != 0U || dying == 0U || active_mask == 0U ||
        mmu_context_test_release_count() != baseline) {
        KER_INFO("[stress] mm-deferred-release FAIL: released before CPU detach");
        return;
    }

    mm_lifetime_detach = 1U;
    __asm__ volatile("dmb ish; sev" ::: "memory");
    for (attempts = 0UL; attempts < 100000UL; attempts++) {
        if (mm_lifetime_first_detached != 0U) {
            break;
        }
        schedule();
    }

    if (mm_lifetime_first_detached == 0U ||
        !mmu_context_test_snapshot(mm, &refs, &dying, &active_mask) ||
        refs != 0U || dying == 0U || active_mask == 0U ||
        mmu_context_test_release_count() != baseline) {
        KER_INFO("[stress] mm-multi-cpu-detach FAIL: released after first detach");
        return;
    }

    mm_lifetime_second_detach = 1U;
    __asm__ volatile("dmb ish; sev" ::: "memory");
    for (attempts = 0UL; attempts < 100000UL; attempts++) {
        if (mmu_context_test_release_count() == baseline + 1UL) {
            KER_INFO("[stress] mm-deferred-release PASS");
            smp_regression_mark_pass(SMP_PASS_MM_DEFERRED_RELEASE);
            KER_INFO("[stress] mm-multi-cpu-detach PASS");
            smp_regression_mark_pass(SMP_PASS_MM_MULTI_CPU_DETACH);

            while (smp_regression_passes() != SMP_PASS_ALL) {
                schedule();
            }

            KER_INFO("[stress] ALL PASS");
            psci_system_off();
            return;
        }
        schedule();
    }

    KER_INFO("[stress] mm-deferred-release FAIL: final release missing");
}

static void wake_before_park_consumer(void)
{
    struct task *current = sched_current();

    wake_before_park_waiter = current;
    __asm__ volatile("dmb ish" ::: "memory");

    /*
     * Wait for the producer to confirm sched_unpark_task() completed. This
     * makes the intended wake-before-park ordering deterministic instead of
     * depending on an arbitrary delay that can expire under SMP contention.
     */
    while (wake_before_park_done == 0U) {
        __asm__ volatile("dmb ish" ::: "memory");
        schedule();
    }

    if (sched_park_task(current) == 0) {
        KER_INFO("[stress] wake-before-park PASS");
        smp_regression_mark_pass(SMP_PASS_WAKE_BEFORE_PARK);
    } else {
        KER_INFO("[stress] wake-before-park FAIL: pending wake was lost");
        schedule();
    }

    task_exit();
}

static void wake_before_park_producer(void)
{
    struct task *waiter;

    do {
        __asm__ volatile("dmb ish" ::: "memory");
        waiter = (struct task *)wake_before_park_waiter;
        if (waiter == (struct task *)0) {
            schedule();
        }
    } while (waiter == (struct task *)0);

    sched_unpark_task(waiter);
    __asm__ volatile("dmb ish" ::: "memory");
    wake_before_park_done = 1U;
    __asm__ volatile("dmb ish; sev" ::: "memory");
    task_exit();
}

static void remote_kill_target(void)
{
    remote_kill_target_id = sched_current()->id;
    __asm__ volatile("dmb ish" ::: "memory");
    remote_kill_target_ready = 1U;
    __asm__ volatile("dmb ish; sev" ::: "memory");

    /* Stay runnable so the killer must use the remote RUNNING-task path. */
    for (;;) {
        __asm__ volatile("yield");
    }
}

static void remote_kill_controller(void)
{
    unsigned long target_id;
    unsigned long state = TASK_STATE_READY;
    unsigned int target_cpu = TASK_NO_CPU;
    unsigned int kill_pending = 0U;
    unsigned int local_cpu;
    unsigned long attempts;

    for (attempts = 0UL; attempts < 100000UL; attempts++) {
        __asm__ volatile("dmb ish" ::: "memory");
        if (remote_kill_target_ready != 0U) {
            break;
        }
        schedule();
    }

    if (remote_kill_target_ready == 0U) {
        KER_INFO("[stress] remote-kill FAIL: target did not start");
        task_exit();
    }

    target_id = remote_kill_target_id;
    local_cpu = arch_get_cpu_id();

    for (attempts = 0UL; attempts < 100000UL; attempts++) {
        if (sched_task_snapshot(target_id, &state, &target_cpu,
                                &kill_pending) &&
            state == TASK_STATE_RUNNING && target_cpu != TASK_NO_CPU &&
            target_cpu != local_cpu) {
            break;
        }
        schedule();
        local_cpu = arch_get_cpu_id();
    }

    if (state != TASK_STATE_RUNNING || target_cpu == TASK_NO_CPU ||
        target_cpu == local_cpu) {
        KER_INFO("[stress] remote-kill FAIL: no remote RUNNING observation");
        task_exit();
    }

    if (!sched_kill_task_sync(target_id)) {
        KER_INFO("[stress] remote-stop-ack FAIL: synchronous kill rejected");
        task_exit();
    }

    if (sched_task_snapshot(target_id, &state, &target_cpu,
                            &kill_pending) &&
        state != TASK_STATE_DEAD &&
        state != TASK_STATE_ZOMBIE &&
        state != TASK_STATE_REAPING &&
        target_cpu != TASK_NO_CPU) {
        KER_INFO("[stress] remote-stop-ack FAIL: target still running");
        task_exit();
    }

    KER_INFO("[stress] remote-stop-ack PASS");
    KER_INFO("[stress] remote-kill PASS");
    smp_regression_mark_pass(SMP_PASS_REMOTE_KILL);
    mm_lifetime_run_controller();
    task_exit();
}

static void mutex_detach_owner(void)
{
    if (!mutex_pool_lock(mutex_detach_id)) {
        KER_INFO("[stress] mutex-owner-detach FAIL: owner lock failed");
        task_exit();
    }

    mutex_owner_id = sched_current()->id;
    __asm__ volatile("dmb ish" ::: "memory");
    mutex_owner_ready = 1U;
    __asm__ volatile("dmb ish; sev" ::: "memory");

    /* The controller kills this task while it owns the mutex. */
    for (;;) {
        __asm__ volatile("yield");
    }
}

static void mutex_detach_waiter(void)
{
    while (mutex_owner_ready == 0U) {
        __asm__ volatile("dmb ish" ::: "memory");
        schedule();
    }

    mutex_waiter_id = sched_current()->id;
    mutex_waiter_started = 1U;
    __asm__ volatile("dmb ish; sev" ::: "memory");

    if (!mutex_pool_lock(mutex_detach_id)) {
        KER_INFO("[stress] mutex-owner-detach FAIL: waiter lock failed");
        task_exit();
    }

    mutex_waiter_acquired = 1U;
    __asm__ volatile("dmb ish; sev" ::: "memory");
    (void)mutex_pool_unlock(mutex_detach_id);
    task_exit();
}

static void mutex_detach_controller(void)
{
    unsigned long state = TASK_STATE_READY;
    unsigned int cpu = TASK_NO_CPU;
    unsigned int pending = 0U;
    unsigned long attempts;

    for (attempts = 0UL; attempts < 100000UL; attempts++) {
        __asm__ volatile("dmb ish" ::: "memory");
        if (mutex_owner_ready != 0U && mutex_waiter_started != 0U &&
            sched_task_snapshot(mutex_waiter_id, &state, &cpu, &pending) &&
            state == TASK_STATE_BLOCKED) {
            break;
        }
        schedule();
    }

    if (state != TASK_STATE_BLOCKED) {
        KER_INFO("[stress] mutex-owner-detach FAIL: waiter did not block");
        task_exit();
    }

    if (!sched_kill_task(mutex_owner_id)) {
        KER_INFO("[stress] mutex-owner-detach FAIL: owner kill rejected");
        task_exit();
    }

    for (attempts = 0UL; attempts < 100000UL; attempts++) {
        __asm__ volatile("dmb ish" ::: "memory");
        if (mutex_waiter_acquired != 0U) {
            break;
        }
        schedule();
    }

    if (mutex_waiter_acquired == 0U) {
        KER_INFO("[stress] mutex-owner-detach FAIL: ownership not handed off");
        task_exit();
    }

    for (attempts = 0UL; attempts < 100000UL; attempts++) {
        if (mutex_pool_free(mutex_detach_id)) {
            KER_INFO("[stress] mutex-owner-detach PASS");
            smp_regression_mark_pass(SMP_PASS_MUTEX_OWNER_DETACH);
            task_exit();
        }
        schedule();
    }

    KER_INFO("[stress] mutex-owner-detach FAIL: pool slot stayed busy");
    task_exit();
}

static void mutex_waiter_kill_owner(void)
{
    unsigned int round;
    unsigned long irq_flags;
    struct mm_context *mm;

    if (!mutex_pool_lock(mutex_waiter_kill_id)) {
        KER_INFO("[stress] mutex-waiter-detach FAIL: owner lock failed");
        task_exit();
    }

    waiter_kill_owner_ready = 1U;
    __asm__ volatile("dmb ish; sev" ::: "memory");
    while (waiter_kill_release_owner == 0U) {
        __asm__ volatile("dmb ish" ::: "memory");
        schedule();
    }

    (void)mutex_pool_unlock(mutex_waiter_kill_id);
    waiter_kill_owner_done = 1U;
    __asm__ volatile("dmb ish; sev" ::: "memory");

    while (mutex_destroy_race_id < 0) {
        __asm__ volatile("dmb ish" ::: "memory");
        schedule();
    }

    for (round = 0U; round < MUTEX_DESTROY_RACE_ROUNDS; round++) {
        if (!mutex_pool_lock(mutex_destroy_race_id)) {
            mutex_destroy_race_failed = 1U;
            break;
        }

        mutex_destroy_race_held = round + 1U;
        __asm__ volatile("dmb ish; sev" ::: "memory");
        while (mutex_destroy_race_probe_done != round + 1U) {
            __asm__ volatile("dmb ish" ::: "memory");
            schedule();
        }

        if (!mutex_pool_unlock(mutex_destroy_race_id)) {
            mutex_destroy_race_failed = 1U;
            break;
        }

        if (mutex_pool_trylock(mutex_destroy_race_id)) {
            if (!mutex_pool_unlock(mutex_destroy_race_id)) {
                mutex_destroy_race_failed = 1U;
                break;
            }
        }
    }

    mutex_destroy_race_finished = 1U;
    __asm__ volatile("dmb ish; sev" ::: "memory");

    while (mm_lifetime_active == 0U) {
        __asm__ volatile("dmb ish" ::: "memory");
        schedule();
    }

    mm = (struct mm_context *)mm_lifetime_context;
    if (mm == (struct mm_context *)0) {
        KER_INFO("[stress] mm-multi-cpu-detach FAIL: missing shared context");
        task_exit();
    }

    irq_flags = arch_local_irq_save();
    mmu_context_switch(mm);
    mm_lifetime_second_active = 1U;
    __asm__ volatile("dmb ish; sev" ::: "memory");
    while (mm_lifetime_second_detach == 0U) {
        /* See mm_lifetime_holder(): service pending shootdowns while the
         * deterministic observation window keeps timer IRQs masked. */
        mmu_handle_shootdown_ipi();
        __asm__ volatile("dmb ish; yield" ::: "memory");
    }
    mmu_context_switch((struct mm_context *)0);
    arch_local_irq_restore(irq_flags);
    task_exit();
}

static void mutex_waiter_kill_target(void)
{
    while (waiter_kill_owner_ready == 0U) {
        __asm__ volatile("dmb ish" ::: "memory");
        schedule();
    }

    killed_waiter_id = sched_current()->id;
    killed_waiter_started = 1U;
    __asm__ volatile("dmb ish; sev" ::: "memory");

    (void)mutex_pool_lock(mutex_waiter_kill_id);
    KER_INFO("[stress] mutex-waiter-detach FAIL: killed waiter resumed");
    task_exit();
}

static void mutex_waiter_kill_controller(void)
{
    unsigned long state = TASK_STATE_READY;
    unsigned int cpu = TASK_NO_CPU;
    unsigned int pending = 0U;
    unsigned long attempts;

    for (attempts = 0UL; attempts < 100000UL; attempts++) {
        __asm__ volatile("dmb ish" ::: "memory");
        if (killed_waiter_started != 0U &&
            sched_task_snapshot(killed_waiter_id, &state, &cpu, &pending) &&
            state == TASK_STATE_BLOCKED) {
            break;
        }
        schedule();
    }

    if (state != TASK_STATE_BLOCKED ||
        !sched_kill_task(killed_waiter_id)) {
        KER_INFO("[stress] mutex-waiter-detach FAIL: waiter kill failed");
        task_exit();
    }

    for (attempts = 0UL; attempts < 100000UL; attempts++) {
        if (!sched_task_snapshot(killed_waiter_id, &state, &cpu, &pending)) {
            break;
        }
        schedule();
    }

    if (sched_task_snapshot(killed_waiter_id, &state, &cpu, &pending)) {
        KER_INFO("[stress] mutex-waiter-detach FAIL: waiter was not reaped");
        task_exit();
    }

    /*
     * The owner still holds the mutex here. Destroy must fail while the slot
     * is locked and has an owner, even though the killed waiter and its
     * abandoned operation pin have already been detached.
     */
    if (mutex_pool_free(mutex_waiter_kill_id)) {
        KER_INFO("[stress] mutex-concurrent-destroy FAIL: destroyed locked mutex");
        task_exit();
    }
    KER_INFO("[stress] mutex-concurrent-destroy PASS");
    smp_regression_mark_pass(SMP_PASS_MUTEX_DESTROY_REJECT);

    waiter_kill_release_owner = 1U;
    __asm__ volatile("dmb ish; sev" ::: "memory");
    for (attempts = 0UL; attempts < 100000UL; attempts++) {
        __asm__ volatile("dmb ish" ::: "memory");
        if (waiter_kill_owner_done != 0U) {
            break;
        }
        schedule();
    }

    if (waiter_kill_owner_done != 0U &&
        mutex_pool_free(mutex_waiter_kill_id)) {
        KER_INFO("[stress] mutex-waiter-detach PASS");
        smp_regression_mark_pass(SMP_PASS_MUTEX_WAITER_DETACH);
    } else {
        KER_INFO("[stress] mutex-waiter-detach FAIL: slot stayed busy");
        task_exit();
    }

    mutex_destroy_race_id = mutex_pool_alloc();
    if (mutex_destroy_race_id < 0) {
        KER_INFO("[stress] mutex-destroy-race FAIL: pool allocation failed");
        task_exit();
    }
    __asm__ volatile("dmb ish; sev" ::: "memory");

    for (unsigned int round = 0U; round < MUTEX_DESTROY_RACE_ROUNDS; round++) {
        while (mutex_destroy_race_held != round + 1U) {
            __asm__ volatile("dmb ish" ::: "memory");
            if (mutex_destroy_race_failed != 0U) {
                break;
            }
            schedule();
        }

        if (mutex_destroy_race_failed != 0U ||
            mutex_pool_free(mutex_destroy_race_id)) {
            mutex_destroy_race_failed = 1U;
            break;
        }

        mutex_destroy_race_probe_done = round + 1U;
        __asm__ volatile("dmb ish; sev" ::: "memory");
    }

    while (mutex_destroy_race_finished == 0U) {
        __asm__ volatile("dmb ish" ::: "memory");
        schedule();
    }

    if (mutex_destroy_race_failed == 0U &&
        mutex_pool_free(mutex_destroy_race_id)) {
        int stale_handle = mutex_destroy_race_id;
        int replacement_handle;

        KER_INFO("[stress] mutex-destroy-race PASS");
        smp_regression_mark_pass(SMP_PASS_MUTEX_DESTROY_RACE);

        replacement_handle = mutex_pool_alloc();
        if (replacement_handle < 0 || replacement_handle == stale_handle ||
            mutex_pool_lock(stale_handle) ||
            mutex_pool_trylock(stale_handle) ||
            mutex_pool_unlock(stale_handle) ||
            mutex_pool_free(stale_handle) ||
            !mutex_pool_trylock(replacement_handle) ||
            !mutex_pool_unlock(replacement_handle) ||
            !mutex_pool_free(replacement_handle)) {
            KER_INFO("[stress] mutex-stale-handle FAIL: generation check failed");
            task_exit();
        }

        KER_INFO("[stress] mutex-stale-handle PASS");
        smp_regression_mark_pass(SMP_PASS_MUTEX_STALE_HANDLE);
        sync_regression_controller();
    } else {
        KER_INFO("[stress] mutex-destroy-race FAIL: race invariant violated");
    }
    task_exit();
}

static void sync_semaphore_waiter(void)
{
    sync_sem_waiter_started = 1U;
    __asm__ volatile("dmb ish; sev" ::: "memory");

    if (!semaphore_pool_wait(sync_sem_handle)) {
        KER_INFO("[stress] semaphore FAIL: wait rejected");
        task_exit();
    }

    sync_sem_waiter_done = 1U;
    __asm__ volatile("dmb ish; sev" ::: "memory");
    task_exit();
}

static void sync_condvar_waiter(void)
{
    if (!mutex_pool_lock(sync_cond_mutex_handle)) {
        KER_INFO("[stress] condvar FAIL: mutex lock rejected");
        task_exit();
    }

    sync_cond_waiter_started = 1U;
    __asm__ volatile("dmb ish; sev" ::: "memory");
    while (sync_cond_predicate == 0U) {
        if (!condvar_pool_wait(sync_cond_handle,
                               sync_cond_mutex_handle)) {
            KER_INFO("[stress] condvar FAIL: wait rejected");
            (void)mutex_pool_unlock(sync_cond_mutex_handle);
            task_exit();
        }
    }

    sync_cond_waiter_done = 1U;
    __asm__ volatile("dmb ish; sev" ::: "memory");
    (void)mutex_pool_unlock(sync_cond_mutex_handle);
    task_exit();
}

static void sync_regression_controller(void)
{
    unsigned long attempts;
    unsigned long child_id;
    unsigned long parent_id;
    struct task *parent;

    if (!sched_register_init_task(sched_current())) {
        KER_INFO("[stress] init-reap FAIL: init setup failed");
        task_exit();
    }

    parent = task_create(init_reap_parent, "stress-orphan-parent");
    if (parent == (struct task *)0 || !sched_adopt_task(parent)) {
        KER_INFO("[stress] init-reap FAIL: parent creation failed");
        task_exit();
    }
    parent_id = parent->id;

    for (attempts = 0UL; attempts < 100000UL; attempts++) {
        if (init_reap_parent_done != 0U) {
            break;
        }
        schedule();
    }

    if (init_reap_parent_done == 0U ||
        sched_wait4((long)parent_id, 0UL) != parent_id) {
        KER_INFO("[stress] init-reap FAIL: parent was not collected");
        task_exit();
    }

    init_reap_child_exit = 1U;
    __asm__ volatile("dmb ish; sev" ::: "memory");
    child_id = sched_wait4((long)init_reap_child_id, 0UL);
    if (init_reap_parent_done == 0U || child_id != init_reap_child_id) {
        KER_INFO("[stress] init-reap FAIL: orphan was not collected");
        task_exit();
    }

    KER_INFO("[stress] init-reap PASS");
    smp_regression_mark_pass(SMP_PASS_INIT_REAP);

    if (task_create(sync_semaphore_waiter, "stress-sem-waiter") ==
            (struct task *)0 ||
        task_create(sync_condvar_waiter, "stress-cond-waiter") ==
            (struct task *)0) {
        KER_INFO("[stress] sync primitives FAIL: task creation failed");
        task_exit();
    }

    for (attempts = 0UL; attempts < 100000UL; attempts++) {
        if (sync_sem_waiter_started != 0U &&
            sync_cond_waiter_started != 0U) {
            break;
        }
        schedule();
    }

    if (sync_sem_waiter_started == 0U ||
        !semaphore_pool_post(sync_sem_handle)) {
        KER_INFO("[stress] semaphore FAIL: waiter did not start");
        task_exit();
    }

    for (attempts = 0UL; attempts < 100000UL; attempts++) {
        if (sync_sem_waiter_done != 0U) break;
        schedule();
    }

    if (sync_sem_waiter_done == 0U ||
        !semaphore_pool_free(sync_sem_handle)) {
        KER_INFO("[stress] semaphore FAIL: wake or destroy failed");
        task_exit();
    }
    KER_INFO("[stress] semaphore PASS");
    smp_regression_mark_pass(SMP_PASS_SEMAPHORE);

    if (!mutex_pool_lock(sync_cond_mutex_handle)) {
        KER_INFO("[stress] condvar FAIL: controller lock rejected");
        task_exit();
    }
    sync_cond_predicate = 1U;
    if (!condvar_pool_signal(sync_cond_handle) ||
        !mutex_pool_unlock(sync_cond_mutex_handle)) {
        KER_INFO("[stress] condvar FAIL: signal rejected");
        task_exit();
    }

    for (attempts = 0UL; attempts < 100000UL; attempts++) {
        if (sync_cond_waiter_done != 0U) break;
        schedule();
    }

    if (sync_cond_waiter_done == 0U ||
        !condvar_pool_free(sync_cond_handle) ||
        !mutex_pool_free(sync_cond_mutex_handle)) {
        KER_INFO("[stress] condvar FAIL: wake or destroy failed");
        task_exit();
    }
    KER_INFO("[stress] condvar PASS");
    smp_regression_mark_pass(SMP_PASS_CONDVAR);
    task_exit();
}

void smp_regression_start(void)
{
    smp_regression_pass_mask = 0U;
    wake_before_park_waiter = (struct task *)0;
    wake_before_park_done = 0U;
    remote_kill_target_id = 0UL;
    remote_kill_target_ready = 0U;
    mutex_owner_id = 0UL;
    mutex_waiter_id = 0UL;
    mutex_owner_ready = 0U;
    mutex_waiter_started = 0U;
    mutex_waiter_acquired = 0U;
    mutex_detach_id = mutex_pool_alloc();
    killed_waiter_id = 0UL;
    waiter_kill_owner_ready = 0U;
    killed_waiter_started = 0U;
    waiter_kill_release_owner = 0U;
    waiter_kill_owner_done = 0U;
    mutex_destroy_race_id = -1;
    mutex_destroy_race_held = 0U;
    mutex_destroy_race_probe_done = 0U;
    mutex_destroy_race_finished = 0U;
    mutex_destroy_race_failed = 0U;
    mutex_waiter_kill_id = mutex_pool_alloc();
    mm_lifetime_context = (struct mm_context *)0;
    mm_lifetime_active = 0U;
    mm_lifetime_detach = 0U;
    mm_lifetime_release_baseline = 0UL;
    mm_lifetime_second_active = 0U;
    mm_lifetime_first_detached = 0U;
    mm_lifetime_second_detach = 0U;
    sync_sem_handle = semaphore_pool_alloc(0UL);
    sync_cond_handle = condvar_pool_alloc();
    sync_cond_mutex_handle = mutex_pool_alloc();
    sync_sem_waiter_started = 0U;
    sync_sem_waiter_done = 0U;
    sync_cond_waiter_started = 0U;
    sync_cond_waiter_done = 0U;
    sync_cond_predicate = 0U;
    init_reap_child_id = 0UL;
    init_reap_child_exit = 0U;
    init_reap_parent_id = 0UL;
    init_reap_parent_done = 0U;
    KER_INFO("[stress] starting SMP regression suite");

    if (sync_sem_handle < 0 || sync_cond_handle < 0 ||
        sync_cond_mutex_handle < 0) {
        KER_INFO("[stress] sync primitives FAIL: pool allocation failed");
    }

    if (task_create(wake_before_park_consumer, "stress-waiter") ==
            (struct task *)0 ||
        task_create(wake_before_park_producer, "stress-waker") ==
            (struct task *)0) {
        KER_INFO("[stress] wake-before-park FAIL: task creation failed");
    }

    if (task_create(remote_kill_target, "stress-kill-target") ==
            (struct task *)0 ||
        task_create(remote_kill_controller, "stress-kill-controller") ==
            (struct task *)0) {
        KER_INFO("[stress] remote-kill FAIL: task creation failed");
    }

    if (mutex_detach_id < 0) {
        KER_INFO("[stress] mutex-owner-detach FAIL: pool allocation failed");
    } else if (task_create(mutex_detach_owner, "stress-mutex-owner") ==
                   (struct task *)0 ||
               task_create(mutex_detach_waiter, "stress-mutex-waiter") ==
                   (struct task *)0 ||
               task_create(mutex_detach_controller,
                           "stress-mutex-controller") == (struct task *)0) {
        KER_INFO("[stress] mutex-owner-detach FAIL: task creation failed");
    }

    if (mutex_waiter_kill_id < 0) {
        KER_INFO("[stress] mutex-waiter-detach FAIL: pool allocation failed");
    } else if (task_create(mutex_waiter_kill_owner,
                           "stress-waiter-owner") == (struct task *)0 ||
               task_create(mutex_waiter_kill_target,
                           "stress-killed-waiter") == (struct task *)0 ||
               task_create(mutex_waiter_kill_controller,
                           "stress-waiter-controller") == (struct task *)0) {
        KER_INFO("[stress] mutex-waiter-detach FAIL: task creation failed");
    }

    /*
     * Reuse the remote-kill controller for the MM assertions. The regression
     * build already has a shell plus ten stress tasks, so creating a separate
     * MM controller would exceed MAX_TASKS before any test task is reaped.
     */
    if (task_create(mm_lifetime_holder, "stress-mm-holder") ==
            (struct task *)0) {
        KER_INFO("[stress] mm-deferred-release FAIL: task creation failed");
    }
}

#else

void smp_regression_start(void)
{
}

#endif /* CONFIG_SMP_REGRESSION_TESTS */