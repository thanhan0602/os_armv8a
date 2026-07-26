#include <kernel/log.h>
#include <kernel/sched.h>
#include <kernel/smp_regression.h>

static volatile struct task *wake_before_park_waiter;

static void wake_before_park_consumer(void)
{
    struct task *current = sched_current();

    wake_before_park_waiter = current;
    __asm__ volatile("dmb ish" ::: "memory");

    /* Give another CPU enough time to publish the wake token first. */
    for (volatile unsigned long i = 0UL; i < 2000000UL; i++) {
        __asm__ volatile("yield");
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
    task_exit();
}

void smp_regression_start(void)
{
    wake_before_park_waiter = (struct task *)0;
    KER_INFO("[stress] starting SMP regression suite");

    if (task_create(wake_before_park_consumer, "stress-waiter") ==
            (struct task *)0 ||
        task_create(wake_before_park_producer, "stress-waker") ==
            (struct task *)0) {
        KER_INFO("[stress] wake-before-park FAIL: task creation failed");
    }
}