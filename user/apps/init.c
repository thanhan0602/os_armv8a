#include <stdio.h>
#include <unistd.h>

int main(void)
{
    int status;
    int child;

    printf("[init] started pid=%d\n", getpid());

    for (;;) {
        child = wait(&status);
        if (child >= 0) {
            printf("[init] reaped pid=%d status=%d\n", child, status);
            continue;
        }

        /* No child is currently waitable. Yield until a service is adopted. */
        yield();
    }
}
