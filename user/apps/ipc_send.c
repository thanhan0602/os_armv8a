#include <user/syscall.h>

int main(void)
{
    static const char message[] = "hello from ipc_send\n";
    long result;

    result = user_ipc_send(1UL, message, sizeof(message) - 1UL);
    if (result < 0) {
        user_write_string("[ipc-send] send failed\n");
        user_exit(1UL);
    }

    user_write_string("[ipc-send] sent message on channel 1\n");
    user_exit(0UL);
    return 0;
}