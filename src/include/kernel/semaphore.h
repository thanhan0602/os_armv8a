#ifndef KERNEL_SEMAPHORE_H
#define KERNEL_SEMAPHORE_H

#include <kernel/spinlock.h>
#include <kernel/wait_queue.h>

struct task;

struct semaphore {
    struct spinlock lock;
    unsigned long value;
    struct wait_queue waiters;
    unsigned int active_ops;
    unsigned int destroying;
};

void semaphore_init(struct semaphore *sem, unsigned long value);
int semaphore_wait(struct semaphore *sem);
int semaphore_trywait(struct semaphore *sem);
int semaphore_post(struct semaphore *sem);
void semaphore_detach_task(struct task *task);

/* Generation-tagged semaphore pool for user space. */
int semaphore_pool_alloc(unsigned long value);
int semaphore_pool_wait(int handle);
int semaphore_pool_trywait(int handle);
int semaphore_pool_post(int handle);
int semaphore_pool_free(int handle);

#endif
