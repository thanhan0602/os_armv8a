#ifndef KERNEL_MMU_TABLE_H
#define KERNEL_MMU_TABLE_H

/*
 * Production ownership and lifecycle API for translation-table pages.
 * Diagnostics may inspect this inventory, but MMU correctness must not
 * depend on the debug subsystem.
 */
void mmu_table_registry_reset(void);
void mmu_table_record_page(unsigned long address, const char *name);
unsigned long *mmu_table_alloc_named_page(const char *name);
unsigned long *mmu_table_alloc_chunk_page(const char *prefix,
                                          unsigned long chunk_index);
void mmu_table_release_boot_ttbr0(unsigned long new_root);

unsigned long mmu_table_page_count(void);
unsigned long mmu_table_page_address(unsigned long index);
const char *mmu_table_page_name(unsigned long index);
unsigned long mmu_table_pages_used(void);

#endif
