#ifndef USER_PTHREAD_H
#define USER_PTHREAD_H

#include <user/syscall.h>
#include <sys/mman.h>
#include <errno.h>

#ifndef NULL
#define NULL ((void*)0)
#endif

typedef struct {
    int id;
} pthread_mutex_t;

typedef struct {
    int id;
} pthread_cond_t;

typedef int pthread_mutexattr_t;
typedef int pthread_condattr_t;
typedef int pthread_attr_t;
typedef unsigned long pthread_t;

/*
 * Static initializer: id = -1 marks the mutex as "not yet allocated".
 * The first lock/trylock lazily allocates a kernel mutex id via an atomic
 * compare-exchange so it is safe even under concurrent first use.
 */
#define PTHREAD_MUTEX_INITIALIZER { -1 }
#define PTHREAD_COND_INITIALIZER { -1 }

#define PTHREAD_STACK_SIZE 65536

/* -------- Mutexes -------- */

/*
 * Atomic compare-and-swap on an int using LL/SC (no libgcc/LSE dependency).
 * Returns the previous value at *ptr.
 */
static inline int __pthread_cas_int(int *ptr, int expected, int desired)
{
    int oldval;
    int status;
    __asm__ volatile(
        "1: ldaxr  %w0, [%2]\n"
        "   cmp    %w0, %w3\n"
        "   b.ne   2f\n"
        "   stlxr  %w1, %w4, [%2]\n"
        "   cbnz   %w1, 1b\n"
        "2:\n"
        : "=&r"(oldval), "=&r"(status)
        : "r"(ptr), "r"(expected), "r"(desired)
        : "memory", "cc");
    return oldval;
}

static inline void __pthread_mutex_ensure(pthread_mutex_t *mutex)
{
    if (mutex->id >= 0) {
        return;
    }
    int new_id = user_mutex_init();
    if (__pthread_cas_int(&mutex->id, -1, new_id) != -1) {
        /* Lost the race: another thread installed an id. Drop ours. */
        user_mutex_destroy(new_id);
    }
}

static inline int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
    (void)attr;
    mutex->id = user_mutex_init();
    return (mutex->id >= 0) ? 0 : -1;
}

static inline int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    if (mutex->id >= 0) {
        user_mutex_destroy(mutex->id);
        mutex->id = -1;
    }
    return 0;
}

static inline int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    __pthread_mutex_ensure(mutex);
    user_mutex_lock(mutex->id);
    return 0;
}

static inline int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
    __pthread_mutex_ensure(mutex);
    return user_mutex_trylock(mutex->id) ? 0 : EBUSY;
}

static inline int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    user_mutex_unlock(mutex->id);
    return 0;
}

/* -------- Condition variables -------- */

static inline void __pthread_cond_ensure(pthread_cond_t *cond)
{
    if (cond->id >= 0) {
        return;
    }
    int new_id = user_cond_init();
    if (__pthread_cas_int(&cond->id, -1, new_id) != -1) {
        user_cond_destroy(new_id);
    }
}

static inline int pthread_cond_init(pthread_cond_t *cond,
                                    const pthread_condattr_t *attr)
{
    (void)attr;
    cond->id = user_cond_init();
    return (cond->id >= 0) ? 0 : -1;
}

static inline int pthread_cond_destroy(pthread_cond_t *cond)
{
    if (cond->id < 0 || user_cond_destroy(cond->id) < 0) {
        return -1;
    }
    cond->id = -1;
    return 0;
}

static inline int pthread_cond_wait(pthread_cond_t *cond,
                                    pthread_mutex_t *mutex)
{
    __pthread_cond_ensure(cond);
    __pthread_mutex_ensure(mutex);
    return user_cond_wait(cond->id, mutex->id) < 0 ? -1 : 0;
}

static inline int pthread_cond_signal(pthread_cond_t *cond)
{
    __pthread_cond_ensure(cond);
    return user_cond_signal(cond->id) < 0 ? -1 : 0;
}

static inline int pthread_cond_broadcast(pthread_cond_t *cond)
{
    __pthread_cond_ensure(cond);
    return user_cond_broadcast(cond->id) < 0 ? -1 : 0;
}

/* -------- Threads -------- */

static inline pthread_t pthread_self(void)
{
    return (pthread_t)user_getpid();
}

static inline void pthread_exit(void *retval)
{
    user_exit((int)(long)retval);
}

static inline int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                 void *(*start_routine)(void *), void *arg)
{
    (void)attr;

    /* Allocate the thread stack in the shared address space (SMP-safe). */
    void *stack_mem = mmap(0, PTHREAD_STACK_SIZE, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack_mem == (void *)-1) {
        return -1;
    }

    /*
     * Reserve a thread-control block at the base of the region for TLS
     * (per-thread errno). The stack grows down from the top, far away.
     */
    struct __pthread_tcb *tcb = (struct __pthread_tcb *)stack_mem;
    tcb->self = tcb;
    tcb->errno_val = 0;

    void *stack_top = (void *)((unsigned long)stack_mem + PTHREAD_STACK_SIZE);

    /* Shared VM + same thread group + install TLS pointer (TPIDR_EL0). */
    unsigned long flags = CLONE_VM | CLONE_THREAD | CLONE_SETTLS;
    long tid = __clone(flags, stack_top, start_routine, arg, tcb);
    if (tid < 0) {
        return -1;
    }

    if (thread) {
        *thread = (pthread_t)tid;
    }
    return 0;
}

static inline int pthread_join(pthread_t thread, void **retval)
{
    int status = 0;
    long r = user_wait4((long)thread, &status);
    if (retval) {
        /* Propagate the thread's exit value (low bits; matches the common
         * "return (void *)(long)small_int" pattern and NULL). */
        *retval = (void *)(long)status;
    }
    return (r < 0) ? -1 : 0;
}

static inline int pthread_detach(pthread_t thread)
{
    /*
     * Threads are reaped either by pthread_join or, if never joined, when
     * the owning process exits (its address space and task slots are freed
     * together). Detach is therefore a well-defined no-op here.
     */
    (void)thread;
    return 0;
}

static inline int sched_yield(void)
{
    user_yield();
    return 0;
}

static inline int pthread_yield(void)
{
    return sched_yield();
}

#endif
