OUTPUT_ARCH(aarch64)
ENTRY(_start)

BOOT_STACK_SIZE = 0x4000;
KERNEL_PAGE_SIZE = 0x1000;

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x40080000;
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

    .boot_stack (NOLOAD) : ALIGN(KERNEL_PAGE_SIZE) {
        __stack_bottom = .;
        . += BOOT_STACK_SIZE;
        __stack_top = .;
    } :data

    . = ALIGN(KERNEL_PAGE_SIZE);
    __kernel_end = .;
}