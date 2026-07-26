#ifndef KERNEL_MUTEX_H
#define KERNEL_MUTEX_H

#include <kernel/spinlock.h>
#include <kernel/sched.h>
#include <kernel/wait_queue.h>

struct mutex {
    struct spinlock lock;
    int locked;
    struct task *owner;
    struct wait_queue waiters;
    unsigned int active_ops;
    unsigned int destroying;
};

void mutex_init(struct mutex *mutex);
void mutex_lock(struct mutex *mutex);
int  mutex_trylock(struct mutex *mutex);
void mutex_unlock(struct mutex *mutex);
void mutex_detach_task(struct task *task);

/* Mutex pool for user-space sharing */
int mutex_pool_alloc(void);
int mutex_pool_lock(int id);
int mutex_pool_trylock(int id);
int mutex_pool_unlock(int id);
int mutex_pool_free(int id);

/* Integration test */
void mutex_run_test(void);

#endif
