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
/*
 * Maximum number of physical pages an mm_context can track.
 * For the current two-mapping layout (code page + stack page):
 *   L0=1, L1=1, L2=1, L3=1 per mmu_map_user_page call (up to 4 sub-tables)
 *   2 calls × 4 levels = 8 sub-table pages worst-case (many shared in practice)
 *   + 2 user content pages (code, stack) = 10 total.
 * 16 gives comfortable headroom for future extra mappings.
 */
#define MM_MAX_TRACKED_PAGES 256

struct mm_context {
    unsigned long root_pa;
    unsigned int  asid;     /* 8-bit ASID encoded in TTBR0_EL1[55:48]; 0 = kernel/empty */
    unsigned long pages[MM_MAX_TRACKED_PAGES]; /* PAs of all owned pages (sub-tables + user data) */
    unsigned int  page_count;                  /* number of valid entries in pages[] */
};

struct mm_context *mmu_context_create(void);
void mmu_context_destroy(struct mm_context *mm);
void mmu_context_switch(struct mm_context *mm);
/*
 * Register a physical page with the mm_context so it is freed when the
 * context is destroyed.  Call this for user content pages (code, stack)
 * that are NOT allocated inside mmu_map_user_page itself.  Sub-table
 * pages are tracked automatically by mmu_map_user_page.
 * Returns 1 on success, 0 if the tracking array is full.
 */
int mmu_context_add_page(struct mm_context *mm, unsigned long pa);
int mmu_context_remove_page(struct mm_context *mm, unsigned long pa);

/*
 * Copy bytes between kernel memory and a user address space without
 * dereferencing the user VA directly.  The helpers walk the owning
 * mm_context page tables, translate each user page to PA, then access the
 * page through the kernel's linear map.
 * Returns 1 on success, 0 if any page in the range is unmapped or does not
 * satisfy the requested access.
 */
int mmu_copy_from_user(const struct mm_context *mm, void *dst,
                       unsigned long src_va, unsigned long len);
int mmu_copy_to_user(const struct mm_context *mm, unsigned long dst_va,
                     const void *src, unsigned long len);
int mmu_user_page_pa(const struct mm_context *mm, unsigned long va,
                     unsigned long *page_pa_out);

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
int mmu_unmap_user_page(struct mm_context *mm, unsigned long va);
#endif

/* Number of physical pages currently consumed by translation tables. */
unsigned long mmu_table_pages_used(void);

#endif