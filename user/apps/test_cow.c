#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

/* Data in .data section to test CoW */
static unsigned long shared_data = 0x12345678;

int main(void) {
    printf("[cow:%d] --- CoW Test Start ---\n", getpid());
    printf("[cow:%d] Initial shared_data = 0x%lx at 0x%lx\n", getpid(), shared_data, (unsigned long)&shared_data);

    long pid = fork();

    if (pid < 0) {
        printf("[cow:%d] Fork failed!\n", getpid());
        exit(1);
    } else if (pid == 0) {
        /* Child */
        printf("[cow:%d] Child: Updating shared_data to 0xdeadbeef\n", getpid());
        shared_data = 0xdeadbeef;
        printf("[cow:%d] Child: shared_data = 0x%lx\n", getpid(), shared_data);
        exit(0);
    } else {
        /* Parent */
        printf("[cow:%d] Parent: Waiting for child PID=%ld\n", getpid(), pid);
        
        int status = -1;
        long waited_pid = wait(&status);
        
        printf("[cow:%d] Parent: Child %ld exited with status %d\n", getpid(), waited_pid, status);

        printf("[cow:%d] Parent: Checking shared_data...\n", getpid());
        printf("[cow:%d] Parent: shared_data = 0x%lx\n", getpid(), shared_data);

        if (shared_data == 0x12345678) {
            printf("[cow:%d] CoW SUCCESS: Parent data unchanged!\n", getpid());
        } else {
            printf("[cow:%d] CoW FAILURE: Parent data corrupted!\n", getpid());
        }
    }

    exit(0);
    return 0;
}
