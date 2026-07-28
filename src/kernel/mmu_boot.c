#include <kernel/mmu.h>
#include <kernel/mmu_debug.h>
#include <kernel/page_alloc.h>
#include <kernel/log.h>
#include <kernel/vm.h>
#include <arch/arm/virt.h>

/*
 * Definitions for the boot-time identity map page tables.
 * These are exported so that mmu.c can use them to enable the MMU via TTBR0.
 */
unsigned long *mmu_l0_table;
unsigned long *mmu_l1_table;
unsigned long *mmu_l2_ram_table;
unsigned long mmu_fine_map_chunks_used;

/* TTBR1 page-table root pointers for the kernel virtual address space. */
unsigned long *l0_table_ttbr1;
unsigned long *l1_table_ttbr1;
unsigned long *l2_ram_table_ttbr1;

extern char __text_start[];
extern char __text_end[];
extern char __rodata_start[];
extern char __rodata_end[];
extern char __data_start[];
extern char __data_end[];
extern char __bss_start[];
extern char __bss_end[];
extern char __stack_bottom[];
extern char __stack_top[];
extern char __stack_guard[];
extern char __stack_guard_end[];
extern char __kernel_end[];

unsigned long *alloc_named_table_page(const char *name)
{
    unsigned long *table = (unsigned long *)page_alloc();
    if (table) mmu_debug_record_table_page((unsigned long)table, name);
    return table;
}

static void mmu_log_segment(const char *name, void *start, void *end)
{
    unsigned long k_start_pa = (unsigned long)va_to_pa(__text_start);
    unsigned long s_pa = (unsigned long)va_to_pa(start);
    unsigned long e_pa = (unsigned long)va_to_pa(end);
    unsigned long size = e_pa - s_pa;
    unsigned long start_page = (s_pa - k_start_pa) / PAGE_SIZE;
    unsigned long end_page = (e_pa - 1 - k_start_pa) / PAGE_SIZE;
    unsigned long start_chunk = (s_pa - QEMU_VIRT_RAM_BASE) / MMU_L2_BLOCK_SIZE;
    unsigned long end_chunk = (e_pa - 1 - QEMU_VIRT_RAM_BASE) / MMU_L2_BLOCK_SIZE;

    if (size == 0) return;

    KER_LOGF("[boot] Segment %s: %lx - %lx (%lu KB) | Kernel Pages: %lu-%lu | Chunks: %lu-%lu\n",
             name, s_pa, e_pa, size / 1024, start_page, end_page, start_chunk, end_chunk);
}

void mmu_log_kernel_layout(void)
{
    unsigned long k_start = (unsigned long)va_to_pa(__text_start);
    unsigned long k_end = (unsigned long)va_to_pa(__kernel_end);

    KER_LOGF("[boot] Kernel Physical Layout:\n");
    KER_LOGF("[boot] Total Size: %lu KB\n", (k_end - k_start) / 1024);

    mmu_log_segment(".text", __text_start, __text_end);
    mmu_log_segment(".rodata", __rodata_start, __rodata_end);
    mmu_log_segment(".data", __data_start, __data_end);
    mmu_log_segment(".bss", __bss_start, __bss_end);
    mmu_log_segment("guard", __stack_guard, __stack_guard_end);
    mmu_log_segment("stack", __stack_bottom, __stack_top);
}

/*
 * The fine-mapped kernel region is split by linker sections so we can keep
 * .text executable, .rodata read-only, and writable data non-executable.
 */
unsigned long mmu_kernel_page_attrs(unsigned long address)
{
    unsigned long pa = (unsigned long)va_to_pa(address);

    /* Guard page: intentionally unmapped to catch stack overflow. */
    if (pa >= (unsigned long)va_to_pa(__stack_guard) && pa < (unsigned long)va_to_pa(__stack_guard_end)) {
        return 0;
    }

    if (pa >= (unsigned long)va_to_pa(__text_start) && pa < (unsigned long)va_to_pa(__text_end)) {
        return MMU_ATTR_NORMAL |
               MMU_AP_RO |
               MMU_SH_INNER_SHAREABLE |
               MMU_AF;
    }

    if (pa >= (unsigned long)va_to_pa(__rodata_start) && pa < (unsigned long)va_to_pa(__rodata_end)) {
        return MMU_ATTR_NORMAL |
               MMU_AP_RO |
               MMU_SH_INNER_SHAREABLE |
               MMU_AF |
               MMU_PXN |
               MMU_UXN;
    }

    if (pa >= (unsigned long)va_to_pa(__data_start) && pa < (unsigned long)va_to_pa(__data_end)) {
        return MMU_ATTR_NORMAL |
               MMU_AP_RW |
               MMU_SH_INNER_SHAREABLE |
               MMU_AF |
               MMU_PXN |
               MMU_UXN;
    }

    if (pa >= (unsigned long)va_to_pa(__bss_start) && pa < (unsigned long)va_to_pa(__bss_end)) {
        return MMU_ATTR_NORMAL |
               MMU_AP_RW |
               MMU_SH_INNER_SHAREABLE |
               MMU_AF |
               MMU_PXN |
               MMU_UXN;
    }

    if (pa >= (unsigned long)va_to_pa(__stack_bottom) && pa < (unsigned long)va_to_pa(__stack_top)) {
        return MMU_ATTR_NORMAL |
               MMU_AP_RW |
               MMU_SH_INNER_SHAREABLE |
               MMU_AF |
               MMU_PXN |
               MMU_UXN;
    }

    return MMU_ATTR_NORMAL |
           MMU_AP_RW |
           MMU_SH_INNER_SHAREABLE |
           MMU_AF |
           MMU_PXN |
           MMU_UXN;
}

