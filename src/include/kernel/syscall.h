#ifndef KERNEL_SYSCALL_H
#define KERNEL_SYSCALL_H

#include <kernel/exception.h>

/*
 * Syscall numbers matching the Linux AArch64 ABI where practical.
 * The caller places the number in x8 before executing svc #0.
 */
#define SYS_WRITE   64UL
#define SYS_READ    63UL
#define SYS_OPEN    56UL
#define SYS_EXIT    93UL
#define SYS_YIELD   124UL
#define SYS_GETPID  172UL
#define SYS_GETCPU  168UL
#define SYS_BRK     214UL
#define SYS_MUNMAP  215UL
#define SYS_MMAP    222UL
#define SYS_FORK    220UL
#define SYS_EXECVE  221UL
#define SYS_WAIT4   260UL
#define SYS_NANOSLEEP 101UL
#define SYS_IPC_SEND  451UL
#define SYS_IPC_RECV  452UL

/*
 * Dispatch a syscall from EL0.
 *
 * nr   - syscall number (from x8 in the saved exception frame)
 * ctx  - pointer to the saved register frame on the EL1 stack; the
 *        handler writes the return value into ctx->gpr[0] so that
 *        restore_context loads it into x0 on eret back to EL0.
 */
void syscall_dispatch(unsigned long nr, struct exception_context *ctx);

#endif
