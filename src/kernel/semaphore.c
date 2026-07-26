#include <kernel/semaphore.h>
#include <kernel/sched.h>

#define MAX_SEMAPHORE_POOL 64
#define SEM_HANDLE_INDEX_BITS 6U
#define SEM_HANDLE_INDEX_MASK ((1U << SEM_HANDLE_INDEX_BITS) - 1U)
#define SEM_HANDLE_GENERATION_MAX 0x01ffffffU

static struct semaphore semaphore_pool[MAX_SEMAPHORE_POOL];
static int semaphore_pool_used[MAX_SEMAPHORE_POOL];
static unsigned int semaphore_pool_generation[MAX_SEMAPHORE_POOL];
static struct spinlock semaphore_pool_lock = SPINLOCK_INITIALIZER;

static int semaphore_handle_index(int handle)
{
    if (handle < 0) return -1;
    return (int)((unsigned int)handle & SEM_HANDLE_INDEX_MASK);
}

static unsigned int semaphore_handle_generation(int handle)
{
    return (unsigned int)handle >> SEM_HANDLE_INDEX_BITS;
}

static int semaphore_make_handle(int index, unsigned int generation)
{
    return (int)((generation << SEM_HANDLE_INDEX_BITS) |
                 (unsigned int)index);
}

void semaphore_init(struct semaphore *sem, unsigned long value)
{
    if (sem == (struct semaphore *)0) return;
    spinlock_init(&sem->lock);
    sem->value = value;
    wait_queue_init(&sem->waiters);
    sem->active_ops = 0U;
    sem->destroying = 0U;
}

int semaphore_wait(struct semaphore *sem)
{
    struct task *current;
    unsigned long flags;

    if (sem == (struct semaphore *)0) return 0;
    current = sched_current();
    if (current == (struct task *)0) return 0;

    flags = spin_lock_irqsave(&sem->lock);
    if (sem->value > 0UL) {
        sem->value--;
        spin_unlock_irqrestore(&sem->lock, flags);
        return 1;
    }

    if (!wait_queue_enqueue(&sem->waiters, current)) {
        spin_unlock_irqrestore(&sem->lock, flags);
        return 0;
    }
    spin_unlock_irqrestore(&sem->lock, flags);

    if (sched_park_task(current)) schedule();
    return 1;
}

int semaphore_trywait(struct semaphore *sem)
{
    unsigned long flags;

    if (sem == (struct semaphore *)0) return 0;
    flags = spin_lock_irqsave(&sem->lock);
    if (sem->value == 0UL) {
        spin_unlock_irqrestore(&sem->lock, flags);
        return 0;
    }
    sem->value--;
    spin_unlock_irqrestore(&sem->lock, flags);
    return 1;
}

int semaphore_post(struct semaphore *sem)
{
    struct task *waiter;
    unsigned long flags;

    if (sem == (struct semaphore *)0) return 0;
    flags = spin_lock_irqsave(&sem->lock);
    waiter = wait_queue_dequeue(&sem->waiters);
    if (waiter == (struct task *)0) sem->value++;
    spin_unlock_irqrestore(&sem->lock, flags);

    if (waiter != (struct task *)0) sched_unpark_task(waiter);
    return 1;
}

static struct semaphore *semaphore_pool_pin(int handle)
{
    int index = semaphore_handle_index(handle);
    unsigned int generation = semaphore_handle_generation(handle);
    struct semaphore *sem;
    unsigned long flags;

    if (index < 0 || index >= MAX_SEMAPHORE_POOL || generation == 0U)
        return (struct semaphore *)0;

    flags = spin_lock_irqsave(&semaphore_pool_lock);
    sem = &semaphore_pool[index];
    if (!semaphore_pool_used[index] ||
        semaphore_pool_generation[index] != generation ||
        sem->destroying != 0U) {
        spin_unlock_irqrestore(&semaphore_pool_lock, flags);
        return (struct semaphore *)0;
    }
    sem->active_ops++;
    spin_unlock_irqrestore(&semaphore_pool_lock, flags);
    return sem;
}

static void semaphore_pool_unpin(struct semaphore *sem)
{
    unsigned long flags = spin_lock_irqsave(&semaphore_pool_lock);
    if (sem != (struct semaphore *)0 && sem->active_ops > 0U)
        sem->active_ops--;
    spin_unlock_irqrestore(&semaphore_pool_lock, flags);
}