static int build_generic_boot_map(unsigned long **l0, unsigned long **l1, unsigned long **l2, const char *prefix)
{
    unsigned long fine_map_end;
    unsigned long kernel_map_end;
    unsigned long minimum_map_end;
    unsigned long chunk_base;
    unsigned long address;

    if (!*l0) {
        *l0 = alloc_named_table_page(prefix ? prefix : "l0");
    }
    if (!*l1) {
        *l1 = alloc_named_table_page(prefix ? prefix : "l1");
    }
    if (!*l2) {
        *l2 = alloc_named_table_page(prefix ? prefix : "l2");
    }

    if (!*l0 || !*l1 || !*l2) return 0;

    /* L1 device block: 0x0 - 1GB maps to Peripheral space */
    (*l1)[L1_INDEX_FOR(0x00000000UL)] = 0x00000000UL |
                                           MMU_ATTR_DEVICE |
                                           MMU_AP_RW |
                                           MMU_SH_NON_SHAREABLE |
                                           MMU_AF |
                                           MMU_PXN |
                                           MMU_UXN |
                                           MMU_DESC_BLOCK;

    /* L0 links to L1 */
    (*l0)[L0_INDEX_FOR(0x00000000UL)] = ((unsigned long)*l1) | MMU_DESC_TABLE;

    /* L1 links to L2 for RAM */
    (*l1)[L1_INDEX_FOR(QEMU_VIRT_RAM_BASE)] = ((unsigned long)*l2) | MMU_DESC_TABLE;

    kernel_map_end = MMU_ALIGN_UP((unsigned long)va_to_pa(__kernel_end), MMU_L2_BLOCK_SIZE);
    minimum_map_end = QEMU_VIRT_RAM_BASE + (MMU_KERNEL_FINE_MAP_MIN_CHUNKS * MMU_L2_BLOCK_SIZE);
    fine_map_end = kernel_map_end;
    if (fine_map_end < minimum_map_end) {
        fine_map_end = minimum_map_end;
    }

    /* Track chunks used for fine mapping (usually for identity map) */
    if (*l2 == mmu_l2_ram_table) {
        mmu_fine_map_chunks_used = (fine_map_end - QEMU_VIRT_RAM_BASE) / MMU_L2_BLOCK_SIZE;
    }

    /* Fine mapping for kernel code/data */
    for (chunk_base = QEMU_VIRT_RAM_BASE; chunk_base < fine_map_end; chunk_base += MMU_L2_BLOCK_SIZE) {
        unsigned long *l3_table;
        unsigned long chunk_index = (chunk_base - QEMU_VIRT_RAM_BASE) / MMU_L2_BLOCK_SIZE;
        
        l3_table = mmu_debug_alloc_named_table_page_chunk(prefix ? prefix : "l3", chunk_index);
        if (!l3_table) return 0;

        (*l2)[L2_INDEX_FOR(chunk_base)] = ((unsigned long)l3_table) | MMU_DESC_TABLE;

        for (address = chunk_base; address < chunk_base + MMU_L2_BLOCK_SIZE; address += PAGE_SIZE) {
            unsigned long attrs = mmu_kernel_page_attrs(address);
            if (attrs) {
                l3_table[L3_INDEX_FOR(address)] = address | attrs | MMU_DESC_PAGE;
            }
        }
    }

    /* Block mapping for the rest of RAM */
    for (address = fine_map_end; address < QEMU_VIRT_RAM_BASE + QEMU_VIRT_RAM_SIZE; address += MMU_L2_BLOCK_SIZE) {
        (*l2)[L2_INDEX_FOR(address)] = address |
                                              MMU_ATTR_NORMAL |
                                              MMU_AP_RW |
                                              MMU_SH_INNER_SHAREABLE |
                                              MMU_AF |
                                              MMU_PXN |
                                              MMU_UXN |
                                              MMU_DESC_BLOCK;
    }

    return 1;
}

int mmu_build_identity_map(void)
{
    return build_generic_boot_map(&mmu_l0_table, &mmu_l1_table, &mmu_l2_ram_table, "id");
}

int build_kernel_map(void)
{
    return build_generic_boot_map(&l0_table_ttbr1, &l1_table_ttbr1, &l2_ram_table_ttbr1, "t1-");
}
