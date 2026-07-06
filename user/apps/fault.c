#include <stdio.h>
#include <unistd.h>

int main(void)
{
    const char *text;
    unsigned long index;

    for (index = 0UL; index < 3UL; index++) {
        printf("[fault:%d] hello from ELF\n", getpid());
        yield();
    }

    text = "[user-fault] hello from ELF\n";
    *(volatile unsigned long *)(unsigned long)text = 0x12345678UL;
    return 0;
}