#ifndef KERNEL_SYSCALL_H
#define KERNEL_SYSCALL_H

#include <kernel/exception.h>

/*
 * Syscall numbers matching the Linux AArch64 ABI where practical.
 * The caller places the number in x8 before executing svc #0.
 */
#define SYS_WRITE   64UL
#define SYS_EXIT    93UL
#define SYS_YIELD   124UL
#define SYS_BRK     214UL

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
