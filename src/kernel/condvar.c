#include <kernel/condvar.h>
#include <kernel/mutex.h>
#include <kernel/sched.h>

#define MAX_CONDVAR_POOL 64
#define CONDVAR_HANDLE_INDEX_BITS 6U
#define CONDVAR_HANDLE_INDEX_MASK ((1U << CONDVAR_HANDLE_INDEX_BITS) - 1U)
#define CONDVAR_HANDLE_GENERATION_MAX 0x01ffffffU

static struct condvar condvar_pool[MAX_CONDVAR_POOL];
static int condvar_pool_used[MAX_CONDVAR_POOL];
static unsigned int condvar_pool_generation[MAX_CONDVAR_POOL];
static struct spinlock condvar_pool_lock = SPINLOCK_INITIALIZER;

static int condvar_handle_index(int handle)
{
    if (handle < 0) return -1;
    return (int)((unsigned int)handle & CONDVAR_HANDLE_INDEX_MASK);
}

static unsigned int condvar_handle_generation(int handle)
{
    return (unsigned int)handle >> CONDVAR_HANDLE_INDEX_BITS;
}

static int condvar_make_handle(int index, unsigned int generation)
{
    return (int)((generation << CONDVAR_HANDLE_INDEX_BITS) |
                 (unsigned int)index);
}

void condvar_init(struct condvar *cond)
{
    if (cond == (struct condvar *)0) return;
    spinlock_init(&cond->lock);
    wait_queue_init(&cond->waiters);
    cond->active_ops = 0U;
    cond->destroying = 0U;
}

int condvar_wait(struct condvar *cond, int mutex_handle)
{
    struct task *current;
    unsigned long flags;

    if (cond == (struct condvar *)0) return 0;
    current = sched_current();
    if (current == (struct task *)0) return 0;

    /*
     * Publish the waiter while holding the condition lock. The mutex is then
     * released before parking. A concurrent signal may arrive in that window;
     * sched_unpark_task() leaves a wake token which sched_park_task() consumes.
     */
    flags = spin_lock_irqsave(&cond->lock);
    if (!wait_queue_enqueue(&cond->waiters, current)) {
        spin_unlock_irqrestore(&cond->lock, flags);
        return 0;
    }

    if (!mutex_pool_unlock(mutex_handle)) {
        (void)wait_queue_remove(&cond->waiters, current);
        spin_unlock_irqrestore(&cond->lock, flags);
        return 0;
    }
    spin_unlock_irqrestore(&cond->lock, flags);

    if (sched_park_task(current)) schedule();

    /* POSIX condition waits return with the associated mutex reacquired. */
    return mutex_pool_lock(mutex_handle);
}

int condvar_signal(struct condvar *cond)
{
    struct task *waiter;
    unsigned long flags;

    if (cond == (struct condvar *)0) return 0;
    flags = spin_lock_irqsave(&cond->lock);
    waiter = wait_queue_dequeue(&cond->waiters);
    spin_unlock_irqrestore(&cond->lock, flags);

    if (waiter != (struct task *)0) sched_unpark_task(waiter);
    return 1;
}

int condvar_broadcast(struct condvar *cond)
{
    struct task *waiter;
    unsigned long flags;

    if (cond == (struct condvar *)0) return 0;

    for (;;) {
        flags = spin_lock_irqsave(&cond->lock);
        waiter = wait_queue_dequeue(&cond->waiters);
        spin_unlock_irqrestore(&cond->lock, flags);
        if (waiter == (struct task *)0) break;
        sched_unpark_task(waiter);
    }
    return 1;
}

static struct condvar *condvar_pool_pin(int handle)
{
    int index = condvar_handle_index(handle);
    unsigned int generation = condvar_handle_generation(handle);
    struct condvar *cond;
    unsigned long flags;

    if (index < 0 || index >= MAX_CONDVAR_POOL || generation == 0U)
        return (struct condvar *)0;

    flags = spin_lock_irqsave(&condvar_pool_lock);
    cond = &condvar_pool[index];
    if (!condvar_pool_used[index] ||
        condvar_pool_generation[index] != generation ||
        cond->destroying != 0U) {
        spin_unlock_irqrestore(&condvar_pool_lock, flags);
        return (struct condvar *)0;
    }
    cond->active_ops++;
    spin_unlock_irqrestore(&condvar_pool_lock, flags);
    return cond;
}

static void condvar_pool_unpin(struct condvar *cond)
{
    unsigned long flags = spin_lock_irqsave(&condvar_pool_lock);
    if (cond != (struct condvar *)0 && cond->active_ops > 0U)
        cond->active_ops--;
    spin_unlock_irqrestore(&condvar_pool_lock, flags);
}

