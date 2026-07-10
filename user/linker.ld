OUTPUT_ARCH(aarch64)
ENTRY(_start)

PHDRS
{
    phdr     PT_PHDR PHDRS;    /* ELF PHDR table - extends first PT_LOAD to offset 0 */
    interp   PT_INTERP;
    text     PT_LOAD FLAGS(5) FILEHDR PHDRS; /* R E - covers ELF header + PHDRs */
    data     PT_LOAD FLAGS(6); /* RW  */
    dynamic  PT_DYNAMIC;
}

SECTIONS
{
    /*
     * SIZEOF_HEADERS places the first section after the ELF header + PHDR table.
     * Combined with PT_PHDR PHDRS before the first PT_LOAD, the linker extends
     * the text PT_LOAD to file offset 0, making AT_PHDR accessible at runtime.
     */
    . = SIZEOF_HEADERS;

    .interp : {
        KEEP(*(.interp))
    } :text :interp

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
