#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    int pid = getpid();
    int iterations = 10;
    
    // printf("[cpu-stress:%d] Starting multi-core stress service on CPU %d\n", pid, getcpu());
    
    for (int i = 0; i < iterations; i++) {
        // printf("[cpu-stress:%d] Working on CPU %d... (iteration %d/%d)\n", pid, getcpu(), i + 1, iterations);
        
        /* Use the new sleep syscall instead of a busy loop */
        sleep(1);
    }
    
    // printf("[cpu-stress:%d] Service complete. Exiting.\n", pid);
    return 0;
}
