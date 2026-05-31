OUTPUT_ARCH(aarch64)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
    dynamic PT_DYNAMIC FLAGS(6);
}

SECTIONS
{
    . = 0;

    .text ALIGN(0x1000) : {
        *(.text.start)
        *(.text*)
    } :text

    .rodata ALIGN(0x1000) : {
        *(.rodata*)
        *(.dynsym)
        *(.dynstr)
        *(.gnu.hash)
        *(.rela.dyn)
    } :text

    .data ALIGN(0x1000) : {
        *(.dynamic)
        *(.got)
        *(.got.plt)
        *(.data*)
    } :data :dynamic

    .bss ALIGN(0x1000) (NOLOAD) : {
        *(.bss*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.comment)
        *(.interp)
        *(.eh_frame*)
        *(.note*)
    }
}