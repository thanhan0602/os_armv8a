#include <user/syscall.h>

int main(void)
{
    char buffer[USER_IPC_MESSAGE_MAX + 1U];
    long len;

    user_write_string("[ipc-recv] waiting on channel 1\n");
    len = user_ipc_recv(1UL, buffer, USER_IPC_MESSAGE_MAX);
    if (len < 0) {
        user_write_string("[ipc-recv] recv failed\n");
        user_exit(1UL);
    }

    buffer[(unsigned long)len] = '\0';
    user_write_string("[ipc-recv] got: ");
    user_write(1UL, buffer, (unsigned long)len);
    if (len == 0 || buffer[(unsigned long)len - 1UL] != '\n') {
        user_write_string("\n");
    }

    user_exit(0UL);
    return 0;
}