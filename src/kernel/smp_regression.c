#include <kernel/log.h>
#include <kernel/mmu.h>
#include <kernel/mutex.h>
#include <kernel/sched.h>
#include <kernel/smp_regression.h>
#include <arch/arm/cpu.h>

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
static volatile struct mm_context *mm_lifetime_context;
static volatile unsigned int mm_lifetime_active;
static volatile unsigned int mm_lifetime_detach;
static volatile unsigned long mm_lifetime_release_baseline;

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
        __asm__ volatile("dmb ish; yield" ::: "memory");
    }

    mmu_context_switch((struct mm_context *)0);
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
        if (mm_lifetime_active != 0U) {
            break;
        }
        schedule();
    }

    mm = (struct mm_context *)mm_lifetime_context;
    baseline = mm_lifetime_release_baseline;
    if (mm_lifetime_active == 0U || mm == (struct mm_context *)0 ||
        !mmu_context_test_snapshot(mm, &refs, &dying, &active_mask) ||
        refs != 1U || dying != 0U || active_mask == 0U) {
        KER_INFO("[stress] mm-deferred-release FAIL: context was not active");
        return;
    }

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
        if (mmu_context_test_release_count() == baseline + 1UL) {
            KER_INFO("[stress] mm-deferred-release PASS");
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

    if (!sched_kill_task(target_id)) {
        KER_INFO("[stress] remote-kill FAIL: kill request rejected");
        task_exit();
    }

    for (attempts = 0UL; attempts < 100000UL; attempts++) {
        if (!sched_task_snapshot(target_id, &state, &target_cpu,
                                 &kill_pending)) {
            KER_INFO("[stress] remote-kill PASS");
            mm_lifetime_run_controller();
            task_exit();
        }
        schedule();
    }

    KER_INFO("[stress] remote-kill FAIL: target was not reaped");
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
            task_exit();
        }
        schedule();
    }

    KER_INFO("[stress] mutex-owner-detach FAIL: pool slot stayed busy");
    task_exit();
}

static void mutex_waiter_kill_owner(void)
{
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
    } else {
        KER_INFO("[stress] mutex-waiter-detach FAIL: slot stayed busy");
    }
    task_exit();
}

void smp_regression_start(void)
{
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
    mutex_waiter_kill_id = mutex_pool_alloc();
    mm_lifetime_context = (struct mm_context *)0;
    mm_lifetime_active = 0U;
    mm_lifetime_detach = 0U;
    mm_lifetime_release_baseline = 0UL;
    KER_INFO("[stress] starting SMP regression suite");

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