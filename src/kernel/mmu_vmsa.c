#include <kernel/mmu.h>
#include <kernel/mmu_debug.h>
#include <kernel/page_alloc.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/heap.h>
#include <kernel/log.h>
#include <kernel/vm.h>
#include <arch/arm/virt.h>
#include <arch/arm/sysregs.h>

#ifdef CONFIG_KERNEL_VIRTUAL

/* Root pointer for the empty TTBR0 root during kernel tasks. */
unsigned long *ttbr0_runtime_empty_root;

void mmu_install_empty_ttbr0_root(void)
{
    unsigned long *new_root;

    if (!mmu_is_enabled()) {
        return;
    }

    if (ttbr0_runtime_empty_root != (unsigned long *)0 && mmu_debug_ttbr0_root() == ttbr0_runtime_empty_root) {
        return;
    }

    new_root = (unsigned long *)page_alloc();
    if (new_root == (unsigned long *)0) {
        KER_INFO("ttbr0 empty-root install failed: no free pages");
        return;
    }

    for (int i = 0; i < 512; i++) {
        new_root[i] = 0UL;
    }

    mmu_set_ttbr0((unsigned long)new_root);
    mmu_invalidate_tlb_all();

    ttbr0_runtime_empty_root = new_root;
    
    /* We need a way to update the global mmu_l0_table in mmu.c if it's external,
     * or at least inform the debug system. 
     * For now, mmu_debug_ttbr0_root() returns mmu_l0_table.
     */
    extern unsigned long *mmu_l0_table;
    extern unsigned long *mmu_l1_table;
    extern unsigned long *mmu_l2_ram_table;
    mmu_l0_table = new_root;
    mmu_l1_table = (unsigned long *)0;
    mmu_l2_ram_table = (unsigned long *)0;

    mmu_debug_compact_for_ttbr0_install((unsigned long)new_root);
}

/*
 * ASID allocation state.
 * ASID 0 is reserved for kernel tasks / the empty lower-half root.
 * 16-bit ASIDs (TCR_EL1.AS=1): valid user range is 1–65535.
 */
static unsigned int next_asid = 1;
static unsigned long asid_bitmap[65536 / 64];

static void mmu_tlbi_asid(unsigned int asid)
{
    if (asid == 0U) {
        return;
    }

    mmu_invalidate_tlb_asid(asid);
}

static void *mmu_alloc_sub_table(struct mm_context *mm, const char *name)
{
    void *new_page = page_alloc();
    if (!new_page) return (void *)0;

    if (!mmu_context_add_page(mm, (unsigned long)va_to_pa(new_page))) {
        page_free(new_page);
        return (void *)0;
    }

    unsigned long *tbl = (unsigned long *)pa_to_va(va_to_pa(new_page));
    for (int i = 0; i < 512; i++) tbl[i] = 0UL;

    if (name) mmu_debug_record_table_page((unsigned long)va_to_pa(new_page), name);
    return new_page;
}

static void mmu_cow_log(unsigned long va, const char *msg, unsigned int ref)
{
    struct task *curr = sched_current();
    if (curr) {
        KER_LOGF("[mmu] [%s:%lu] CoW: VA=%lx %s (ref=%u)\n", curr->name, curr->id, va, msg, ref);
    } else {
        KER_LOGF("[mmu] CoW: VA=%lx %s (ref=%u)\n", va, msg, ref);
    }
}

static void mmu_asid_set(unsigned int asid)
{
    asid_bitmap[asid / 64] |= (1UL << (asid % 64));
}

static void mmu_asid_clear(unsigned int asid)
{
    asid_bitmap[asid / 64] &= ~(1UL << (asid % 64));
}

static int mmu_asid_test(unsigned int asid)
{
    return (asid_bitmap[asid / 64] & (1UL << (asid % 64))) != 0UL;
}

