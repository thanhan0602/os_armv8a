#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

#define USER_SYS_WRITE  64UL
#define USER_SYS_EXIT   93UL
#define USER_SYS_YIELD  124UL
#define USER_SYS_GETPID 172UL
#define USER_SYS_GETCPU 168UL
#define USER_SYS_BRK    214UL
#define USER_SYS_MUNMAP 215UL
#define USER_SYS_MMAP   222UL
#define USER_SYS_FORK   220UL
#define USER_SYS_CLONE  224UL
#define USER_SYS_THREAD_CREATE 223UL
#define USER_SYS_EXECVE 221UL
#define USER_SYS_WAIT4  260UL
#define USER_SYS_NANOSLEEP 101UL
#define USER_SYS_IPC_SEND  451UL
#define USER_SYS_IPC_RECV  452UL
#define USER_SYS_MUTEX_INIT    502UL
#define USER_SYS_MUTEX_DESTROY 503UL
#define USER_SYS_MUTEX_LOCK   500UL
#define USER_SYS_MUTEX_UNLOCK 501UL
#define USER_SYS_MUTEX_TRYLOCK 504UL

#define USER_IPC_MESSAGE_MAX  64UL

/* Clone flags */
#define CLONE_VM        0x00000100
#define CLONE_FS        0x00000200
#define CLONE_FILES     0x00000400
#define CLONE_SIGHAND   0x00000800
#define CLONE_THREAD    0x00010000
#define CLONE_SETTLS    0x00080000

static inline long user_syscall0(unsigned long nr)
{
    register unsigned long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0");
    __asm__ volatile("svc #0" : "=r"(x0) : "r"(x8) : "x1","x2","x3","x4","x5","x6","x7","memory");
    return x0;
}

static inline long user_syscall1(unsigned long nr, unsigned long arg0)
{
    register unsigned long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = (long)arg0;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "x1","x2","x3","x4","x5","x6","x7","memory");
    return x0;
}

static inline long user_syscall2(unsigned long nr, unsigned long arg0, unsigned long arg1)
{
    register unsigned long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = (long)arg0;
    register unsigned long x1 __asm__("x1") = arg1;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "x2","x3","x4","x5","x6","x7","memory");
    return x0;
}

static inline long user_syscall3(unsigned long nr, unsigned long arg0, unsigned long arg1, unsigned long arg2)
{
    register unsigned long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = (long)arg0;
    register unsigned long x1 __asm__("x1") = arg1;
    register unsigned long x2 __asm__("x2") = arg2;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "x3","x4","x5","x6","x7","memory");
    return x0;
}

static inline long user_syscall5(unsigned long nr, unsigned long arg0, unsigned long arg1, unsigned long arg2, unsigned long arg3, unsigned long arg4)
{
    register unsigned long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = (long)arg0;
    register unsigned long x1 __asm__("x1") = arg1;
    register unsigned long x2 __asm__("x2") = arg2;
    register unsigned long x3 __asm__("x3") = arg3;
    register unsigned long x4 __asm__("x4") = arg4;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4) : "x5","x6","x7","memory");
    return x0;
}

static inline long user_syscall6(unsigned long nr, unsigned long arg0, unsigned long arg1,
                                 unsigned long arg2, unsigned long arg3, unsigned long arg4,
                                 unsigned long arg5)
{
    register unsigned long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = (long)arg0;
    register unsigned long x1 __asm__("x1") = arg1;
    register unsigned long x2 __asm__("x2") = arg2;
    register unsigned long x3 __asm__("x3") = arg3;
    register unsigned long x4 __asm__("x4") = arg4;
    register unsigned long x5 __asm__("x5") = arg5;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "x6","x7","memory");
    return x0;
}

static inline long user_write(unsigned long fd, const void *buf, unsigned long len)
{
    return user_syscall3(USER_SYS_WRITE, fd, (unsigned long)buf, len);
}

static inline long user_exit(int status)
{
    return user_syscall1(USER_SYS_EXIT, (unsigned long)status);
}

static inline long user_yield(void)
{
    return user_syscall0(USER_SYS_YIELD);
}

static inline long user_getpid(void)
{
    return user_syscall0(USER_SYS_GETPID);
}

static inline long user_getcpu(void)
{
    return user_syscall0(USER_SYS_GETCPU);
}

static inline long user_clone(unsigned long flags, void *stack, void *tls)
{
    /* aarch64 syscall clone: x0=flags, x1=stack, x2=parent_tidptr, x3=tls, x4=child_tidptr */
    return user_syscall5(USER_SYS_CLONE, flags, (unsigned long)stack, 0, (unsigned long)tls, 0);
}

static inline long user_fork(void)
{
    return user_syscall0(USER_SYS_FORK);
}

static inline int user_thread_create(void* entry, void* arg, void* stack)
{
    return (int)user_syscall3(USER_SYS_THREAD_CREATE, (unsigned long)entry, (unsigned long)arg, (unsigned long)stack);
}

static inline long user_wait4(long pid, int *status)
{
    return user_syscall2(USER_SYS_WAIT4, (unsigned long)pid, (unsigned long)status);
}

static inline long user_execve(const char *filename, char *const argv[], char *const envp[])
{
    return user_syscall3(USER_SYS_EXECVE, (unsigned long)filename, (unsigned long)argv, (unsigned long)envp);
}

static inline unsigned long user_brk(unsigned long new_break)
{
    return (unsigned long)user_syscall1(USER_SYS_BRK, new_break);
}

static inline long user_nanosleep(unsigned long ticks)
{
    return user_syscall1(USER_SYS_NANOSLEEP, ticks);
}

static inline long user_ipc_send(unsigned long tid, const void *msg, unsigned long len)
{
    return user_syscall3(USER_SYS_IPC_SEND, tid, (unsigned long)msg, len);
}

static inline long user_ipc_recv(unsigned long *tid_out, void *msg_out, unsigned long max_len)
{
    return user_syscall3(USER_SYS_IPC_RECV, (unsigned long)tid_out, (unsigned long)msg_out, max_len);
}

static inline int user_mutex_init(void) { return (int)user_syscall0(USER_SYS_MUTEX_INIT); }
static inline void user_mutex_destroy(int id) { user_syscall1(USER_SYS_MUTEX_DESTROY, (unsigned long)id); }
static inline void user_mutex_lock(int id) { user_syscall1(USER_SYS_MUTEX_LOCK, (unsigned long)id); }
static inline void user_mutex_unlock(int id) { user_syscall1(USER_SYS_MUTEX_UNLOCK, (unsigned long)id); }
static inline int user_mutex_trylock(int id) { return (int)user_syscall1(USER_SYS_MUTEX_TRYLOCK, (unsigned long)id); }

static inline void *user_mmap(void *addr, unsigned long length, int prot, int flags, int fd, unsigned long offset)
{
    return (void *)user_syscall6(USER_SYS_MMAP, (unsigned long)addr, length, (unsigned long)prot, (unsigned long)flags, (unsigned long)fd, offset);
}

/*
 * Low-level thread creation trampoline (implemented in libc/clone.c).
 * Robust: the child runs fn(arg) on its own stack via an assembly trampoline
 * rather than relying on the C ABI across the clone boundary.
 */
extern long __clone(unsigned long flags, void *child_stack,
                    void *(*fn)(void *), void *arg, void *tls);

/* Read the EL0 thread pointer (TPIDR_EL0) — base of the current TLS block. */
static inline void *user_get_tls(void)
{
    void *tp;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    return tp;
}

#endif
