#ifndef KERNEL_MMU_DEBUG_H
#define KERNEL_MMU_DEBUG_H

const char *mmu_region_name(unsigned long address);
void mmu_debug_print_index(const char *level_name, unsigned long index);
void mmu_debug_print_entry(const char *level_name, unsigned long entry);
void mmu_debug_print_leaf_attrs(unsigned long address, unsigned long entry);
void mmu_debug_print_desc_kind(const char *level_name, const char *kind);
void mmu_debug_walk_address(unsigned long address);
unsigned long mmu_debug_probe_address(unsigned long address);

/* Boot target helpers */
unsigned long mmu_debug_boot_target_count(void);
const char *mmu_debug_boot_target_name(unsigned long index);
unsigned long mmu_debug_boot_target_address(unsigned long index);

unsigned long *mmu_debug_ttbr0_root(void);

#endif
