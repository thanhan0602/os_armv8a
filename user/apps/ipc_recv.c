#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <user/syscall.h>

int main(void)
{
    char buffer[USER_IPC_MESSAGE_MAX + 1U];
    long len;

    printf("[ipc-recv] waiting on channel 1\n");
    len = ipc_recv(1UL, buffer, USER_IPC_MESSAGE_MAX);
    if (len < 0) {
        printf("[ipc-recv] recv failed\n");
        exit(1);
    }

    if ((unsigned long)len > USER_IPC_MESSAGE_MAX) {
        len = (long)USER_IPC_MESSAGE_MAX;
    }

    buffer[(unsigned long)len] = '\0';
    printf("[ipc-recv] got: %s", buffer);
    if (len == 0 || buffer[(unsigned long)len - 1UL] != '\n') {
        printf("\n");
    }

    exit(0);
    return 0;
}