#ifndef KERNEL_VM_H
#define KERNEL_VM_H

/*
 * Kernel virtual memory layout constants.
 *
 * The kernel uses TTBR1_EL1 for its own virtual address space.
 * TCR_EL1.T1SZ = 16 gives a 48-bit virtual address space starting at
 * 0xFFFF000000000000.
 *
 * The entire physical address space is linearly mapped into the kernel
 * virtual space via a fixed offset:
 *
 *   VA = PA + KERNEL_VA_OFFSET
 *   PA = VA - KERNEL_VA_OFFSET
 *
 * This means every physical address has exactly one kernel VA, and
 * converting between the two is a single add or subtract.
 *
 * KERNEL_VA_OFFSET is chosen so that:
 *   - PA 0x40080000 (kernel load address) maps to VA 0xFFFF000040080000
 *   - PA 0x09000000 (PL011 UART)          maps to VA 0xFFFF000009000000
 *   - PA 0x08000000 (GICv2 GICD)          maps to VA 0xFFFF000008000000
 */
#ifdef CONFIG_KERNEL_VIRTUAL
#define KERNEL_VA_OFFSET  0xFFFF000000000000UL
#else
#define KERNEL_VA_OFFSET  0UL
#endif

/*
 * Convert a physical address (or a pointer derived from one) to the
 * corresponding kernel virtual address.  The result is a void * so it
 * can be assigned to any pointer type without an explicit cast at the
 * call site.
 */
#define pa_to_va(pa)  ((void *)((unsigned long)(pa) + KERNEL_VA_OFFSET))

/*
 * Convert a kernel virtual address (pointer) back to the underlying
 * physical address.  Returns an unsigned long, not a pointer.
 */
#define va_to_pa(va)  ((unsigned long)(va) - KERNEL_VA_OFFSET)

#endif
