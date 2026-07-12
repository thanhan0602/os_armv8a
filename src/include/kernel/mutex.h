#ifndef KERNEL_MUTEX_H
#define KERNEL_MUTEX_H

#include <kernel/spinlock.h>
#include <kernel/sched.h>

struct mutex {
    struct spinlock lock;
    int locked;
    struct task *owner;
    struct task *wait_queue;
};

void mutex_init(struct mutex *mutex);
void mutex_lock(struct mutex *mutex);
int  mutex_trylock(struct mutex *mutex);
void mutex_unlock(struct mutex *mutex);

/* Mutex pool for user-space sharing */
int mutex_pool_alloc(void);
struct mutex *mutex_pool_get(int id);
void mutex_pool_free(int id);

/* Integration test */
void mutex_run_test(void);

#endif