static unsigned int mmu_asid_alloc(void)
{
    unsigned int attempts;
    unsigned int asid;

    for (attempts = 0U; attempts < 65535U; attempts++) {
        asid = next_asid;
        next_asid++;
        if (next_asid >= 65536U) {
            next_asid = 1U;
        }

        if (asid == 0U || asid >= 65536U || mmu_asid_test(asid)) {
            continue;
        }

        mmu_asid_set(asid);
        mmu_tlbi_asid(asid);
        return asid;
    }

    return 0U;
}

static void mmu_asid_free(unsigned int asid)
{
    if (asid == 0U || asid >= 65536U) {
        return;
    }

    mmu_tlbi_asid(asid);
    mmu_asid_clear(asid);
}

static int mmu_context_has_page(const struct mm_context *mm, unsigned long pa)
{
    unsigned int index;

    if (mm == (const struct mm_context *)0) {
        return 0;
    }

    for (index = 0U; index < mm->page_count; index++) {
        if (mm->pages[index] == pa) {
            return 1;
        }
    }

    return 0;
}

int mmu_context_add_page(struct mm_context *mm, unsigned long pa)
{
    if (mm == (struct mm_context *)0 || pa == 0UL) {
        return 0;
    }

    if (mmu_context_has_page(mm, pa)) {
        return 1;
    }

    if (mm->page_count >= MM_MAX_TRACKED_PAGES) {
        return 0;
    }

    mm->pages[mm->page_count++] = pa;
    return 1;
}

int mmu_context_remove_page(struct mm_context *mm, unsigned long pa)
{
    unsigned int index;

    if (mm == (struct mm_context *)0 || pa == 0UL) {
        return 0;
    }

    for (index = 0U; index < mm->page_count; index++) {
        unsigned int tail;

        if (mm->pages[index] != pa) {
            continue;
        }

        for (tail = index + 1U; tail < mm->page_count; tail++) {
            mm->pages[tail - 1U] = mm->pages[tail];
        }

        mm->page_count--;
        mm->pages[mm->page_count] = 0UL;
        return 1;
    }

    return 0;
}

static int mmu_resolve_user_page_desc(const struct mm_context *mm,
                                      unsigned long va,
                                      unsigned long **entry_ptr_out)
{
    unsigned long *l0;
    unsigned long *l1;
    unsigned long *l2;
    unsigned long *l3;
    unsigned long entry;

    if (mm == (const struct mm_context *)0 || entry_ptr_out == (unsigned long **)0) {
        return 0;
    }

    l0 = (unsigned long *)pa_to_va((void *)mm->root_pa);
    entry = l0[L0_INDEX_FOR(va)];
    if ((entry & MMU_DESC_VALID) == 0UL || (entry & MMU_DESC_TYPE_MASK) != MMU_DESC_TABLE) {
        return 0;
    }

    l1 = (unsigned long *)pa_to_va((void *)(entry & MMU_DESC_ADDR_MASK));
    entry = l1[L1_INDEX_FOR(va)];
    if ((entry & MMU_DESC_VALID) == 0UL || (entry & MMU_DESC_TYPE_MASK) != MMU_DESC_TABLE) {
        return 0;
    }

    l2 = (unsigned long *)pa_to_va((void *)(entry & MMU_DESC_ADDR_MASK));
    entry = l2[L2_INDEX_FOR(va)];
    if ((entry & MMU_DESC_VALID) == 0UL || (entry & MMU_DESC_TYPE_MASK) != MMU_DESC_TABLE) {
        return 0;
    }

    l3 = (unsigned long *)pa_to_va((void *)(entry & MMU_DESC_ADDR_MASK));
    *entry_ptr_out = &l3[L3_INDEX_FOR(va)];
    return 1;
}

static int mmu_resolve_user_page(const struct mm_context *mm,
                                 unsigned long va,
                                 unsigned long *page_pa,
                                 unsigned long *entry_out)
{
    unsigned long *entry_ptr;
    unsigned long entry;

    if (!mmu_resolve_user_page_desc(mm, va, &entry_ptr)) {
        return 0;
    }

    entry = *entry_ptr;
    if ((entry & MMU_DESC_VALID) == 0UL || (entry & MMU_DESC_TYPE_MASK) != MMU_DESC_PAGE) {
        return 0;
    }

    if (page_pa != (unsigned long *)0) {
        *page_pa = entry & MMU_L3_PAGE_ADDR_MASK;
    }
    if (entry_out != (unsigned long *)0) {
        *entry_out = entry;
    }

    return 1;
}

