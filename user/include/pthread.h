#ifndef USER_PTHREAD_H
#define USER_PTHREAD_H

#include <user/syscall.h>

#ifndef NULL
#define NULL ((void*)0)
#endif

/* 
 * Mutex structure for user-space. 
 * Now uses a kernel-side ID to support sharing across processes.
 */
typedef struct {
    int id;
} pthread_mutex_t;

typedef int pthread_mutexattr_t;
typedef int pthread_t;

static inline int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
    (void)attr;
    mutex->id = user_mutex_init();
    return (mutex->id >= 0) ? 0 : -1;
}

static inline int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    user_mutex_destroy(mutex->id);
    return 0;
}

static inline int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    user_mutex_lock(mutex->id);
    return 0;
}

static inline int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
    return user_mutex_trylock(mutex->id) ? 0 : 16; /* 16 is EBUSY in some ABIs */
}

static inline int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    user_mutex_unlock(mutex->id);
    return 0;
}

/* Thread management via fork/wait4 wrappers */

static inline int pthread_create(pthread_t *thread, void *attr, void *(*start_routine)(void *), void *arg)
{
    (void)attr;
    int pid = (int)user_fork();
    if (pid == 0) {
        /* Child: execute routine and exit */
        start_routine(arg);
        user_exit(0);
        return 0; /* Unreachable */
    } else if (pid > 0) {
        /* Parent: return thread handle */
        if (thread) *thread = pid;
        return 0;
    }
    return -1;
}

static inline int pthread_join(pthread_t thread, void **retval)
{
    (void)retval;
    return (int)user_wait4((unsigned long)thread, (int *)0);
}

static inline void pthread_exit(void *retval)
{
    (void)retval;
    user_exit(0);
}

static inline int pthread_yield(void)
{
    user_yield();
    return 0;
}

#endif
