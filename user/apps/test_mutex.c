#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

pthread_mutex_t shared_lock;
int shared_resource = 0;

int main(int argc, char **argv)
{
    pthread_mutex_init(&shared_lock, NULL);

    printf("User-space Mutex Test Starting...\n");

    // In this OS, fork() is not fully POSIX but let's assume multiple processes 
    // or kernel threads mapped to EL0. 
    // Since we don't have pthread_create yet, we test basic locking logic
    // or use fork if available.

    pthread_mutex_lock(&shared_lock);
    printf("Process locked mutex. Resource: %d\n", shared_resource);
    shared_resource++;
    printf("Incremented resource: %d\n", shared_resource);
    pthread_mutex_unlock(&shared_lock);

    printf("Unlocked mutex. Success.\n");

    return 0;
}
