#include <kernel/mutex.h>
#include <kernel/sched.h>
#include <kernel/log.h>

#define MAX_MUTEX_POOL 64
#define MUTEX_HANDLE_INDEX_BITS 6U
#define MUTEX_HANDLE_INDEX_MASK ((1U << MUTEX_HANDLE_INDEX_BITS) - 1U)
#define MUTEX_HANDLE_GENERATION_MAX 0x01ffffffU
static struct mutex mutex_pool[MAX_MUTEX_POOL];
static int mutex_pool_used[MAX_MUTEX_POOL];
static unsigned int mutex_pool_generation[MAX_MUTEX_POOL];
static struct spinlock pool_lock = SPINLOCK_INITIALIZER;

static int mutex_handle_index(int handle)
{
    if (handle < 0) {
        return -1;
    }
    return (int)((unsigned int)handle & MUTEX_HANDLE_INDEX_MASK);
}

static unsigned int mutex_handle_generation(int handle)
{
    return (unsigned int)handle >> MUTEX_HANDLE_INDEX_BITS;
}

static int mutex_make_handle(int index, unsigned int generation)
{
    return (int)((generation << MUTEX_HANDLE_INDEX_BITS) |
                 (unsigned int)index);
}

void mutex_init(struct mutex *mutex)
{
    if (mutex == (struct mutex *)0) {
        return;
    }
    spinlock_init(&mutex->lock);
    mutex->locked = 0;
    mutex->owner = (struct task *)0;
    wait_queue_init(&mutex->waiters);
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

    /* Already locked, add current task to the shared FIFO wait queue. */
    struct task *curr = sched_current();
    if (!wait_queue_enqueue(&mutex->waiters, curr)) {
        spin_unlock_irqrestore(&mutex->lock, flags);
        return;
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

    if (wait_queue_empty(&mutex->waiters)) {
        /* No one is waiting */
        mutex->locked = 0;
        mutex->owner = (struct task *)0;
    } else {
        /* Hand off ownership to the next waiting task */
        next_task = wait_queue_dequeue(&mutex->waiters);
        
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
        int detached_waiter = 0;
        unsigned long pool_flags = spin_lock_irqsave(&pool_lock);

        if (!mutex_pool_used[i]) {
            spin_unlock_irqrestore(&pool_lock, pool_flags);
            continue;
        }

        mutex->active_ops++;
        spin_unlock_irqrestore(&pool_lock, pool_flags);

        unsigned long flags = spin_lock_irqsave(&mutex->lock);
        detached_waiter = wait_queue_remove(&mutex->waiters, task);

        if (mutex->owner == task) {
            if (wait_queue_empty(&mutex->waiters)) {
                mutex->owner = (struct task *)0;
                mutex->locked = 0;
            } else {
                wake_task = wait_queue_dequeue(&mutex->waiters);
                mutex->owner = wake_task;
            }
        }
        spin_unlock_irqrestore(&mutex->lock, flags);

        pool_flags = spin_lock_irqsave(&pool_lock);
        mutex->active_ops--;
        /*
         * A task blocked inside mutex_pool_lock() keeps one operation pin
         * until mutex_lock() returns. A killed waiter can never return through
         * that wrapper, so detach must release its abandoned pin after removing
         * it from the queue. The increment above belongs to this detach scan
         * and is released separately by the first decrement.
         */
        if (detached_waiter && mutex->active_ops > 0U) {
            mutex->active_ops--;
        }
        spin_unlock_irqrestore(&pool_lock, pool_flags);

        if (wake_task != (struct task *)0) {
            sched_unpark_task(wake_task);
        }
    }
}

static struct mutex *mutex_pool_pin(int handle)
{
    struct mutex *mutex;
    int index;
    unsigned int generation;
    unsigned long flags;

    index = mutex_handle_index(handle);
    generation = mutex_handle_generation(handle);
    if (index < 0 || index >= MAX_MUTEX_POOL || generation == 0U) {
        return (struct mutex *)0;
    }

    flags = spin_lock_irqsave(&pool_lock);
    mutex = &mutex_pool[index];
    if (!mutex_pool_used[index] ||
        mutex_pool_generation[index] != generation ||
        mutex->destroying != 0U) {
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
            mutex_pool_generation[i]++;
            if (mutex_pool_generation[i] == 0U ||
                mutex_pool_generation[i] > MUTEX_HANDLE_GENERATION_MAX) {
                mutex_pool_generation[i] = 1U;
            }
            mutex_pool_used[i] = 1;
            mutex_init(&mutex_pool[i]);
            spin_unlock_irqrestore(&pool_lock, flags);
            return mutex_make_handle(i, mutex_pool_generation[i]);
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

int mutex_pool_free(int handle)
{
    struct mutex *mutex;
    int index;
    unsigned int generation;
    unsigned long pool_flags;
    unsigned long mutex_flags;

    index = mutex_handle_index(handle);
    generation = mutex_handle_generation(handle);
    if (index < 0 || index >= MAX_MUTEX_POOL || generation == 0U) return 0;

    pool_flags = spin_lock_irqsave(&pool_lock);
    mutex = &mutex_pool[index];
    if (!mutex_pool_used[index] ||
        mutex_pool_generation[index] != generation ||
        mutex->destroying != 0U) {
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
        !wait_queue_empty(&mutex->waiters)) {
        spin_unlock_irqrestore(&mutex->lock, mutex_flags);
        pool_flags = spin_lock_irqsave(&pool_lock);
        mutex->destroying = 0U;
        spin_unlock_irqrestore(&pool_lock, pool_flags);
        return 0;
    }
    spin_unlock_irqrestore(&mutex->lock, mutex_flags);

    pool_flags = spin_lock_irqsave(&pool_lock);
    if (!mutex_pool_used[index] ||
        mutex_pool_generation[index] != generation) {
        mutex->destroying = 0U;
        spin_unlock_irqrestore(&pool_lock, pool_flags);
        return 0;
    }
    mutex_pool_used[index] = 0;
    mutex->destroying = 0U;
    spin_unlock_irqrestore(&pool_lock, pool_flags);
    return 1;
}
