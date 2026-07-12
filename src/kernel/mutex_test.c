#include <kernel/mutex.h>
#include <kernel/sched.h>
#include <kernel/log.h>

static struct mutex test_mutex;
static volatile int test_value = 0;

static void mutex_test_task(void)
{
    const char *name = sched_current()->name;
    for (int i = 0; i < 5; i++) {
        mutex_lock(&test_mutex);
        KER_LOGF("[%s] acquired mutex, value = %d\n", name, test_value);
        int local = test_value;
        
        /* Deliberate delay to encourage race conditions if mutex fails */
        for (volatile int j = 0; j < 500000; j++);
        
        test_value = local + 1;
        KER_LOGF("[%s] releasing mutex, new value = %d\n", name, test_value);
        mutex_unlock(&test_mutex);
        
        /* Delay outside lock to give other task a chance */
        for (volatile int j = 0; j < 500000; j++);
    }
    KER_LOGF("[%s] done\n", name);
    task_exit();
}

void mutex_run_test(void)
{
    mutex_init(&test_mutex);
    test_value = 0;
    KER_INFO("Starting mutex test tasks...");
    task_create(mutex_test_task, "mutex-1");
    task_create(mutex_test_task, "mutex-2");
}
