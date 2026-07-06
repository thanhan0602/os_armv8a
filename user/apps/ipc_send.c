#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

int main(void)
{
    static const char message[] = "hello from ipc_send\n";
    long result;

    result = ipc_send(1UL, message, sizeof(message) - 1UL);
    if (result < 0) {
        printf("[ipc-send] send failed\n");
        exit(1);
    }

    printf("[ipc-send] sent message on channel 1\n");
    exit(0);
    return 0;
}