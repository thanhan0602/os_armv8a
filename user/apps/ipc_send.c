#include <user/syscall.h>
#include <user/shared.h>

int main(void)
{
    static const char message[] = "hello from ipc_send\n";
    long result;

    result = user_ipc_send(1UL, message, sizeof(message) - 1UL);
    if (result < 0) {
        printf("[ipc-send] send failed\n");
        user_exit(1UL);
    }

    printf("[ipc-send] sent message on channel 1\n");
    user_exit(0UL);
    return 0;
}