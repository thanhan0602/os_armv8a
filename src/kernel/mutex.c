#include <kernel/mutex.h>
#include <kernel/sched.h>
#include <kernel/log.h>

void mutex_init(struct mutex *mutex)
{
    if (mutex == (struct mutex *)0) {
        return;
    }
    spinlock_init(&mutex->lock);
    mutex->locked = 0;
    mutex->owner = (struct task *)0;
    mutex->wait_queue = (struct task *)0;
}

void mutex_lock(struct mutex *mutex)
{
    if (mutex == (struct mutex *)0) {
        return;
    }

    unsigned long flags = spin_lock_irqsave(&mutex->lock);

    if (mutex->locked == 0) {
        mutex->locked = 1;
        mutex->owner = sched_current();
        spin_unlock_irqrestore(&mutex->lock, flags);
        return;
    }

    /* Already locked, add current task to wait queue */
    struct task *curr = sched_current();
    curr->wait_next = (struct task *)0;

    if (mutex->wait_queue == (struct task *)0) {
        mutex->wait_queue = curr;
    } else {
        struct task *t = mutex->wait_queue;
        while (t->wait_next != (struct task *)0) {
            t = t->wait_next;
        }
        t->wait_next = curr;
    }

    /* Block the task */
    sched_block_task(curr);

    /* 
     * We must release the mutex spinlock before calling schedule() 
     * to avoid deadlocks and allowing other cores to perform mutex operations.
     */
    spin_unlock_irqrestore(&mutex->lock, flags);

    /* Yield the processor */
    schedule();

    /* 
     * When we return from schedule(), we are the new owner.
     * Note: In this simple implementation, mutex_unlock sets the owner.
     */
}

int mutex_trylock(struct mutex *mutex)
{
    if (mutex == (struct mutex *)0) {
        return 0;
    }

    unsigned long flags = spin_lock_irqsave(&mutex->lock);
    if (mutex->locked == 0) {
        mutex->locked = 1;
        mutex->owner = sched_current();
        spin_unlock_irqrestore(&mutex->lock, flags);
        return 1;
    }
    spin_unlock_irqrestore(&mutex->lock, flags);
    return 0;
}

void mutex_unlock(struct mutex *mutex)
{
    if (mutex == (struct mutex *)0) {
        return;
    }

    unsigned long flags = spin_lock_irqsave(&mutex->lock);

    if (mutex->wait_queue == (struct task *)0) {
        /* No one is waiting */
        mutex->locked = 0;
        mutex->owner = (struct task *)0;
    } else {
        /* Hand off ownership to the next waiting task */
        struct task *next_task = mutex->wait_queue;
        mutex->wait_queue = next_task->wait_next;
        next_task->wait_next = (struct task *)0;
        
        mutex->owner = next_task;
        
        /* Wake up the task */
        sched_wake_task(next_task);
    }

    spin_unlock_irqrestore(&mutex->lock, flags);
}

#define MAX_MUTEX_POOL 64
static struct mutex mutex_pool[MAX_MUTEX_POOL];
static int mutex_pool_used[MAX_MUTEX_POOL];
static struct spinlock pool_lock = {0};

int mutex_pool_alloc(void)
{
    unsigned long flags = spin_lock_irqsave(&pool_lock);
    for (int i = 0; i < MAX_MUTEX_POOL; i++) {
        if (!mutex_pool_used[i]) {
            mutex_pool_used[i] = 1;
            mutex_init(&mutex_pool[i]);
            spin_unlock_irqrestore(&pool_lock, flags);
            return i;
        }
    }
    spin_unlock_irqrestore(&pool_lock, flags);
    return -1;
}

struct mutex *mutex_pool_get(int id)
{
    if (id < 0 || id >= MAX_MUTEX_POOL) return (struct mutex *)0;
    if (!mutex_pool_used[id]) return (struct mutex *)0;
    return &mutex_pool[id];
}

void mutex_pool_free(int id)
{
    unsigned long flags = spin_lock_irqsave(&pool_lock);
    if (id >= 0 && id < MAX_MUTEX_POOL) {
        mutex_pool_used[id] = 0;
    }
    spin_unlock_irqrestore(&pool_lock, flags);
}
