#include <stdlib.h>
#include <unistd.h>

void exit(int status)
{
    _exit(status);
}

/* Very primitive malloc for now */
void *malloc(unsigned long size)
{
    if (size == 0) return NULL;
    
    /* Align to 8 bytes */
    size = (size + 7) & ~7UL;
    
    void *ptr = sbrk(size);
    if (ptr == (void *)-1) {
        return NULL;
    }
    return ptr;
}

void free(void *ptr)
{
    /* No-op in this primitive implementation */
    (void)ptr;
}