static int mmu_user_access_allowed(unsigned long entry, int write)
{
    unsigned long ap_bits;

    ap_bits = entry & (3UL << 6);
    if (write != 0) {
        return ap_bits == MMU_USER_PAGE_AP_RW;
    }

    return ap_bits == MMU_USER_PAGE_AP_RW || ap_bits == MMU_USER_PAGE_AP_RO;
}

static int mmu_copy_user_range(const struct mm_context *mm,
                               unsigned char *kernel_buffer,
                               unsigned long user_va,
                               unsigned long len,
                               int write_to_user)
{
    while (len != 0UL) {
        unsigned long page_pa;
        unsigned long entry;
        unsigned long page_offset;
        unsigned long chunk;
        unsigned char *page_va;
        unsigned long index;

        if (!mmu_resolve_user_page(mm, user_va, &page_pa, &entry)) {
            /* Try to handle fault if it's the current task's MM */
            struct task *cur = sched_current();
            if (cur && cur->mm == mm && cur->process) {
                /* KER_LOGF("[mmu] Software fault: VA=%lx\n", user_va); */
                /* ESR 0x04 = translation fault EL0 */
                if (mmu_handle_process_page_fault(cur->process, user_va, 0x04)) {
                    if (mmu_resolve_user_page(mm, user_va, &page_pa, &entry)) {
                        goto resolved;
                    }
                }
            }
            KER_LOGF("[mmu] copy failed: could not resolve VA=%lx\n", user_va);
            return 0;
        }

resolved:
        if (!mmu_user_access_allowed(entry, write_to_user)) {
            /* If it's a write and it failed, it might be CoW */
            if (write_to_user) {
                struct task *cur = sched_current();
                if (cur && cur->mm == mm && cur->process) {
                    /* KER_LOGF("[mmu] Software permission fault: VA=%lx\n", user_va); */
                    /* ESR 0x0F = permission fault EL0 write */
                    if (mmu_handle_process_page_fault(cur->process, user_va, 0x0F)) {
                        if (mmu_resolve_user_page(mm, user_va, &page_pa, &entry)) {
                            goto resolved_cow;
                        }
                    }
                }
            }
            KER_LOGF("[mmu] copy failed: access denied VA=%lx write=%d\n", user_va, write_to_user);
            return 0;
        }

resolved_cow:
        page_offset = user_va & (PAGE_SIZE - 1UL);
        chunk = PAGE_SIZE - page_offset;
        if (chunk > len) {
            chunk = len;
        }

        page_va = (unsigned char *)pa_to_va((void *)(page_pa + page_offset));
        for (index = 0UL; index < chunk; index++) {
            if (write_to_user != 0) {
                page_va[index] = kernel_buffer[index];
            } else {
                kernel_buffer[index] = page_va[index];
            }
        }

        kernel_buffer += chunk;
        user_va += chunk;
        len -= chunk;
    }

    return 1;
}

struct mm_context *mmu_context_create(void)
{
    struct mm_context *mm;
    unsigned long *root;
    unsigned int index;

    mm = (struct mm_context *)kmalloc(sizeof(struct mm_context));
    if (mm == (struct mm_context *)0) {
        return (struct mm_context *)0;
    }

    root = (unsigned long *)page_alloc();
    if (root == (unsigned long *)0) {
        kfree(mm);
        return (struct mm_context *)0;
    }

    mm->root_pa = (unsigned long)root;
    mm->asid    = mmu_asid_alloc();
    mm->page_count = 0U;
    for (index = 0U; index < MM_MAX_TRACKED_PAGES; index++) {
        mm->pages[index] = 0UL;
    }

    if (mm->asid == 0U) {
        page_free(root);
        kfree(mm);
        return (struct mm_context *)0;
    }

