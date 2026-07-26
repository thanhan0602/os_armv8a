#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <unistd.h>
#include <stddef.h>

pthread_mutex_t lock;
static sem_t test_sem;
static pthread_mutex_t cond_lock;
static pthread_cond_t cond;
static int cond_ready;

static void *worker_semaphore(void *arg)
{
    int id = *(int *)arg;

    if (sem_wait(&test_sem) != 0) {
        printf("[Semaphore %d] wait failed\n", id);
        return (void *)1;
    }
    printf("[Semaphore %d] acquired\n", id);
    user_nanosleep(1);
    if (sem_post(&test_sem) != 0) {
        printf("[Semaphore %d] post failed\n", id);
        return (void *)1;
    }
    return NULL;
}

static void *worker_condition(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&cond_lock);
    while (!cond_ready) {
        if (pthread_cond_wait(&cond, &cond_lock) != 0) {
            pthread_mutex_unlock(&cond_lock);
            return (void *)1;
        }
    }
    pthread_mutex_unlock(&cond_lock);
    printf("[Condition] waiter resumed\n");
    return NULL;
}

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

    if (sem_init(&test_sem, 0, 1) != 0) {
        printf("Semaphore init failed.\n");
        return 1;
    }
    pthread_create(&t1, NULL, worker_semaphore, &id1);
    pthread_create(&t2, NULL, worker_semaphore, &id2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    if (sem_destroy(&test_sem) != 0) {
        printf("Semaphore destroy failed.\n");
        return 1;
    }
    printf("Semaphore Test Finished.\n");

    cond_ready = 0;
    pthread_mutex_init(&cond_lock, NULL);
    pthread_cond_init(&cond, NULL);
    pthread_create(&t1, NULL, worker_condition, NULL);
    pthread_yield();
    pthread_mutex_lock(&cond_lock);
    cond_ready = 1;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&cond_lock);
    pthread_join(t1, NULL);
    pthread_cond_destroy(&cond);
    pthread_mutex_destroy(&cond_lock);
    printf("Condition Variable Test Finished.\n");

    printf("Complex Test Finished.\n");

    return 0;
}
