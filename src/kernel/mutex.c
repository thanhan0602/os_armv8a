#include <kernel/mutex.h>
#include <kernel/sched.h>
#include <kernel/log.h>

#define MAX_MUTEX_POOL 64
static struct mutex mutex_pool[MAX_MUTEX_POOL];
static int mutex_pool_used[MAX_MUTEX_POOL];
static struct spinlock pool_lock = SPINLOCK_INITIALIZER;

void mutex_init(struct mutex *mutex)
{
    if (mutex == (struct mutex *)0) {
        return;
    }
    spinlock_init(&mutex->lock);
    mutex->locked = 0;
    mutex->owner = (struct task *)0;
    mutex->wait_queue = (struct task *)0;
    mutex->active_ops = 0U;
    mutex->destroying = 0U;
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

    /* Never acquire sched_lock while holding mutex->lock. */
    spin_unlock_irqrestore(&mutex->lock, flags);

    /*
     * unlock may hand ownership to us before this call. The scheduler wake
     * token makes that race safe and prevents a lost wakeup.
     */
    if (sched_park_task(curr)) {
        schedule();
    }

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
    struct task *next_task = (struct task *)0;

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
        next_task = mutex->wait_queue;
        mutex->wait_queue = next_task->wait_next;
        next_task->wait_next = (struct task *)0;
        
        mutex->owner = next_task;
    }

    spin_unlock_irqrestore(&mutex->lock, flags);

    /* Never acquire sched_lock while holding mutex->lock. */
    if (next_task != (struct task *)0) {
        sched_unpark_task(next_task);
    }
}

void mutex_detach_task(struct task *task)
{
    if (task == (struct task *)0) {
        return;
    }

    for (int i = 0; i < MAX_MUTEX_POOL; i++) {
        struct mutex *mutex = &mutex_pool[i];
        struct task *wake_task = (struct task *)0;
        unsigned long pool_flags = spin_lock_irqsave(&pool_lock);

        if (!mutex_pool_used[i]) {
            spin_unlock_irqrestore(&pool_lock, pool_flags);
            continue;
        }

        mutex->active_ops++;
        spin_unlock_irqrestore(&pool_lock, pool_flags);

        unsigned long flags = spin_lock_irqsave(&mutex->lock);
        struct task *prev = (struct task *)0;
        struct task *cur = mutex->wait_queue;
        while (cur != (struct task *)0) {
            if (cur == task) {
                if (prev == (struct task *)0) {
                    mutex->wait_queue = cur->wait_next;
                } else {
                    prev->wait_next = cur->wait_next;
                }
                cur->wait_next = (struct task *)0;
                break;
            }
            prev = cur;
            cur = cur->wait_next;
        }

        if (mutex->owner == task) {
            if (mutex->wait_queue == (struct task *)0) {
                mutex->owner = (struct task *)0;
                mutex->locked = 0;
            } else {
                wake_task = mutex->wait_queue;
                mutex->wait_queue = wake_task->wait_next;
                wake_task->wait_next = (struct task *)0;
                mutex->owner = wake_task;
            }
        }
        spin_unlock_irqrestore(&mutex->lock, flags);

        pool_flags = spin_lock_irqsave(&pool_lock);
        mutex->active_ops--;
        spin_unlock_irqrestore(&pool_lock, pool_flags);

        if (wake_task != (struct task *)0) {
            sched_unpark_task(wake_task);
        }
    }
}

static struct mutex *mutex_pool_pin(int id)
{
    struct mutex *mutex;
    unsigned long flags;

    if (id < 0 || id >= MAX_MUTEX_POOL) {
        return (struct mutex *)0;
    }

    flags = spin_lock_irqsave(&pool_lock);
    mutex = &mutex_pool[id];
    if (!mutex_pool_used[id] || mutex->destroying != 0U) {
        spin_unlock_irqrestore(&pool_lock, flags);
        return (struct mutex *)0;
    }
    mutex->active_ops++;
    spin_unlock_irqrestore(&pool_lock, flags);
    return mutex;
}

static void mutex_pool_unpin(struct mutex *mutex)
{
    unsigned long flags = spin_lock_irqsave(&pool_lock);
    if (mutex != (struct mutex *)0 && mutex->active_ops > 0U) {
        mutex->active_ops--;
    }
    spin_unlock_irqrestore(&pool_lock, flags);
}

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

int mutex_pool_lock(int id)
{
    struct mutex *mutex = mutex_pool_pin(id);
    if (mutex == (struct mutex *)0) return 0;
    mutex_lock(mutex);
    mutex_pool_unpin(mutex);
    return 1;
}

int mutex_pool_trylock(int id)
{
    struct mutex *mutex = mutex_pool_pin(id);
    int result;
    if (mutex == (struct mutex *)0) return 0;
    result = mutex_trylock(mutex);
    mutex_pool_unpin(mutex);
    return result;
}

int mutex_pool_unlock(int id)
{
    struct mutex *mutex = mutex_pool_pin(id);
    if (mutex == (struct mutex *)0) return 0;
    mutex_unlock(mutex);
    mutex_pool_unpin(mutex);
    return 1;
}

int mutex_pool_free(int id)
{
    struct mutex *mutex;
    unsigned long pool_flags;
    unsigned long mutex_flags;

    if (id < 0 || id >= MAX_MUTEX_POOL) return 0;

    pool_flags = spin_lock_irqsave(&pool_lock);
    mutex = &mutex_pool[id];
    if (!mutex_pool_used[id] || mutex->destroying != 0U) {
        spin_unlock_irqrestore(&pool_lock, pool_flags);
        return 0;
    }
    mutex->destroying = 1U;
    if (mutex->active_ops != 0U) {
        mutex->destroying = 0U;
        spin_unlock_irqrestore(&pool_lock, pool_flags);
        return 0;
    }
    spin_unlock_irqrestore(&pool_lock, pool_flags);

    mutex_flags = spin_lock_irqsave(&mutex->lock);
    if (mutex->locked || mutex->owner != (struct task *)0 ||
        mutex->wait_queue != (struct task *)0) {
        spin_unlock_irqrestore(&mutex->lock, mutex_flags);
        pool_flags = spin_lock_irqsave(&pool_lock);
        mutex->destroying = 0U;
        spin_unlock_irqrestore(&pool_lock, pool_flags);
        return 0;
    }
    spin_unlock_irqrestore(&mutex->lock, mutex_flags);

    pool_flags = spin_lock_irqsave(&pool_lock);
    mutex_pool_used[id] = 0;
    mutex->destroying = 0U;
    spin_unlock_irqrestore(&pool_lock, pool_flags);
    return 1;
}