    if (!mmu_context_add_page(mm, mm->root_pa)) {
        page_free(root);
        mmu_asid_free(mm->asid);
        kfree(mm);
        return (struct mm_context *)0;
    }

    return mm;
}

void mmu_context_destroy(struct mm_context *mm)
{
    if (mm == (struct mm_context *)0) {
        return;
    }

    while (mm->page_count > 0U) {
        mm->page_count--;
        if (mm->pages[mm->page_count] != 0UL) {
            page_free((void *)mm->pages[mm->page_count]);
        }
    }

    mmu_asid_free(mm->asid);

    kfree(mm);
}

static void mmu_clone_walk(struct mm_context *mm, unsigned long *src_tbl, unsigned long *dst_tbl, int level)
{
    for (int i = 0; i < 512; i++) {
        unsigned long entry = src_tbl[i];
        if (!(entry & MMU_DESC_VALID)) continue;

        if (level < 3 && (entry & MMU_DESC_TYPE_MASK) == MMU_DESC_TABLE) {
            /* Table entry */
            unsigned long *next_src = (unsigned long *)pa_to_va(entry & MMU_DESC_ADDR_MASK);
            unsigned long next_dst_pa = (unsigned long)page_alloc();
            if (!next_dst_pa) continue;
            
            if (!mmu_context_add_page(mm, next_dst_pa)) {
                page_free((void *)next_dst_pa);
                continue;
            }
            
            unsigned long *next_dst_va = (unsigned long *)pa_to_va(next_dst_pa);
            for (int k = 0; k < 512; k++) next_dst_va[k] = 0;
            
            dst_tbl[i] = (next_dst_pa & MMU_L3_PAGE_ADDR_MASK) | (entry & ~MMU_L3_PAGE_ADDR_MASK);
            mmu_clone_walk(mm, next_src, next_dst_va, level + 1);
        } else if (level == 3) {
            /* L3 Leaf entry - Mark as Read-Only for CoW */
            unsigned long pa = entry & MMU_L3_PAGE_ADDR_MASK;
            
            /* Ensure it is Read-Only in both and marked for CoW */
            unsigned long cow_entry = (entry & ~MMU_AP_RW) | MMU_AP_RO | MMU_DESC_SOFTWARE_COW;
            
            /* 
             * Order is important: increment ref_count before making src_tbl entry RO 
             * to avoid potential race where other CPU might see RO and try to free/upgrade 
             */
            page_ref_inc(pa);
            
            dst_tbl[i] = cow_entry;
            src_tbl[i] = cow_entry;
        }
    }
}

struct mm_context *mmu_context_clone(struct mm_context *src)
{
    if (!src) return (struct mm_context *)0;

    struct mm_context *dst = mmu_context_create();
    if (!dst) return (struct mm_context *)0;

    unsigned long *src_root = (unsigned long *)pa_to_va(src->root_pa);
    unsigned long *dst_root = (unsigned long *)pa_to_va(dst->root_pa);

    mmu_clone_walk(dst, src_root, dst_root, 0);

    /* Invalidate TLB for the source ASID since we modified its entries to RO */
    mmu_invalidate_tlb_asid(src->asid);

    return dst;
}

int mmu_copy_from_user(const struct mm_context *mm, void *dst,
                       unsigned long src_va, unsigned long len)
{
    if (len == 0UL) {
        return 1;
    }

    if (dst == (void *)0) {
        return 0;
    }

    return mmu_copy_user_range(mm, (unsigned char *)dst, src_va, len, 0);
}

int mmu_copy_to_user(const struct mm_context *mm, unsigned long dst_va,
                     const void *src, unsigned long len)
{
    if (len == 0UL) {
        return 1;
    }

    if (src == (const void *)0) {
        return 0;
    }

    return mmu_copy_user_range(mm, (unsigned char *)src, dst_va, len, 1);
}

