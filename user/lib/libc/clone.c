/*
 * __clone — low-level thread/process creation trampoline (glibc-style).
 *
 * long __clone(unsigned long flags, void *child_stack,
 *              void *(*fn)(void *), void *arg, void *tls);
 *
 * The child is started on its own stack. Unlike the fragile "fork-style"
 * approach (returning through C on a fresh stack), this trampoline is pure
 * assembly: it stores fn/arg on the child stack BEFORE the syscall and,
 * in the child, loads them from its own stack and calls fn(arg) directly,
 * never relying on the C calling convention across the clone boundary.
 *
 * Register usage on entry:
 *   x0 = flags, x1 = child_stack, x2 = fn, x3 = arg, x4 = tls
 *
 * clone syscall ABI (AArch64): x0=flags, x1=child_stack,
 *   x2=parent_tidptr, x3=tls, x4=child_tidptr, x8=SYS_CLONE(224)
 */
__asm__(
    ".section .text\n"
    ".global __clone\n"
    ".type __clone, %function\n"
    "__clone:\n"
    "    bic  x1, x1, #15\n"          /* 16-byte align the child stack */
    "    stp  x2, x3, [x1, #-16]!\n"  /* push fn (lo) and arg (hi); x1 -> them */
    "    mov  x3, x4\n"               /* x3 = tls  (syscall arg) */
    "    mov  x2, xzr\n"              /* x2 = parent_tidptr = 0 */
    "    mov  x4, xzr\n"              /* x4 = child_tidptr = 0 */
    "    mov  x8, #224\n"             /* SYS_CLONE */
    "    svc  #0\n"
    "    cbz  x0, 1f\n"               /* x0 == 0 -> child */
    "    ret\n"                       /* parent: return child tid in x0 */
    "1:\n"
    "    ldp  x1, x0, [sp], #16\n"    /* x1 = fn, x0 = arg */
    "    blr  x1\n"                   /* fn(arg); return value in x0 */
    "    mov  x8, #93\n"             /* SYS_EXIT */
    "    svc  #0\n"
    "    b    .\n"                    /* unreachable */
);
