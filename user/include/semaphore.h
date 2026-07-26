#ifndef USER_SEMAPHORE_H
#define USER_SEMAPHORE_H

#include <user/syscall.h>
#include <errno.h>

#define SEM_VALUE_MAX 0x7fffffffUL

typedef struct {
    int id;
} sem_t;

static inline int sem_init(sem_t *sem, int pshared, unsigned int value)
{
    if (sem == (sem_t *)0 || value > SEM_VALUE_MAX) {
        return -1;
    }

    /* Handles are kernel-global, so both private and shared modes work. */
    (void)pshared;
    sem->id = user_sem_init((unsigned long)value);
    return (sem->id >= 0) ? 0 : -1;
}

static inline int sem_destroy(sem_t *sem)
{
    if (sem == (sem_t *)0 || sem->id < 0) {
        return -1;
    }

    if (user_sem_destroy(sem->id) < 0) {
        return -1;
    }

    sem->id = -1;
    return 0;
}

static inline int sem_wait(sem_t *sem)
{
    if (sem == (sem_t *)0 || sem->id < 0) {
        return -1;
    }
    return user_sem_wait(sem->id) < 0 ? -1 : 0;
}

static inline int sem_trywait(sem_t *sem)
{
    if (sem == (sem_t *)0 || sem->id < 0) {
        return -1;
    }

    if (!user_sem_trywait(sem->id)) {
        errno = EAGAIN;
        return -1;
    }
    return 0;
}

static inline int sem_post(sem_t *sem)
{
    if (sem == (sem_t *)0 || sem->id < 0) {
        return -1;
    }
    return user_sem_post(sem->id) < 0 ? -1 : 0;
}

#endif
