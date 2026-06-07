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
void mmu_init_secondary(void);
int mmu_is_enabled(void);
int mmu_handle_page_fault(unsigned long far_el1, unsigned long esr_el1);

/* Refactored version to handle faults for a specific process (used during loading) */
struct process;
int mmu_handle_process_page_fault(struct process *p, unsigned long far_el1, unsigned long esr_el1);

#define MMU_VA_BITS          48UL
#define MMU_L1_BLOCK_SIZE    0x40000000UL
#define MMU_T0SZ             (64UL - MMU_VA_BITS)
#define MMU_L2_BLOCK_SIZE    0x00200000UL
#define MMU_KERNEL_FINE_MAP_MIN_CHUNKS 2UL

#define MMU_DESC_VALID         (1UL << 0)
#define MMU_DESC_TABLE         (MMU_DESC_VALID | (1UL << 1))
#define MMU_DESC_BLOCK         MMU_DESC_VALID
#define MMU_DESC_PAGE          MMU_DESC_TABLE

#define MMU_ATTR_INDEX(x)      ((unsigned long)(x) << 2)
#define MMU_ATTR_INDEX_MASK    MMU_ATTR_INDEX(0x7UL)
#define MMU_ATTR_DEVICE        MMU_ATTR_INDEX(0)
#define MMU_ATTR_NORMAL        MMU_ATTR_INDEX(1)

#define MMU_AP_RW              (0UL << 6)
#define MMU_AP_RO              (2UL << 6)
#define MMU_SH_NON_SHAREABLE   (0UL << 8)
#define MMU_SH_INNER_SHAREABLE (3UL << 8)
#define MMU_AF                 (1UL << 10)
#define MMU_PXN                (1UL << 53)
#define MMU_UXN                (1UL << 54)

#define MMU_MAIR_DEVICE_nGnRnE 0x00UL
#define MMU_MAIR_NORMAL_WBWA   0xffUL
#define MMU_TCR_IPS_48BIT      (5UL << 32)
#ifdef CONFIG_KERNEL_VIRTUAL
#define MMU_T1SZ               (16UL << 16)
#define MMU_TG1_4K             (2UL << 30)
#endif

#define SCTLR_EL1_RES1         ((1UL << 29) | (1UL << 28) | (1UL << 23) | (1UL << 22) | (1UL << 20) | (1UL << 11))
#define SCTLR_EL1_M            (1UL << 0)
#define SCTLR_EL1_C            (1UL << 2)
#define SCTLR_EL1_I            (1UL << 12)

#define MMU_DESC_TYPE_MASK      0x3UL
#define MMU_DESC_ADDR_MASK      0x0000fffffffff000UL
#define MMU_L1_BLOCK_ADDR_MASK  0x0000ffffc0000000UL
#define MMU_L2_BLOCK_ADDR_MASK  0x0000ffffffe00000UL
#define MMU_L3_PAGE_ADDR_MASK   0x0000fffffffff000UL

#define L0_INDEX_FOR(address) ((address >> 39) & 0x1ffUL)
#define L1_INDEX_FOR(address) ((address >> 30) & 0x1ffUL)
#define L2_INDEX_FOR(address) ((address >> 21) & 0x1ffUL)
#define L3_INDEX_FOR(address) ((address >> 12) & 0x1ffUL)

#define MMU_ALIGN_UP(value, alignment) (((value) + (alignment) - 1UL) & ~((alignment) - 1UL))

/* 
 * Symbols for boot-time identity map, shared between mmu.c and mmu_boot.c.
 */
extern unsigned long *mmu_l0_table;
extern unsigned long *mmu_l1_table;
extern unsigned long *mmu_l2_ram_table;
extern unsigned long mmu_fine_map_chunks_used;

#ifdef CONFIG_KERNEL_VIRTUAL
extern unsigned long *l0_table_ttbr1;
extern unsigned long *l1_table_ttbr1;
extern unsigned long *l2_ram_table_ttbr1;
int build_kernel_map(void);
#endif

unsigned long mmu_kernel_page_attrs(unsigned long address);
int mmu_build_identity_map(void);
unsigned long *alloc_named_table_page(const char *name);

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
struct mm_context *mmu_context_clone(struct mm_context *src);
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
int mmu_unmap_user_range(struct mm_context *mm, unsigned long va, unsigned long len);

/*
 * Handle an EL0 translation fault by lazy-mapping a new page if the address
 * falls within the task's valid address space (heap or stack).
 * Returns 1 if the fault was handled (mapping successful), 0 otherwise.
 */
int mmu_handle_page_fault(unsigned long far_el1, unsigned long esr_el1);
#endif

/* Number of physical pages currently consumed by translation tables. */
unsigned long mmu_table_pages_used(void);

#endif