OUTPUT_ARCH(aarch64)
ENTRY(_start)

BOOT_STACK_SIZE  = 0x4000;
KERNEL_PAGE_SIZE = 0x1000;

/*
 * Physical load address: where QEMU places the raw binary in RAM and
 * where _start begins executing before the MMU is enabled.
 *
 * Virtual address offset: the fixed delta added to every PA to obtain
 * the corresponding kernel VA once TTBR1_EL1 is active (48-bit VA,
 * T1SZ=16).  Must match KERNEL_VA_OFFSET in src/include/kernel/vm.h.
 */
KERNEL_LMA_BASE  = 0x40080000;
KERNEL_VA_OFFSET = 0xFFFF000000000000;
KERNEL_VMA_BASE  = KERNEL_LMA_BASE + KERNEL_VA_OFFSET;

/*
 * Export the offset as a linker symbol so assembly code can load it
 * as an immediate via  ldr xN, =__kernel_va_offset.
 * The symbol's VALUE is the offset itself (not a pointer to it).
 */
__kernel_va_offset = KERNEL_VA_OFFSET;

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    /*
     * VMA stays at the physical load address so that initialised pointer
     * data (.rodata struct arrays with const char* members, function
     * pointer tables, etc.) contain PAs that can be safely dereferenced
     * before the MMU is turned on.
     *
     * The VA trampoline in start.S computes the high kernel VA at
     * runtime: VA = PA + KERNEL_VA_OFFSET.  Once the full kernel virtual
     * layout is mature, a VMA=VA linker split (with AT() for LMA) can
     * replace this scheme; that requires moving page-table setup into
     * assembly so the MMU is active before any C pointer-in-data access.
     */
    . = KERNEL_LMA_BASE;
    __kernel_start = .;

    .text : ALIGN(KERNEL_PAGE_SIZE) {
        __text_start = .;
        KEEP(*(.text.boot))   /* entry point */
        . = ALIGN(2048);
        KEEP(*(.text.vectors))
        *(.text*)
        __text_end = .;
    } :text

    .rodata : ALIGN(KERNEL_PAGE_SIZE) {
        __rodata_start = .;
        *(.rodata*)
        __rodata_end = .;
    } :text

    .data : ALIGN(KERNEL_PAGE_SIZE) {
        __data_start = .;
        *(.data*)
        __data_end = .;
    } :data

    .bss (NOLOAD) : ALIGN(KERNEL_PAGE_SIZE) {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(KERNEL_PAGE_SIZE);
        __bss_end = .;
    } :data

    .stack_guard (NOLOAD) : ALIGN(KERNEL_PAGE_SIZE) {
        __stack_guard = .;
        . += KERNEL_PAGE_SIZE;
        __stack_guard_end = .;
    } :data

    .boot_stack (NOLOAD) : ALIGN(KERNEL_PAGE_SIZE) {
        __stack_bottom = .;
        . += BOOT_STACK_SIZE;
        __stack_top = .;
    } :data

    . = ALIGN(KERNEL_PAGE_SIZE);
    __kernel_end = .;
}