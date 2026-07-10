#include <stdio.h>
#include <unistd.h>

#ifndef NULL
#define NULL ((void*)0)
#endif

int main(void)
{
    printf("[test_exec] starting...\n");
    
    char *filename = "/bin/hello.elf";
    char *argv[] = {filename, NULL};
    char *envp[] = {NULL};

    printf("[test_exec] calling execve(\"%s\")...\n", filename);
    
    int ret = execve(filename, argv, envp);
    
    /* If execve returns, it failed */
    printf("[test_exec] execve failed with %d\n", ret);
    
    return 1;
}