int mmu_user_page_pa(const struct mm_context *mm, unsigned long va,
                     unsigned long *page_pa_out)
{
    unsigned long page_pa;

    if (page_pa_out == (unsigned long *)0) {
        return 0;
    }

    if (!mmu_resolve_user_page(mm, va, &page_pa, (unsigned long *)0)) {
        return 0;
    }

    *page_pa_out = page_pa;
    return 1;
}

void mmu_context_switch(struct mm_context *mm)
{
    unsigned long ttbr0_val;

    if (!mmu_is_enabled()) {
        return;
    }

    if (mm == (struct mm_context *)0) {
        ttbr0_val = (unsigned long)va_to_pa(ttbr0_runtime_empty_root);
    } else {
        ttbr0_val = mm->root_pa | ((unsigned long)mm->asid << 48);
    }

    mmu_set_ttbr0(ttbr0_val);
    mmu_invalidate_tlb_all();
}

static void mmu_split_block(struct mm_context *mm, unsigned long *entry_ptr, unsigned long level)
{
    unsigned long entry = *entry_ptr;
    unsigned long block_pa = entry & MMU_DESC_ADDR_MASK;
    unsigned long block_attrs = entry & ~MMU_DESC_ADDR_MASK;
    unsigned long step = (level == 1) ? MMU_L2_BLOCK_SIZE : PAGE_SIZE;
    unsigned long next_type = (level == 1) ? MMU_DESC_BLOCK : MMU_DESC_PAGE;

    void *new_page = mmu_alloc_sub_table(mm, (level == 1) ? "split-l2" : "split-l3");
    if (!new_page) return;

    unsigned long *new_tbl = (unsigned long *)pa_to_va(new_page);
    for (int i = 0; i < 512; i++) {
        new_tbl[i] = (block_pa + ((unsigned long)i * step)) | block_attrs | next_type;
    }

    *entry_ptr = (unsigned long)va_to_pa(new_page) | MMU_DESC_TABLE;
}

static unsigned long *mmu_find_pte(struct mm_context *mm, unsigned long va, int create)
{
    unsigned long *l0, *l1, *l2, *l3;
    unsigned long idx;

    if (!mm) return (unsigned long *)0;

    l0 = (unsigned long *)pa_to_va((void *)mm->root_pa);
    idx = L0_INDEX_FOR(va);
    if (!(l0[idx] & MMU_DESC_VALID)) {
        if (!create) return (unsigned long *)0;
        void *new_page = mmu_alloc_sub_table(mm, "l1");
        if (!new_page) return (unsigned long *)0;
        l0[idx] = (unsigned long)va_to_pa(new_page) | MMU_DESC_TABLE;
    }
    
    l1 = (unsigned long *)pa_to_va((void *)(l0[idx] & MMU_DESC_ADDR_MASK));
    idx = L1_INDEX_FOR(va);

    if ((l1[idx] & MMU_DESC_VALID) && (l1[idx] & MMU_DESC_TYPE_MASK) == MMU_DESC_BLOCK) {
        if (!create) return (unsigned long *)0;
        mmu_split_block(mm, &l1[idx], 1);
    }

    if (!(l1[idx] & MMU_DESC_VALID)) {
        if (!create) return (unsigned long *)0;
        void *new_page = mmu_alloc_sub_table(mm, "l2");
        if (!new_page) return (unsigned long *)0;
        l1[idx] = (unsigned long)va_to_pa(new_page) | MMU_DESC_TABLE;
    }

    l2 = (unsigned long *)pa_to_va((void *)(l1[idx] & MMU_DESC_ADDR_MASK));
    idx = L2_INDEX_FOR(va);

    if ((l2[idx] & MMU_DESC_VALID) && (l2[idx] & MMU_DESC_TYPE_MASK) == MMU_DESC_BLOCK) {
        if (!create) return (unsigned long *)0;
        mmu_split_block(mm, &l2[idx], 2);
    }

    if (!(l2[idx] & MMU_DESC_VALID)) {
        if (!create) return (unsigned long *)0;
        void *new_page = mmu_alloc_sub_table(mm, "l3");
        if (!new_page) return (unsigned long *)0;
        l2[idx] = (unsigned long)va_to_pa(new_page) | MMU_DESC_TABLE;
    }

    l3 = (unsigned long *)pa_to_va((void *)(l2[idx] & MMU_DESC_ADDR_MASK));
    return l3;
}

