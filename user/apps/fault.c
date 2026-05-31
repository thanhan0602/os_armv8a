#include <user/syscall.h>

int main(void)
{
    const char *text;
    unsigned long index;

    for (index = 0UL; index < 3UL; index++) {
        user_write_string("[user-fault] hello from ELF\n");
        user_yield();
    }

    text = "[user-fault] hello from ELF\n";
    *(volatile unsigned long *)(unsigned long)text = 0x12345678UL;
    return 0;
}