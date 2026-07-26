#ifndef KERNEL_CONDVAR_H
#define KERNEL_CONDVAR_H

#include <kernel/spinlock.h>
#include <kernel/wait_queue.h>

struct task;

struct condvar {
    struct spinlock lock;
    struct wait_queue waiters;
    unsigned int active_ops;
    unsigned int destroying;
};

void condvar_init(struct condvar *cond);
int condvar_wait(struct condvar *cond, int mutex_handle);
int condvar_signal(struct condvar *cond);
int condvar_broadcast(struct condvar *cond);
void condvar_detach_task(struct task *task);

/* Generation-tagged condition-variable pool for user space. */
int condvar_pool_alloc(void);
int condvar_pool_wait(int cond_handle, int mutex_handle);
int condvar_pool_signal(int handle);
int condvar_pool_broadcast(int handle);
int condvar_pool_free(int handle);

#endif
