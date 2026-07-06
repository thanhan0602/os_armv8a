OUTPUT_ARCH(aarch64)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5); /* R E */
    data PT_LOAD FLAGS(6); /* RW  */
    dynamic PT_DYNAMIC;
}

SECTIONS
{
    . = 0x0;

    .text ALIGN(0x1000) : {
        *(.text.start)
        *(.text*)
    } :text

    .plt ALIGN(8) : {
        *(.plt)
    } :text

    .rodata ALIGN(0x1000) : {
        *(.rodata*)
    } :text

    .dynsym ALIGN(8) : { *(.dynsym) } :text
    .dynstr ALIGN(8) : { *(.dynstr) } :text
    .hash ALIGN(8) : { *(.hash) } :text
    .gnu.hash ALIGN(8) : { *(.gnu.hash) } :text
    .rela.dyn ALIGN(8) : { *(.rela*) } :text

    .data ALIGN(0x1000) : {
        *(.data*)
    } :data

    .dynamic ALIGN(8) : {
        *(.dynamic)
    } :data :dynamic

    .got ALIGN(8) : {
        *(.got)
        *(.got.plt)
    } :data

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
