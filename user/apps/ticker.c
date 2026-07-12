#include <stdio.h>
#include <unistd.h>

int main(void)
{
    for (;;) {
        // printf("[user-ticker] tick\n");
        yield();
    }
}