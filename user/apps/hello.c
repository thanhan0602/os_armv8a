#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

int main(void)
{
    const char *hello = "hello";
    const char *heap_ok = "heap is working\n";
    unsigned long heap_base;
    unsigned long new_break;
    unsigned long index;

    for (index = 0UL; index < 3UL; index++) {
        int cpu = getcpu();
        printf("[hello:%d:cpu%d] hello from ELF\n", getpid(), cpu);
        yield();
    }

    heap_base = (unsigned long)sbrk(strlen(heap_ok));
    if (heap_base != (unsigned long)-1) {
        char *heap_text = (char *)heap_base;
        strcpy(heap_text, heap_ok);
        printf("%s", heap_text);
    }

    exit(0);
    return 0;
}