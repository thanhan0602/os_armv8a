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
 * Replace the boot-time TTBR0 identity map with an owned empty lower-half
 * root after the kernel has switched to TTBR1. The old TTBR0 table pages are
 * returned to the page allocator while TTBR0 walks remain enabled.
 */
void mmu_install_empty_ttbr0_root(void);

/*
 * Per-process lower-half address space handle.  A NULL mm means no user
 * address space (kernel task); mmu_context_switch() installs the empty
 * runtime TTBR0 root in that case.
 */
struct mm_context {
    unsigned long root_pa;
};

struct mm_context *mmu_context_create(void);
void mmu_context_destroy(struct mm_context *mm);
void mmu_context_switch(struct mm_context *mm);

/*
 * Page attribute flag constants for mmu_map_user_page().
 * Callers combine these to describe the desired permissions.
 */
#define MMU_USER_PAGE_NORMAL    (1UL << 2)   /* AttrIndx=1: Normal WB/WA  */
#define MMU_USER_PAGE_AF        (1UL << 10)  /* access flag (must be set) */
#define MMU_USER_PAGE_INNER_SH  (3UL << 8)   /* inner-shareable           */
#define MMU_USER_PAGE_AP_RO     (3UL << 6)   /* AP[2:1]=11: EL0+EL1 RO   */
#define MMU_USER_PAGE_AP_RW     (1UL << 6)   /* AP[2:1]=01: EL0+EL1 RW   */
#define MMU_USER_PAGE_UXN       (1UL << 54)  /* EL0 execute-never        */
#define MMU_USER_PAGE_PXN       (1UL << 53)  /* EL1 execute-never        */

/*
 * Map a single 4 KiB page in the lower-half address space described by mm.
 * va and pa must be 4 KiB aligned.  flags are the page descriptor attribute
 * bits (use the MMU_USER_PAGE_* constants above); the function supplies the
 * VALID+PAGE encoding itself.  Intermediate tables are allocated on demand.
 * Returns 1 on success, 0 if a page allocation for a sub-table fails.
 * Performs a TLB broadcast invalidation on success.
 */
int mmu_map_user_page(struct mm_context *mm, unsigned long va,
                      unsigned long pa, unsigned long flags);
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