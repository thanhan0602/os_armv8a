#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <stddef.h>

pthread_mutex_t lock;

void *worker_blocking(void *arg)
{
    int id = *(int *)arg;
    for (int i = 0; i < 2; i++) {
        printf("[Thread %d] Waiting for lock...\n", id);
        pthread_mutex_lock(&lock);
        printf("[Thread %d] Acquired lock. Working...\n", id);
        
        user_nanosleep(1); // Sleep 1 second
        
        printf("[Thread %d] Releasing lock.\n", id);
        pthread_mutex_unlock(&lock);
        pthread_yield();
    }
    return NULL;
}

void *worker_trylock(void *arg)
{
    int id = *(int *)arg;
    int success_count = 0;
    while (success_count < 2) {
        if (pthread_mutex_trylock(&lock) == 0) {
            printf("[Thread %d] Trylock SUCCESS! Working...\n", id);
            user_nanosleep(1);
            printf("[Thread %d] Releasing lock.\n", id);
            pthread_mutex_unlock(&lock);
            success_count++;
        } else {
            printf("[Thread %d] Trylock BUSY... retrying later.\n", id);
            user_nanosleep(1);
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    pthread_t t1, t2, t3;
    int id1 = 1, id2 = 2, id3 = 3;

    printf("Starting Pthread-like Complex Test (SMP Synchronization)\n");

    pthread_mutex_init(&lock, NULL);

    pthread_create(&t1, NULL, worker_blocking, &id1);
    pthread_yield();
    pthread_create(&t2, NULL, worker_blocking, &id2);
    pthread_yield();
    pthread_create(&t3, NULL, worker_trylock, &id3);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);

    pthread_mutex_destroy(&lock);
    printf("Complex Test Finished.\n");

    return 0;
}
