#include <kernel/spinlock.h>
#include <arch/arm/cpu.h>

static int spin_try_lock(struct spinlock *lock)
{
    unsigned int locked;
    unsigned int status;

    __asm__ volatile(
        "ldaxr %w0, [%2]\n"
        "cbnz %w0, 1f\n"
        "mov %w0, #1\n"
        "stxr %w1, %w0, [%2]\n"
        "cbz %w1, 2f\n"
        "1:\n"
        "mov %w0, #0\n"
        "2:\n"
        : "=&r"(locked), "=&r"(status)
        : "r"(&lock->value)
        : "memory");

    return locked != 0U;
}

void spinlock_init(struct spinlock *lock)
{
    if (lock != (struct spinlock *)0) {
        lock->value = 0U;
    }
}

void cpu_relax(void)
{
    cpu_yield();
}

void cpu_wait(void)
{
    cpu_wfe();
}

void cpu_wake(void)
{
    cpu_sev();
}

void spin_lock(struct spinlock *lock)
{
    if (lock == (struct spinlock *)0) {
        return;
    }

    while (!spin_try_lock(lock)) {
        while (lock->value != 0U) {
            cpu_wait();
        }
        cpu_relax();
    }
}

void spin_unlock(struct spinlock *lock)
{
    if (lock == (struct spinlock *)0) {
        return;
    }

    __asm__ volatile("stlr wzr, [%0]" : : "r"(&lock->value) : "memory");
    cpu_wake();
}

unsigned long spin_lock_irqsave(struct spinlock *lock)
{
    unsigned long flags = arch_local_irq_save();
    spin_lock(lock);
    return flags;
}

void spin_unlock_irqrestore(struct spinlock *lock, unsigned long flags)
{
    spin_unlock(lock);
    arch_local_irq_restore(flags);
}