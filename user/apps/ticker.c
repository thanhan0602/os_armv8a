#include <user/syscall.h>

int main(void)
{
    for (;;) {
        user_write_string("[user-ticker] tick\n");
        user_yield();
    }
}