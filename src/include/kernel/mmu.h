#ifndef KERNEL_MMU_H
#define KERNEL_MMU_H

void mmu_init(void);
int mmu_is_enabled(void);
unsigned long mmu_table_pages_used(void);
void mmu_debug_walk_address(unsigned long address);
unsigned long mmu_debug_probe_address(unsigned long address);
unsigned long mmu_debug_boot_target_count(void);
const char *mmu_debug_boot_target_name(unsigned long index);
unsigned long mmu_debug_boot_target_address(unsigned long index);
unsigned long mmu_debug_table_page_count(void);
unsigned long mmu_debug_table_page_address(unsigned long index);
const char *mmu_debug_table_page_name(unsigned long index);

#endif