int semaphore_pool_alloc(unsigned long value)
{
    unsigned long flags = spin_lock_irqsave(&semaphore_pool_lock);

    for (int i = 0; i < MAX_SEMAPHORE_POOL; i++) {
        if (!semaphore_pool_used[i]) {
            semaphore_pool_generation[i]++;
            if (semaphore_pool_generation[i] == 0U ||
                semaphore_pool_generation[i] > SEM_HANDLE_GENERATION_MAX)
                semaphore_pool_generation[i] = 1U;
            semaphore_pool_used[i] = 1;
            semaphore_init(&semaphore_pool[i], value);
            spin_unlock_irqrestore(&semaphore_pool_lock, flags);
            return semaphore_make_handle(i, semaphore_pool_generation[i]);
        }
    }

    spin_unlock_irqrestore(&semaphore_pool_lock, flags);
    return -1;
}

int semaphore_pool_wait(int handle)
{
    struct semaphore *sem = semaphore_pool_pin(handle);
    int result;
    if (sem == (struct semaphore *)0) return 0;
    result = semaphore_wait(sem);
    semaphore_pool_unpin(sem);
    return result;
}

int semaphore_pool_trywait(int handle)
{
    struct semaphore *sem = semaphore_pool_pin(handle);
    int result;
    if (sem == (struct semaphore *)0) return 0;
    result = semaphore_trywait(sem);
    semaphore_pool_unpin(sem);
    return result;
}

int semaphore_pool_post(int handle)
{
    struct semaphore *sem = semaphore_pool_pin(handle);
    int result;
    if (sem == (struct semaphore *)0) return 0;
    result = semaphore_post(sem);
    semaphore_pool_unpin(sem);
    return result;
}

int semaphore_pool_free(int handle)
{
    int index = semaphore_handle_index(handle);
    unsigned int generation = semaphore_handle_generation(handle);
    struct semaphore *sem;
    unsigned long pool_flags;
    unsigned long sem_flags;

    if (index < 0 || index >= MAX_SEMAPHORE_POOL || generation == 0U)
        return 0;

    pool_flags = spin_lock_irqsave(&semaphore_pool_lock);
    sem = &semaphore_pool[index];
    if (!semaphore_pool_used[index] ||
        semaphore_pool_generation[index] != generation ||
        sem->destroying != 0U || sem->active_ops != 0U) {
        spin_unlock_irqrestore(&semaphore_pool_lock, pool_flags);
        return 0;
    }
    sem->destroying = 1U;
    spin_unlock_irqrestore(&semaphore_pool_lock, pool_flags);

    sem_flags = spin_lock_irqsave(&sem->lock);
    if (!wait_queue_empty(&sem->waiters)) {
        spin_unlock_irqrestore(&sem->lock, sem_flags);
        pool_flags = spin_lock_irqsave(&semaphore_pool_lock);
        sem->destroying = 0U;
        spin_unlock_irqrestore(&semaphore_pool_lock, pool_flags);
        return 0;
    }
    spin_unlock_irqrestore(&sem->lock, sem_flags);

    pool_flags = spin_lock_irqsave(&semaphore_pool_lock);
    if (!semaphore_pool_used[index] ||
        semaphore_pool_generation[index] != generation) {
        sem->destroying = 0U;
        spin_unlock_irqrestore(&semaphore_pool_lock, pool_flags);
        return 0;
    }
    semaphore_pool_used[index] = 0;
    sem->destroying = 0U;
    spin_unlock_irqrestore(&semaphore_pool_lock, pool_flags);
    return 1;
}

void semaphore_detach_task(struct task *task)
{
    if (task == (struct task *)0) return;

    for (int i = 0; i < MAX_SEMAPHORE_POOL; i++) {
        struct semaphore *sem = &semaphore_pool[i];
        unsigned long pool_flags;
        unsigned long sem_flags;
        int detached;

        pool_flags = spin_lock_irqsave(&semaphore_pool_lock);
        if (!semaphore_pool_used[i]) {
            spin_unlock_irqrestore(&semaphore_pool_lock, pool_flags);
            continue;
        }
        sem->active_ops++;
        spin_unlock_irqrestore(&semaphore_pool_lock, pool_flags);

        sem_flags = spin_lock_irqsave(&sem->lock);
        detached = wait_queue_remove(&sem->waiters, task);
        spin_unlock_irqrestore(&sem->lock, sem_flags);

        pool_flags = spin_lock_irqsave(&semaphore_pool_lock);
        if (sem->active_ops > 0U) sem->active_ops--;
        if (detached && sem->active_ops > 0U) sem->active_ops--;
        spin_unlock_irqrestore(&semaphore_pool_lock, pool_flags);
    }
}
