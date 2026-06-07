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

#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/mmu_debug.h>

/* Forward declaration for fault handling */
int mmu_handle_process_page_fault(struct process *process, unsigned long fault_addr, unsigned long esr);

static int mmu_resolve_user_page(const struct mm_context *mm,
                                 unsigned long va,
                                 unsigned long *pa_out,
                                 unsigned long *entry_out);

static int mmu_user_access_allowed(unsigned long entry, int write);

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

static void mmu_clone_walk(unsigned long *src_tbl, unsigned long *dst_tbl, int level)
{
    for (int i = 0; i < 512; i++) {
        unsigned long entry = src_tbl[i];
        if (!(entry & 0x1)) continue;

        if (level < 3 && (entry & 0x2)) {
            /* Table entry */
            unsigned long *next_src = (unsigned long *)pa_to_va(entry & MMU_L3_PAGE_ADDR_MASK);
            unsigned long next_dst_pa = (unsigned long)page_alloc();
            if (!next_dst_pa) continue;
            
            unsigned long *next_dst_va = (unsigned long *)pa_to_va(next_dst_pa);
            for (int k = 0; k < 512; k++) next_dst_va[k] = 0;
            
            dst_tbl[i] = (next_dst_pa & MMU_L3_PAGE_ADDR_MASK) | (entry & ~MMU_L3_PAGE_ADDR_MASK);
            mmu_clone_walk(next_src, next_dst_va, level + 1);
        } else if (level == 3) {
            /* L3 Leaf entry - Mark as Read-Only for CoW */
            unsigned long pa = entry & MMU_L3_PAGE_ADDR_MASK;
            page_ref_inc(pa);
            
            /* Ensure it is Read-Only in both */
            dst_tbl[i] = entry | MMU_AP_RO;
            src_tbl[i] = entry | MMU_AP_RO;
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

    mmu_clone_walk(src_root, dst_root, 0);

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
        ttbr0_val = (unsigned long)ttbr0_runtime_empty_root;
    } else {
        ttbr0_val = mm->root_pa | ((unsigned long)mm->asid << 48);
    }

    mmu_set_ttbr0(ttbr0_val);
}

static unsigned long *mmu_find_pte(struct mm_context *mm, unsigned long va, int create)
{
    unsigned long *l0, *l1, *l2, *l3;
    unsigned long idx;
    void *new_page;
    unsigned int i;

    if (!mm) return (unsigned long *)0;

    l0 = (unsigned long *)pa_to_va((void *)mm->root_pa);
    idx = L0_INDEX_FOR(va);
    if ((l0[idx] & 0x1) == 0) {
        if (!create) return (unsigned long *)0;
        new_page = page_alloc();
        if (!new_page) return (unsigned long *)0;
        if (!mmu_context_add_page(mm, (unsigned long)new_page)) { page_free(new_page); return (unsigned long *)0; }
        
        /* Zero the new sub-table */
        for (i = 0; i < 512; i++) {
            ((unsigned long *)pa_to_va(new_page))[i] = 0UL;
        }

        l0[idx] = (unsigned long)new_page | 0x3UL;
    }
    
    l1 = (unsigned long *)pa_to_va((void *)(l0[idx] & 0x0000FFFFFFFFF000UL));
    idx = L1_INDEX_FOR(va);
    if ((l1[idx] & 0x1) == 0) {
        if (!create) return (unsigned long *)0;
        new_page = page_alloc();
        if (!new_page) return (unsigned long *)0;
        if (!mmu_context_add_page(mm, (unsigned long)new_page)) { page_free(new_page); return (unsigned long *)0; }
        
        /* Zero the new sub-table */
        for (i = 0; i < 512; i++) {
            ((unsigned long *)pa_to_va(new_page))[i] = 0UL;
        }

        l1[idx] = (unsigned long)new_page | 0x3UL;
    }

    l2 = (unsigned long *)pa_to_va((void *)(l1[idx] & 0x0000FFFFFFFFF000UL));
    idx = L2_INDEX_FOR(va);
    if ((l2[idx] & 0x1) == 0) {
        if (!create) return (unsigned long *)0;
        new_page = page_alloc();
        if (!new_page) return (unsigned long *)0;
        if (!mmu_context_add_page(mm, (unsigned long)new_page)) { page_free(new_page); return (unsigned long *)0; }
        
        /* Zero the new sub-table */
        for (i = 0; i < 512; i++) {
            ((unsigned long *)pa_to_va(new_page))[i] = 0UL;
        }

        l2[idx] = (unsigned long)new_page | 0x3UL;
    }

    l3 = (unsigned long *)pa_to_va((void *)(l2[idx] & 0x0000FFFFFFFFF000UL));
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

int mmu_unmap_user_page(struct mm_context *mm, unsigned long va)
{
    unsigned long *l3;

    if (mm == (struct mm_context *)0) {
        return 0;
    }

    l3 = mmu_find_pte(mm, va, 0);
    if (!l3) return 0;

    *l3 = 0UL;

    mmu_invalidate_tlb_va(mm->asid, va);

    return 1;
}

int mmu_handle_process_page_fault(struct process *p, unsigned long far_el1, unsigned long esr_el1)
{
    unsigned long fsc;
    unsigned long fsc_type;
    unsigned long page_va;
    void *new_page;
    unsigned long flags;
    unsigned long *entry_ptr;
    unsigned int ec = (unsigned int)(esr_el1 >> 26) & 0x3F;
    unsigned int iss = (unsigned int)esr_el1 & 0x1FFFFFF;

    if (!mmu_is_enabled()) {
        return 0;
    }

    if (p == (struct process *)0 || p->mm == (struct mm_context *)0) {
        return 0;
    }

    fsc = esr_el1 & 0x3FUL;
    fsc_type = fsc & 0x3CUL;

    /* KER_LOGF("[mmu] Fault: VA=%lx ESR=%lx (EC=%x FSC=%x)\n", far_el1, esr_el1, ec, (unsigned int)fsc); */

    /* Handle Permission Fault (0x0C) for Copy-on-Write (CoW) */
    if (fsc_type == 0x0CUL) {
        unsigned long old_pa;
        unsigned long entry;
        void *copy_page;

        if (!mmu_resolve_user_page_desc(p->mm, far_el1, &entry_ptr)) {
            goto cleanup_fatal;
        }

        entry = *entry_ptr;
        if ((entry & MMU_DESC_VALID) != 0UL && 
            (entry & MMU_AP_RO) == MMU_AP_RO &&
            (entry & (1UL << 11)) != 0UL) {
            
            old_pa = entry & MMU_L3_PAGE_ADDR_MASK;
            page_va = far_el1 & ~0xFFFUL;

            if (page_ref_get(old_pa) == 1U) {
                *entry_ptr = (*entry_ptr & ~MMU_AP_RO) | MMU_AP_RW;
                mmu_invalidate_tlb_va(p->mm->asid, page_va);
                KER_LOGF("[mmu] CoW: VA=%lx upgraded to RW (ref=1)\n", page_va);
                return 1;
            }

            copy_page = page_alloc();
            if (copy_page == (void *)0) return 0;

            unsigned char *src = (unsigned char *)pa_to_va(old_pa);
            unsigned char *dst = (unsigned char *)pa_to_va(copy_page);
            for (unsigned long i = 0; i < PAGE_SIZE; i++) dst[i] = src[i];

            *entry_ptr = ((unsigned long)va_to_pa(copy_page) & MMU_L3_PAGE_ADDR_MASK) | 
                         (entry & ~MMU_L3_PAGE_ADDR_MASK & ~MMU_AP_RO) | MMU_AP_RW;

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

        for (i = 0; i < p->region_count; i++) {
            if (far_el1 >= p->regions[i].start && far_el1 < p->regions[i].end) {
                region = &p->regions[i];
                break;
            }
        }

        if (region) {
            page_va = far_el1 & ~0xFFFUL;
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
             page_va = far_el1 & ~0xFFFUL;
        } else if (far_el1 >= (USER_STACK_TOP - 0x100000) && far_el1 < USER_STACK_TOP) {
             /* Reasonable stack range (1MB from top) */
             page_va = far_el1 & ~0xFFFUL;
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
    {
        const char *el = (ec == 0x20 || ec == 0x24) ? "EL0" : "EL1";
        const char *type = "Fault";
        const char *acc = "unknown";

        if (ec == 0x20 || ec == 0x21) {
            type = "Instruction Abort";
            acc = "Fetch";
        } else if (ec == 0x24 || ec == 0x25) {
            type = "Data Abort";
            acc = (iss & (1 << 6)) ? "Write" : "Read";
        }

        KER_LOGF("[p=%p] %s %s abort (%s L%u): FAR=0x%lx ESR=0x%lx\n",
                 p, el, acc, type, (unsigned int)(fsc & 0x3), far_el1, esr_el1);
    }
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

    unsigned long start = va & ~0xFFFUL;
    unsigned long end = (va + len + 0xFFFUL) & ~0xFFFUL;

    for (unsigned long curr = start; curr < end; curr += PAGE_SIZE) {
        unsigned long *pte = mmu_find_pte(mm, curr, 0);
        if (pte && (*pte & 0x1)) {
            unsigned long pa = *pte & 0x0000FFFFFFFFF000UL;
            *pte = 0;
            page_ref_dec(pa);
            mmu_invalidate_tlb_va(mm->asid, curr);
        }
    }
    return 1;
}

#endif /* CONFIG_KERNEL_VIRTUAL */
