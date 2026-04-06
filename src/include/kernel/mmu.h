#ifndef KERNEL_MMU_H
#define KERNEL_MMU_H

/*
 * MMU bring-up API for the current kernel-only EL1 address space.
 *
 * Current design:
 * - one Stage-1 translation regime owned by the kernel
 * - TTBR0_EL1 points at a 4-level page-table tree
 * - virtual addresses currently equal physical addresses for bring-up
 * - early kernel RAM uses L3 pages for per-section permissions
 * - later RAM uses L2 blocks to keep table usage small
 */
void mmu_init(void);
int mmu_is_enabled(void);

#ifdef CONFIG_KERNEL_VIRTUAL
/*
 * Disable TTBR0 translations (set EPD0=1 in TCR_EL1) and return the
 * TTBR0 table pages to the page allocator.  Only meaningful when the
 * kernel is running at high VA through TTBR1.
 */
void mmu_disable_ttbr0(void);
#endif

/* Number of physical pages currently consumed by translation tables. */
unsigned long mmu_table_pages_used(void);

/* Debug helpers for software walks and hardware translation probes. */
void mmu_debug_walk_address(unsigned long address);
unsigned long mmu_debug_probe_address(unsigned long address);

/* Boot-time debug targets used by the MMU bring-up logs. */
unsigned long mmu_debug_boot_target_count(void);
const char *mmu_debug_boot_target_name(unsigned long index);
unsigned long mmu_debug_boot_target_address(unsigned long index);

/* Table-page inventory recorded while page tables are allocated. */
unsigned long mmu_debug_table_page_count(void);
unsigned long mmu_debug_table_page_address(unsigned long index);
const char *mmu_debug_table_page_name(unsigned long index);

#endif