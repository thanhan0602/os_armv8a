.section .text.boot
.global _start
.global secondary_entry
.type _start, %function
.type secondary_entry, %function

_start:
    mov x9, x0
    mrs x0, mpidr_el1
    and x0, x0, #0xff
    cbz x0, 0f

secondary_entry:
    /* Secondary CPU path */
    adrp x0, __stack_top
    add  x0, x0, :lo12:__stack_top
    
    /* Give each CPU a separate 16KB stack */
    mrs x1, mpidr_el1
    and x1, x1, #0xff
    lsl x1, x1, #14  /* CPU_ID * 16KB */
    sub x0, x0, x1
    mov sp, x0

    /* Enable FP/SIMD */
    mrs x0, cpacr_el1
    orr x0, x0, #(3 << 20)
    msr cpacr_el1, x0
    isb

    bl secondary_main_early
    
    /* 
     * Now MMU is ON for this core.
     * Switch to high VA space.
     */
    ldr x9, =__kernel_va_offset

    mov x0, sp
    add x0, x0, x9
    mov sp, x0

    ldr x0, =secondary_main
    br  x0
    
    /* Should not return */
1:
    wfe
    b 1b

0:
    /*
     * Running at physical address with MMU off.
     * Use adrp/add to compute the PA of symbols even if linked at VA.
     */
    adrp x0, __stack_top
    add  x0, x0, :lo12:__stack_top
    mov  sp, x0

    adrp x0, __bss_start
    add  x0, x0, :lo12:__bss_start
    adrp x1, __bss_end
    add  x1, x1, :lo12:__bss_end
    mov  x2, xzr

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

    mov x0, x9
    bl kernel_main_early

#ifdef CONFIG_KERNEL_VIRTUAL
    /*
     * kernel_main_early has returned.  The MMU is now on with:
     *   TTBR0_EL1 -> identity map  (VA == PA)
     *   TTBR1_EL1 -> kernel VA map (VA == PA + KERNEL_VA_OFFSET)
     *
     * Switch execution to the kernel virtual address space:
     * 1. Move SP from PA to its corresponding VA
     * 2. Branch to the high VA — TTBR1 takes over
     */
    ldr x9, =__kernel_va_offset

    mov x0, sp
    add x0, x0, x9
    mov sp, x0

    ldr x0, =kernel_main
    br  x0
#else
    bl kernel_main
#endif

4:
    wfe
    b 4b
