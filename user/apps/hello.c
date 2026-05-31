#include <user/syscall.h>

int main(void)
{
    const char *hello;
    const char *heap_ok;
    unsigned long heap_base;
    unsigned long new_break;
    unsigned long index;

    hello = "[user-elf] hello from ELF\n";
    heap_ok = "[user-elf] brk ok\n";

    for (index = 0UL; index < 3UL; index++) {
        user_write_string(hello);
        user_yield();
    }

    heap_base = user_brk(0UL);
    new_break = user_brk(heap_base + user_strlen(heap_ok));
    if (new_break != heap_base) {
        char *heap_text;

        heap_text = (char *)heap_base;
        for (index = 0UL; index < user_strlen(heap_ok); index++) {
            heap_text[index] = heap_ok[index];
        }
        user_write(1UL, heap_text, user_strlen(heap_ok));
    }

    *(volatile unsigned long *)0xDEAD0000UL = 0xDEAD0000UL;
    return 0;
}