#include <user/syscall.h>
#include <user/shared.h>

int main(void)
{
    char buffer[USER_IPC_MESSAGE_MAX + 1U];
    long len;

    printf("[ipc-recv] waiting on channel 1\n");
    len = user_ipc_recv(1UL, buffer, USER_IPC_MESSAGE_MAX);
    if (len < 0) {
        printf("[ipc-recv] recv failed\n");
        user_exit(1UL);
    }

    if ((unsigned long)len > USER_IPC_MESSAGE_MAX) {
        len = (long)USER_IPC_MESSAGE_MAX;
    }

    buffer[(unsigned long)len] = '\0';
    printf("[ipc-recv] got: %s", buffer);
    if (len == 0 || buffer[(unsigned long)len - 1UL] != '\n') {
        printf("\n");
    }

    user_exit(0UL);
    return 0;
}