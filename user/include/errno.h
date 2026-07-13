#ifndef USER_ERRNO_H
#define USER_ERRNO_H

#include <user/syscall.h>

/*
 * Minimal thread-control block. TPIDR_EL0 (the EL0 thread pointer) points
 * at one of these for pthreads, giving each thread a private errno slot.
 * The main thread has TPIDR_EL0 == 0 and falls back to a global errno.
 */
struct __pthread_tcb {
    struct __pthread_tcb *self;   /* self pointer (TPIDR_EL0 points here) */
    int errno_val;               /* per-thread errno */
    int _pad;
};

/* Returns the address of the current thread's errno. */
int *__errno_location(void);

#define errno (*__errno_location())

/* Common error numbers (Linux-compatible values). */
#define EPERM    1
#define ESRCH    3
#define EINTR    4
#define EAGAIN  11
#define ENOMEM  12
#define EBUSY   16
#define EINVAL  22

#endif /* USER_ERRNO_H */
