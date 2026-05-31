#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

#define USER_SYS_WRITE  64UL
#define USER_SYS_EXIT   93UL
#define USER_SYS_YIELD  124UL
#define USER_SYS_BRK    214UL

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

#endif