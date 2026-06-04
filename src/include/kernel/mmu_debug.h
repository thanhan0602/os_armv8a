#ifndef KERNEL_MMU_DEBUG_H
#define KERNEL_MMU_DEBUG_H

const char *mmu_region_name(unsigned long address);
void mmu_debug_print_index(const char *level_name, unsigned long index);
void mmu_debug_print_entry(const char *level_name, unsigned long entry);
void mmu_debug_print_leaf_attrs(unsigned long address, unsigned long entry);
void mmu_debug_print_desc_kind(const char *level_name, const char *kind);
void mmu_debug_walk_address(unsigned long address);
unsigned long mmu_debug_probe_address(unsigned long address);

/* Table page inventory and helpers */
void mmu_debug_record_table_page(unsigned long address, const char *name);
void mmu_debug_copy_name(char *destination, const char *source);
int mmu_debug_name_has_prefix(const char *name, const char *prefix);
void mmu_debug_set_chunk_name(char *destination, const char *prefix, unsigned long chunk_index);
unsigned long mmu_debug_table_page_count(void);
unsigned long mmu_debug_table_page_address(unsigned long index);
const char *mmu_debug_table_page_name(unsigned long index);
unsigned long mmu_table_pages_used(void);

/* Boot target helpers */
unsigned long mmu_debug_boot_target_count(void);
const char *mmu_debug_boot_target_name(unsigned long index);
unsigned long mmu_debug_boot_target_address(unsigned long index);

/* Compact tables after installing an empty TTBR0 root */
void mmu_debug_compact_for_ttbr0_install(unsigned long new_root);
unsigned long *mmu_debug_alloc_named_table_page_chunk(const char *prefix, unsigned long chunk_index);
void mmu_debug_reset(void);

unsigned long *mmu_debug_ttbr0_root(void);

#endif
