.section .text.boot
.global _start
.type _start, %function

_start:
    mrs x0, mpidr_el1
    and x0, x0, #0xff
    cbz x0, 0f

1:
    wfe
    b 1b

0:
    ldr x0, =__stack_top
    mov sp, x0

    ldr x0, =__bss_start
    ldr x1, =__bss_end
    mov x2, xzr

2:
    cmp x0, x1
    b.hs 3f
    str x2, [x0], #8
    b 2b

3:
    // Enable FP/SIMD for EL1
    mrs x0, cpacr_el1
    orr x0, x0, #(3 << 20)
    msr cpacr_el1, x0
    isb

    bl kernel_main

4:
    wfe
    b 4b
