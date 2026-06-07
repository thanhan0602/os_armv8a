#include <user/syscall.h>
#include <user/shared.h>

/* Data in .data section to test CoW */
static unsigned long shared_data = 0x12345678;

int main(void) {
    printf("--- CoW Test Start ---\n");
    printf("Initial shared_data = 0x%lx at 0x%lx\n", shared_data, (unsigned long)&shared_data);

    long pid = user_fork();

    if (pid < 0) {
        printf("Fork failed!\n");
        user_exit(1);
    } else if (pid == 0) {
        /* Child */
        printf("Child: Updating shared_data to 0xdeadbeef\n");
        shared_data = 0xdeadbeef;
        printf("Child: shared_data = 0x%lx\n", shared_data);
        user_exit(0);
    } else {
        /* Parent */
        printf("Parent: Waiting for child PID=%ld\n", pid);
        
        int status = -1;
        long waited_pid = user_wait4(pid, &status);
        
        printf("Parent: Child %ld exited with status %d\n", waited_pid, status);

        printf("Parent: Checking shared_data...\n");
        printf("Parent: shared_data = 0x%lx\n", shared_data);

        if (shared_data == 0x12345678) {
            printf("CoW SUCCESS: Parent data unchanged!\n");
        } else {
            printf("CoW FAILURE: Parent data corrupted!\n");
        }
    }

    user_exit(0);
    return 0;
}
