#ifndef _UNISTD_H
#define _UNISTD_H

long write(int fd, const void *buf, unsigned long count);
void _exit(int status);
long yield(void);
int getpid(void);
int getcpu(void);
void *sbrk(long increment);
int fork(void);
int execve(const char *filename, char *const argv[], char *const envp[]);
int wait(int *status);
unsigned int sleep(unsigned int seconds);

/* Non-standard but available in this system */
long ipc_send(unsigned long channel_id, const void *buf, unsigned long len);
long ipc_recv(unsigned long channel_id, void *buf, unsigned long capacity);

#endif
