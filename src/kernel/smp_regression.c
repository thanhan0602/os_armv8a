#include <kernel/log.h>
#include <kernel/sched.h>
#include <kernel/smp_regression.h>
#include <arch/arm/cpu.h>

static volatile struct task *wake_before_park_waiter;
static volatile unsigned int wake_before_park_done;
static volatile unsigned long remote_kill_target_id;
static volatile unsigned int remote_kill_target_ready;

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
            task_exit();
        }
        schedule();
    }

    KER_INFO("[stress] remote-kill FAIL: target was not reaped");
    task_exit();
}

void smp_regression_start(void)
{
    wake_before_park_waiter = (struct task *)0;
    wake_before_park_done = 0U;
    remote_kill_target_id = 0UL;
    remote_kill_target_ready = 0U;
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
}