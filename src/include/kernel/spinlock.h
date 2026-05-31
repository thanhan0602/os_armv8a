#ifndef KERNEL_SPINLOCK_H
#define KERNEL_SPINLOCK_H

struct spinlock {
    volatile unsigned int value;
};

#define SPINLOCK_INITIALIZER  { 0U }

void spinlock_init(struct spinlock *lock);
void spin_lock(struct spinlock *lock);
void spin_unlock(struct spinlock *lock);
unsigned long spin_lock_irqsave(struct spinlock *lock);
void spin_unlock_irqrestore(struct spinlock *lock, unsigned long flags);
void cpu_relax(void);
void cpu_wait(void);
void cpu_wake(void);

#endif