#include <unistd.h>
#include <user/syscall.h>

long write(int fd, const void *buf, unsigned long count)
{
    return user_write((unsigned long)fd, buf, count);
}

void _exit(int status)
{
    user_exit((unsigned long)status);
    while (1);
}

long yield(void)
{
    return user_yield();
}

int getpid(void)
{
    return (int)user_getpid();
}

int getcpu(void)
{
    return (int)user_getcpu();
}

void *sbrk(long increment)
{
    static unsigned long current_brk = 0;
    if (current_brk == 0) {
        current_brk = user_brk(0);
    }
    
    unsigned long old_brk = current_brk;
    unsigned long new_brk = old_brk + increment;
    
    unsigned long ret = user_brk(new_brk);
    if (ret < new_brk && increment > 0) {
        return (void *)-1;
    }
    
    current_brk = ret;
    return (void *)old_brk;
}

int fork(void)
{
    return (int)user_fork();
}

int execve(const char *filename, char *const argv[], char *const envp[])
{
    return (int)user_execve(filename, argv, envp);
}

int wait(int *status)
{
    return (int)user_wait4(-1, status);
}

long ipc_send(unsigned long channel_id, const void *buf, unsigned long len)
{
    return user_ipc_send(channel_id, buf, len);
}

long ipc_recv(unsigned long channel_id, void *buf, unsigned long capacity)
{
    return user_ipc_recv(channel_id, buf, capacity);
}

unsigned int sleep(unsigned int seconds)
{
    return user_nanosleep(seconds);
}