int condvar_pool_alloc(void)
{
    unsigned long flags = spin_lock_irqsave(&condvar_pool_lock);

    for (int i = 0; i < MAX_CONDVAR_POOL; i++) {
        if (!condvar_pool_used[i]) {
            condvar_pool_generation[i]++;
            if (condvar_pool_generation[i] == 0U ||
                condvar_pool_generation[i] > CONDVAR_HANDLE_GENERATION_MAX)
                condvar_pool_generation[i] = 1U;
            condvar_pool_used[i] = 1;
            condvar_init(&condvar_pool[i]);
            spin_unlock_irqrestore(&condvar_pool_lock, flags);
            return condvar_make_handle(i, condvar_pool_generation[i]);
        }
    }

    spin_unlock_irqrestore(&condvar_pool_lock, flags);
    return -1;
}

int condvar_pool_wait(int cond_handle, int mutex_handle)
{
    struct condvar *cond = condvar_pool_pin(cond_handle);
    int result;

    if (cond == (struct condvar *)0) return 0;
    result = condvar_wait(cond, mutex_handle);
    condvar_pool_unpin(cond);
    return result;
}

int condvar_pool_signal(int handle)
{
    struct condvar *cond = condvar_pool_pin(handle);
    int result;

    if (cond == (struct condvar *)0) return 0;
    result = condvar_signal(cond);
    condvar_pool_unpin(cond);
    return result;
}

int condvar_pool_broadcast(int handle)
{
    struct condvar *cond = condvar_pool_pin(handle);
    int result;

    if (cond == (struct condvar *)0) return 0;
    result = condvar_broadcast(cond);
    condvar_pool_unpin(cond);
    return result;
}

int condvar_pool_free(int handle)
{
    int index = condvar_handle_index(handle);
    unsigned int generation = condvar_handle_generation(handle);
    struct condvar *cond;
    unsigned long pool_flags;
    unsigned long cond_flags;

    if (index < 0 || index >= MAX_CONDVAR_POOL || generation == 0U)
        return 0;

    pool_flags = spin_lock_irqsave(&condvar_pool_lock);
    cond = &condvar_pool[index];
    if (!condvar_pool_used[index] ||
        condvar_pool_generation[index] != generation ||
        cond->destroying != 0U || cond->active_ops != 0U) {
        spin_unlock_irqrestore(&condvar_pool_lock, pool_flags);
        return 0;
    }
    cond->destroying = 1U;
    spin_unlock_irqrestore(&condvar_pool_lock, pool_flags);

    cond_flags = spin_lock_irqsave(&cond->lock);
    if (!wait_queue_empty(&cond->waiters)) {
        spin_unlock_irqrestore(&cond->lock, cond_flags);
        pool_flags = spin_lock_irqsave(&condvar_pool_lock);
        cond->destroying = 0U;
        spin_unlock_irqrestore(&condvar_pool_lock, pool_flags);
        return 0;
    }
    spin_unlock_irqrestore(&cond->lock, cond_flags);

    pool_flags = spin_lock_irqsave(&condvar_pool_lock);
    if (!condvar_pool_used[index] ||
        condvar_pool_generation[index] != generation) {
        cond->destroying = 0U;
        spin_unlock_irqrestore(&condvar_pool_lock, pool_flags);
        return 0;
    }
    condvar_pool_used[index] = 0;
    cond->destroying = 0U;
    spin_unlock_irqrestore(&condvar_pool_lock, pool_flags);
    return 1;
}

void condvar_detach_task(struct task *task)
{
    if (task == (struct task *)0) return;

    for (int i = 0; i < MAX_CONDVAR_POOL; i++) {
        struct condvar *cond = &condvar_pool[i];
        unsigned long pool_flags;
        unsigned long cond_flags;
        int detached;

        pool_flags = spin_lock_irqsave(&condvar_pool_lock);
        if (!condvar_pool_used[i]) {
            spin_unlock_irqrestore(&condvar_pool_lock, pool_flags);
            continue;
        }
        cond->active_ops++;
        spin_unlock_irqrestore(&condvar_pool_lock, pool_flags);

        cond_flags = spin_lock_irqsave(&cond->lock);
        detached = wait_queue_remove(&cond->waiters, task);
        spin_unlock_irqrestore(&cond->lock, cond_flags);

        pool_flags = spin_lock_irqsave(&condvar_pool_lock);
        if (cond->active_ops > 0U) cond->active_ops--;
        /* A blocked wait owns a pin that it cannot release after being killed. */
        if (detached && cond->active_ops > 0U) cond->active_ops--;
        spin_unlock_irqrestore(&condvar_pool_lock, pool_flags);
    }
}