int mmu_map_user_page(struct mm_context *mm, unsigned long va,
                      unsigned long pa, unsigned long flags)
{
    unsigned long *l3;
    unsigned long idx;

    if (mm == (struct mm_context *)0) {
        return 0;
    }

    l3 = mmu_find_pte(mm, va, 1);
    if (!l3) return 0;

    /* Install the L3 page descriptor.
     * Force nG=1 (bit 11) so the TLB entry is tagged with the current ASID.
     */
    idx = L3_INDEX_FOR(va);
    l3[idx] = (pa & MMU_L3_PAGE_ADDR_MASK) | flags | MMU_DESC_PAGE | (1UL << 11);

    mmu_invalidate_tlb_va(mm->asid, va);

    return 1;
}

static void mmu_try_merge_table(struct mm_context *mm, unsigned long va, int level)
{
    unsigned long *l0, *l1, *l2;
    unsigned long *table;
    unsigned long block_size = (level == 2) ? MMU_L1_BLOCK_SIZE : MMU_L2_BLOCK_SIZE;
    unsigned long sub_step = (level == 2) ? MMU_L2_BLOCK_SIZE : PAGE_SIZE;
    unsigned long sub_type = (level == 2) ? MMU_DESC_BLOCK : MMU_DESC_PAGE;

    l0 = (unsigned long *)pa_to_va((void *)mm->root_pa);
    l1 = (unsigned long *)pa_to_va((void *)(l0[L0_INDEX_FOR(va)] & MMU_DESC_ADDR_MASK));
    if (level == 2) {
        table = l1;
    } else {
        l2 = (unsigned long *)pa_to_va((void *)(l1[L1_INDEX_FOR(va)] & MMU_DESC_ADDR_MASK));
        table = (level == 3) ? (unsigned long *)pa_to_va((void *)(l2[L2_INDEX_FOR(va)] & MMU_DESC_ADDR_MASK)) : l2;
    }

    /* Check if all 512 entries are identical and contiguous */
    unsigned long base_pa = 0;
    unsigned long base_attrs = 0;
    for (int i = 0; i < 512; i++) {
        if (!(table[i] & MMU_DESC_VALID) || (table[i] & MMU_DESC_TYPE_MASK) != sub_type) return;
        unsigned long cur_pa = table[i] & MMU_DESC_ADDR_MASK;
        unsigned long cur_attrs = table[i] & ~MMU_DESC_ADDR_MASK;
        if (i == 0) {
            base_pa = cur_pa;
            base_attrs = cur_attrs;
            if (base_pa & (block_size - 1)) return;
        } else if (cur_pa != base_pa + (i * sub_step) || cur_attrs != base_attrs) {
            return;
        }
    }

    /* All entries match! Merge them. */
    unsigned long *parent_table = (level == 2) ? l0 : ((level == 3) ? (unsigned long *)pa_to_va((void *)(l1[L1_INDEX_FOR(va)] & MMU_DESC_ADDR_MASK)) : l1);
    unsigned long idx = (level == 2) ? L0_INDEX_FOR(va) : ((level == 3) ? L2_INDEX_FOR(va) : L1_INDEX_FOR(va));
    unsigned long table_pa = parent_table[idx] & MMU_DESC_ADDR_MASK;

    parent_table[idx] = base_pa | (base_attrs & ~MMU_DESC_TYPE_MASK) | MMU_DESC_BLOCK;

    mmu_context_remove_page(mm, table_pa);
    page_free((void *)table_pa);
    mmu_invalidate_tlb_va(mm->asid, va & ~(block_size - 1));

    /* Recurse up if we just merged a table */
    if (level > 2) mmu_try_merge_table(mm, va, level - 1);
}

