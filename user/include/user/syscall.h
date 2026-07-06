#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

#define USER_SYS_WRITE  64UL
#define USER_SYS_EXIT   93UL
#define USER_SYS_YIELD  124UL
#define USER_SYS_GETPID 172UL
#define USER_SYS_GETCPU 168UL
#define USER_SYS_BRK    214UL
#define USER_SYS_MUNMAP 215UL
#define USER_SYS_FORK   220UL
#define USER_SYS_WAIT4  260UL
#define USER_SYS_NANOSLEEP 101UL
#define USER_SYS_IPC_SEND  451UL
#define USER_SYS_IPC_RECV  452UL

#define USER_IPC_MESSAGE_MAX  64UL

static inline long user_syscall0(unsigned long nr)
{
    register unsigned long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0");

    __asm__ volatile(
        "svc #0"
        : "=r"(x0)
        : "r"(x8)
        : "memory");

    return x0;
}

static inline long user_syscall1(unsigned long nr, unsigned long arg0)
{
    register unsigned long x8 __asm__("x8") = nr;
    register unsigned long x0 __asm__("x0") = arg0;

    __asm__ volatile(
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "memory");

    return (long)x0;
}

static inline long user_syscall2(unsigned long nr, unsigned long arg0, unsigned long arg1)
{
    register unsigned long x8 __asm__("x8") = nr;
    register unsigned long x0 __asm__("x0") = arg0;
    register unsigned long x1 __asm__("x1") = arg1;

    __asm__ volatile(
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory");

    return (long)x0;
}

static inline long user_syscall3(unsigned long nr,
                                 unsigned long arg0,
                                 unsigned long arg1,
                                 unsigned long arg2)
{
    register unsigned long x8 __asm__("x8") = nr;
    register unsigned long x0 __asm__("x0") = arg0;
    register unsigned long x1 __asm__("x1") = arg1;
    register unsigned long x2 __asm__("x2") = arg2;

    __asm__ volatile(
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory");

    return (long)x0;
}

static inline long user_write(unsigned long fd, const void *buf, unsigned long len)
{
    return user_syscall3(USER_SYS_WRITE, fd, (unsigned long)buf, len);
}

static inline long user_yield(void)
{
    return user_syscall0(USER_SYS_YIELD);
}

static inline long user_exit(unsigned long code)
{
    return user_syscall1(USER_SYS_EXIT, code);
}

static inline unsigned long user_brk(unsigned long new_break)
{
    return (unsigned long)user_syscall1(USER_SYS_BRK, new_break);
}

static inline long user_munmap(void *addr, unsigned long len)
{
    return user_syscall2(USER_SYS_MUNMAP, (unsigned long)addr, len);
}
static inline long user_getpid(void)
{
    return user_syscall0(USER_SYS_GETPID);
}

static inline long user_getcpu(void)
{
    return user_syscall0(USER_SYS_GETCPU);
}

static inline long user_fork(void)
{
    return user_syscall0(USER_SYS_FORK);
}

static inline long user_wait4(long pid, int *status)
{
    return user_syscall2(USER_SYS_WAIT4, (unsigned long)pid, (unsigned long)status);
}

static inline long user_ipc_send(unsigned long channel_id, const void *buf, unsigned long len)
{
    return user_syscall3(USER_SYS_IPC_SEND, channel_id, (unsigned long)buf, len);
}

static inline long user_ipc_recv(unsigned long channel_id, void *buf, unsigned long capacity)
{
    return user_syscall3(USER_SYS_IPC_RECV, channel_id, (unsigned long)buf, capacity);
}

static inline unsigned long user_strlen(const char *text)
{
    unsigned long length;

    length = 0UL;
    while (text[length] != '\0') {
        length++;
    }

    return length;
}

static inline void user_write_string(const char *text)
{
    user_write(1UL, text, user_strlen(text));
}

static inline unsigned int user_nanosleep(unsigned int seconds)
{
    return (unsigned int)user_syscall1(USER_SYS_NANOSLEEP, (unsigned long)seconds);
}

#endif