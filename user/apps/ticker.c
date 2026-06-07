#include <user/syscall.h>
#include <user/shared.h>

int main(void)
{
    for (;;) {
        printf("[user-ticker] tick\n");
        user_yield();
    }
}