int mmu_unmap_user_page(struct mm_context *mm, unsigned long va)
{
    unsigned long *l3;
    unsigned long idx;

    if (mm == (struct mm_context *)0) {
        return 0;
    }

    l3 = mmu_find_pte(mm, va, 0);
    if (!l3) return 0;

    idx = L3_INDEX_FOR(va);
    l3[idx] = 0UL;

    mmu_invalidate_tlb_va(mm->asid, va);

    /* Try to merge L3->L2 then L2->L1 */
    mmu_try_merge_table(mm, va, 3);

    return 1;
}

int mmu_handle_process_page_fault(struct process *p, unsigned long far_el1, unsigned long esr_el1)
{
    unsigned long fsc;
    unsigned long fsc_type;
    unsigned long page_va;
    void *new_page;
    unsigned long flags;

    if (!mmu_is_enabled()) {
        return 0;
    }

    if (p == (struct process *)0 || p->mm == (struct mm_context *)0) {
        return 0;
    }

    fsc = esr_el1 & 0x3FUL;
    fsc_type = fsc & 0x3CUL;

    /* KER_LOGF("[mmu] Fault: VA=%lx ESR=%lx (EC=%x FSC=%x)\n", far_el1, esr_el1, ec, (unsigned int)fsc); */

    /* Handle Permission Fault (range 0x0C-0x0F) for Copy-on-Write (CoW) */
    if (fsc_type == 0x0CUL) {
        unsigned long old_pa;
        unsigned long entry;
        void *copy_page;
        unsigned long * volatile cow_entry_ptr = (unsigned long *)0;

        if (!mmu_resolve_user_page_desc(p->mm, far_el1, (unsigned long **)&cow_entry_ptr)) {
            goto cleanup_fatal;
        }

        entry = *cow_entry_ptr;
        if ((entry & MMU_DESC_VALID) != 0UL && 
            (entry & MMU_AP_RO) == MMU_AP_RO &&
            (entry & MMU_DESC_SOFTWARE_COW) != 0UL) {
            
            old_pa = entry & MMU_L3_PAGE_ADDR_MASK;
            page_va = far_el1 & ~(PAGE_SIZE - 1UL);

            if (page_ref_get(old_pa) == 1U) {
                *cow_entry_ptr = (*cow_entry_ptr & ~MMU_AP_RO & ~MMU_DESC_SOFTWARE_COW) | MMU_AP_RW;
                mmu_invalidate_tlb_va(p->mm->asid, page_va);
                mmu_cow_log(page_va, "upgraded to RW (ref=1)", 1);
                return 1;
            }

            mmu_cow_log(page_va, "copying", page_ref_get(old_pa));

            copy_page = page_alloc();
            if (copy_page == (void *)0) return 0;

            unsigned char *src = (unsigned char *)pa_to_va(old_pa);
            unsigned char *dst = (unsigned char *)pa_to_va(copy_page);
            for (unsigned long i = 0; i < PAGE_SIZE; i++) dst[i] = src[i];

            *cow_entry_ptr = ((unsigned long)va_to_pa(copy_page) & MMU_DESC_ADDR_MASK) | 
                         (entry & ~MMU_DESC_ADDR_MASK & ~MMU_AP_RO & ~MMU_DESC_SOFTWARE_COW) | MMU_AP_RW;

            if (!mmu_context_add_page(p->mm, (unsigned long)va_to_pa(copy_page))) {}
            page_ref_dec(old_pa);

            mmu_invalidate_tlb_va(p->mm->asid, page_va);
            return 1;
        }

        goto cleanup_fatal;
    }

    /* Handle Translation Fault (0x04) */
    if (fsc_type == 0x04UL) {
        unsigned int i;
        struct vm_region *region = (struct vm_region *)0;

        /* Search in reverse so the most recently added region wins.
         * This gives mmap() regions precedence over the large anonymous
         * heap region they may overlap (e.g. the dynamic linker mapping
         * an executable inside the process heap range). */
        for (i = p->region_count; i-- > 0;) {
            if (far_el1 >= p->regions[i].start && far_el1 < p->regions[i].end) {
                region = &p->regions[i];
                break;
            }
        }

        if (region) {
            page_va = far_el1 & ~(PAGE_SIZE - 1UL);
            new_page = page_alloc();
            if (!new_page) return 0;

            /* Zero the page first */
            unsigned char *dst = (unsigned char *)pa_to_va(new_page);
            for (unsigned int j = 0; j < PAGE_SIZE; j++) dst[j] = 0;

            if (region->type == VM_TYPE_ELF) {
                /* Copy part of the ELF image if it's within the file size */
                unsigned long region_offset = page_va - region->start;
                if (region->elf_image && region_offset < region->file_size) {
                    unsigned long copy_len = region->file_size - region_offset;
                    if (copy_len > PAGE_SIZE) copy_len = PAGE_SIZE;

                    const unsigned char *src = region->elf_image + region->elf_offset + region_offset;
                    for (unsigned long j = 0; j < copy_len; j++) {
                        dst[j] = src[j];
                    }
                }
            }

            flags = region->flags;
            if (!mmu_context_add_page(p->mm, (unsigned long)va_to_pa(new_page))) {
                page_free(new_page);
                return 0;
            }

            if (!mmu_map_user_page(p->mm, page_va, (unsigned long)va_to_pa(new_page), flags)) {
                mmu_context_remove_page(p->mm, (unsigned long)va_to_pa(new_page));
                page_free(new_page);
                return 0;
            }
            return 1;
        }

        /* Fallback for automatic stack growth or legacy heap if not in regions */
        if (far_el1 >= USER_HEAP_BASE && far_el1 < p->brk) {
             page_va = far_el1 & ~(PAGE_SIZE - 1UL);
        } else if (far_el1 >= (USER_STACK_TOP - 0x100000) && far_el1 < USER_STACK_TOP) {
             /* Reasonable stack range (1MB from top) */
             page_va = far_el1 & ~(PAGE_SIZE - 1UL);
        } else {
             goto cleanup_fatal;
        }
    } else {
        goto cleanup_fatal;
    }

    new_page = page_alloc();
    if (new_page == (void *)0) return 0;

    flags = MMU_USER_PAGE_NORMAL | MMU_USER_PAGE_AF |
            MMU_USER_PAGE_INNER_SH | MMU_USER_PAGE_AP_RW |
            MMU_USER_PAGE_UXN | MMU_USER_PAGE_PXN;

    if (!mmu_context_add_page(p->mm, (unsigned long)va_to_pa(new_page))) {
        page_free(new_page);
        return 0;
    }

    if (!mmu_map_user_page(p->mm, page_va, (unsigned long)va_to_pa(new_page), flags)) {
        mmu_context_remove_page(p->mm, (unsigned long)va_to_pa(new_page));
        page_free(new_page);
        return 0;
    }

    return 1;

cleanup_fatal:
    return 0;
}

int mmu_handle_page_fault(unsigned long far_el1, unsigned long esr_el1)
{
    struct task *t = sched_current();

    if (t == (struct task *)0 || t->process == (struct process *)0) {
        return 0;
    }

    return mmu_handle_process_page_fault(t->process, far_el1, esr_el1);
}

int mmu_unmap_user_range(struct mm_context *mm, unsigned long va, unsigned long len)
{
    if (!mm || len == 0) return 1;

    unsigned long start = va & ~(PAGE_SIZE - 1UL);
    unsigned long end = (va + len + (PAGE_SIZE - 1UL)) & ~(PAGE_SIZE - 1UL);

    for (unsigned long curr = start; curr < end; curr += PAGE_SIZE) {
        unsigned long *pte = mmu_find_pte(mm, curr, 0);
        if (pte && (*pte & MMU_DESC_VALID)) {
            unsigned long pa = *pte & MMU_DESC_ADDR_MASK;
            *pte = 0;
            page_ref_dec(pa);
            mmu_invalidate_tlb_va(mm->asid, curr);
        }
    }
    return 1;
}

#endif /* CONFIG_KERNEL_VIRTUAL */
