#ifndef LD_SYSCALLS_H
#define LD_SYSCALLS_H

static inline long ld_write(int fd, const void *buf, unsigned long count) {
    long ret;
    asm volatile (
        "mov x0, %1\n"
        "mov x1, %2\n"
        "mov x2, %3\n"
        "mov x8, #64\n"
        "svc #0\n"
        "mov %0, x0\n"
        : "=r"(ret) : "r"((long)fd), "r"(buf), "r"(count) : "x0", "x1", "x2", "x8", "memory"
    );
    return ret;
}

static inline void ld_puts(const char *s) {
    unsigned long len = 0;
    while (s[len]) len++;
    ld_write(1, s, len);
}

static inline long ld_open(const char *path) {
    long ret;
    asm volatile (
        "mov x0, %1\n"
        "mov x8, #56\n"
        "svc #0\n"
        "mov %0, x0\n"
        : "=r"(ret) : "r"(path) : "x0", "x8", "memory"
    );
    return ret;
}

static inline long ld_read(int fd, void *buf, unsigned long count) {
    long ret;
    asm volatile (
        "mov x0, %1\n"
        "mov x1, %2\n"
        "mov x2, %3\n"
        "mov x8, #63\n"
        "svc #0\n"
        "mov %0, x0\n"
        : "=r"(ret) : "r"((long)fd), "r"(buf), "r"(count) : "x0", "x1", "x2", "x8", "memory"
    );
    return ret;
}

static inline void *ld_mmap(void *addr, unsigned long len, int prot, int flags, int fd, unsigned long offset) {
    void *ret;
    asm volatile (
        "mov x0, %1\n"
        "mov x1, %2\n"
        "mov x2, %3\n"
        "mov x3, %4\n"
        "mov x4, %5\n"
        "mov x5, %6\n"
        "mov x8, #222\n"
        "svc #0\n"
        "mov %0, x0\n"
        : "=r"(ret) : "r"(addr), "r"(len), "r"((long)prot), "r"((long)flags), "r"((long)fd), "r"(offset) : "x0", "x1", "x2", "x3", "x4", "x5", "x8", "memory"
    );
    return ret;
}

static inline void ld_exit(int code) {
    asm volatile (
        "mov x0, %0\n"
        "mov x8, #93\n"
        "svc #0\n"
        : : "r"((long)code) : "x0", "x8"
    );
}

static inline int ld_strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}
static inline int ld_strncmp(const char *s1, const char *s2, unsigned long n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++; s2++; n--;
    }
    if (n == 0) return 0;
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

static inline unsigned long ld_strlen(const char *s) {
    unsigned long len = 0;
    while (s[len]) len++;
    return len;
}
static inline void ld_strcpy(char *dest, const char *src) {
    while ((*dest++ = *src++));
}

#endif
