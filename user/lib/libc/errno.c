#include <errno.h>

/* Fallback errno for the main thread (no TLS block installed). */
static int __global_errno;

int *__errno_location(void)
{
    struct __pthread_tcb *tcb = (struct __pthread_tcb *)user_get_tls();
    if (tcb != (struct __pthread_tcb *)0) {
        return &tcb->errno_val;
    }
    return &__global_errno;
}
