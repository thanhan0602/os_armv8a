#include <kernel/page_alloc.h>

#include <arch/arm/virt.h>
#include <kernel/log.h>
#include <kernel/mmu.h>
#include <kernel/vm.h>
#include <kernel/spinlock.h>

/*
 * Page allocator state model:
 * - UNUSED    = page is outside the managed allocator window
 * - FREE      = page is currently reachable from the free list
 * - ALLOCATED = page has been handed out to a subsystem
 *
 * The page_state[] array is intentionally redundant with the free list so the
 * kernel can detect bookkeeping drift during bring-up.
 */
#define PAGE_STATE_UNUSED    0U
#define PAGE_STATE_FREE      1U
#define PAGE_STATE_ALLOCATED 2U
#define QEMU_VIRT_MAX_PAGES  (QEMU_VIRT_RAM_SIZE / PAGE_SIZE)

/* Free-list link stored in the physical page itself while the page is free. */
struct page_node {
    struct page_node *next;
};

extern char __kernel_end[];

static unsigned long page_free_list_pa;
static unsigned long total_pages;
static unsigned long free_pages;
static unsigned long reserved_bytes;
static unsigned long managed_start;
static unsigned long managed_end;
static unsigned long invalid_free_count;
static unsigned long double_free_count;
static struct spinlock page_lock = SPINLOCK_INITIALIZER;

struct page {
    unsigned char state;
    unsigned short ref_count;
};

static struct page pages[QEMU_VIRT_MAX_PAGES];

/* Align the managed region to whole pages so allocator clients never see partial pages. */
static unsigned long align_up(unsigned long value, unsigned long alignment)
{
    return (value + alignment - 1UL) & ~(alignment - 1UL);
}

/*
 * Convert a physical page address to a pointer that can be safely
 * dereferenced at the current execution context:
 *
 * - Before the MMU is enabled, PA == usable pointer (identity mapping or
 *   no translation at all).
 * - After the MMU is enabled, the page must be reached through the
 *   TTBR1 kernel VA (PA + KERNEL_VA_OFFSET).
 */
static void *page_pa_to_ptr(unsigned long pa)
{
    if (mmu_is_enabled()) {
        return pa_to_va(pa);
    }

    return (void *)pa;
}

/*
 * Convert a pointer back to a physical address.  The inverse of
 * page_pa_to_ptr(): strips KERNEL_VA_OFFSET when the MMU is active,
 * or is a plain cast when it is not.
 */
static unsigned long page_ptr_to_pa(const void *ptr)
{
    if (mmu_is_enabled() && (unsigned long)ptr >= KERNEL_VA_OFFSET) {
        return va_to_pa(ptr);
    }

    return (unsigned long)ptr;
}

static void zero_page(unsigned long page_pa)
{
    unsigned long *words;
    unsigned long index;

    words = (unsigned long *)page_pa_to_ptr(page_pa);
    for (index = 0; index < (PAGE_SIZE / sizeof(unsigned long)); index++) {
        words[index] = 0UL;
    }
}

static unsigned long page_index_from_address(unsigned long address)
{
    return (address - QEMU_VIRT_RAM_BASE) / PAGE_SIZE;
}

static int page_address_is_in_ram(unsigned long address)
{
    return address >= QEMU_VIRT_RAM_BASE && address < (QEMU_VIRT_RAM_BASE + QEMU_VIRT_RAM_SIZE);
}

static const char *page_state_name_from_value(unsigned char state)
{
    if (state == PAGE_STATE_UNUSED) {
        return "unused";
    }

    if (state == PAGE_STATE_FREE) {
        return "free";
    }

    if (state == PAGE_STATE_ALLOCATED) {
        return "allocated";
    }

    return "unknown";
}

static void page_allocator_warn(const char *message, unsigned long address)
{
    if (address != 0UL) {
        KER_LOGF("[warn] %s: %lx\n", message, address);
    } else {
        KER_LOGF("[warn] %s\n", message);
    }
}

static unsigned long count_free_list_nodes(void)
{
    unsigned long node_pa;
    unsigned long count;

    count = 0UL;
    node_pa = page_free_list_pa;
    while (node_pa != 0UL) {
        struct page_node *node;

        node = (struct page_node *)page_pa_to_ptr(node_pa);
        count++;
        node_pa = page_ptr_to_pa(node->next);
    }

    return count;
}

static void page_allocator_rebuild_free_list(void)
{
    unsigned long page_addr;

    page_free_list_pa = 0UL;
    for (page_addr = managed_end - PAGE_SIZE; page_addr >= managed_start; page_addr -= PAGE_SIZE) {
        if (pages[page_index_from_address(page_addr)].state == PAGE_STATE_FREE) {
            struct page_node *page;

            page = (struct page_node *)page_pa_to_ptr(page_addr);
            page->next = (struct page_node *)page_free_list_pa;
            page_free_list_pa = page_addr;
        }

        if (page_addr == managed_start) {
            break;
        }
    }
}

void page_allocator_init(void)
{
    unsigned long page_addr;
    unsigned long ram_end;
    unsigned long page_index;

    /*
     * The allocator owns RAM from the first page after the kernel image to the
     * end of the QEMU virt RAM window. Earlier bytes stay reserved for kernel
     * text/data/bss/boot stack and must never re-enter the free list.
     */
    ram_end = QEMU_VIRT_RAM_BASE + QEMU_VIRT_RAM_SIZE;
    managed_start = align_up((unsigned long)va_to_pa(__kernel_end), PAGE_SIZE);
    managed_end = ram_end;
    reserved_bytes = managed_start - QEMU_VIRT_RAM_BASE;

    page_free_list_pa = 0UL;
    total_pages = 0UL;
    free_pages = 0UL;
    invalid_free_count = 0UL;
    double_free_count = 0UL;

    for (page_index = 0UL; page_index < QEMU_VIRT_MAX_PAGES; page_index++) {
        pages[page_index].state = PAGE_STATE_UNUSED;
        pages[page_index].ref_count = 0U;
    }

    /*
     * Build the initial free list by pushing every managed page. The list order
     * is not important yet; simplicity matters more than locality in early boot.
     */
    for (page_addr = managed_start; (page_addr + PAGE_SIZE) <= managed_end; page_addr += PAGE_SIZE) {
        struct page_node *page;

        page = (struct page_node *)page_pa_to_ptr(page_addr);
        page->next = (struct page_node *)page_free_list_pa;
        page_free_list_pa = page_addr;
        pages[page_index_from_address(page_addr)].state = PAGE_STATE_FREE;
        total_pages++;
        free_pages++;
    }

    KER_LOGF("[info] page_alloc init: managed_start=%lx managed_end=%lx total_pages=%lu\n",
             managed_start, managed_end, total_pages);
}

void *page_alloc(void)
{
    struct page_node *page;
    unsigned long page_pa;

    unsigned long flags = spin_lock_irqsave(&page_lock);

    /* Pop from the head of the free list, mark allocated, then scrub the page. */
    page_pa = page_free_list_pa;
    if (page_pa == 0UL) {
        spin_unlock_irqrestore(&page_lock, flags);
        return (void *)0;
    }

    page = (struct page_node *)page_pa_to_ptr(page_pa);
    page_free_list_pa = page_ptr_to_pa(page->next);
    unsigned long idx = page_index_from_address(page_pa);
    pages[idx].state = PAGE_STATE_ALLOCATED;
    pages[idx].ref_count = 1U;
    free_pages--;

    spin_unlock_irqrestore(&page_lock, flags);

    zero_page(page_pa);
    return (void *)page_pa;
}

void *page_alloc_contiguous(unsigned long page_count)
{
    unsigned long run_start;
    unsigned long run_length;
    unsigned long page_addr;
    unsigned long offset;

    unsigned long flags = spin_lock_irqsave(&page_lock);

    if (page_count == 0UL || page_count > free_pages) {
        spin_unlock_irqrestore(&page_lock, flags);
        return (void *)0;
    }

    run_start = 0UL;
    run_length = 0UL;

    for (page_addr = managed_start; page_addr < managed_end; page_addr += PAGE_SIZE) {
        unsigned long idx = page_index_from_address(page_addr);
        if (pages[idx].state == PAGE_STATE_FREE) {
            if (run_length == 0UL) {
                run_start = page_addr;
            }

            run_length++;
            if (run_length == page_count) {
                for (offset = 0UL; offset < page_count; offset++) {
                    unsigned long alloc_addr;
                    unsigned long alloc_idx;

                    alloc_addr = run_start + (offset * PAGE_SIZE);
                    alloc_idx = page_index_from_address(alloc_addr);
                    pages[alloc_idx].state = PAGE_STATE_ALLOCATED;
                    pages[alloc_idx].ref_count = 1U;
                    zero_page(alloc_addr);
                }

                free_pages -= page_count;
                page_allocator_rebuild_free_list();
                spin_unlock_irqrestore(&page_lock, flags);
                return (void *)run_start;
            }
        } else {
            run_length = 0UL;
        }
    }

    spin_unlock_irqrestore(&page_lock, flags);
    return (void *)0;
}

void page_free(void *page)
{
    struct page_node *node;
    unsigned long page_addr;
    unsigned int idx;

    unsigned long flags = spin_lock_irqsave(&page_lock);

    /* Reject anything that is not a managed, page-aligned allocation. */
    if (page == (void *)0) {
        spin_unlock_irqrestore(&page_lock, flags);
        return;
    }

    page_addr = (unsigned long)page;
    if ((page_addr & (PAGE_SIZE - 1UL)) != 0UL) {
        invalid_free_count++;
        spin_unlock_irqrestore(&page_lock, flags);
        page_allocator_warn("ignoring unaligned page free", page_addr);
        return;
    }

    if (page_addr < managed_start || page_addr >= managed_end) {
        invalid_free_count++;
        spin_unlock_irqrestore(&page_lock, flags);
        page_allocator_warn("ignoring out-of-range page free", page_addr);
        return;
    }

    idx = page_index_from_address(page_addr);
    if (pages[idx].state != PAGE_STATE_ALLOCATED) {
        double_free_count++;
        spin_unlock_irqrestore(&page_lock, flags);
        page_allocator_warn("ignoring duplicate or invalid page free", page_addr);
        return;
    }

    /* Reference counting check: if this page is shared, just decrement. */
    if (pages[idx].ref_count > 1U) {
        pages[idx].ref_count--;
        spin_unlock_irqrestore(&page_lock, flags);
        return;
    }

    /* Return the page to the free-list head and restore its metadata to FREE. */
    pages[idx].ref_count = 0U;
    node = (struct page_node *)page_pa_to_ptr(page_addr);
    node->next = (struct page_node *)page_free_list_pa;
    page_free_list_pa = page_addr;
    pages[idx].state = PAGE_STATE_FREE;
    free_pages++;

    spin_unlock_irqrestore(&page_lock, flags);
}

void page_ref_inc(unsigned long pa)
{
    unsigned long flags = spin_lock_irqsave(&page_lock);
    unsigned int idx = page_index_from_address(pa);
    if (pa >= managed_start && pa < managed_end) {
        pages[idx].ref_count++;
    }
    spin_unlock_irqrestore(&page_lock, flags);
}

int page_ref_dec(unsigned long pa)
{
    unsigned long flags = spin_lock_irqsave(&page_lock);
    unsigned int idx = page_index_from_address(pa);
    if (pa < managed_start || pa >= managed_end) {
        spin_unlock_irqrestore(&page_lock, flags);
        return 0;
    }
    if (pages[idx].ref_count > 0) {
        pages[idx].ref_count--;
    }
    int ref = (int)pages[idx].ref_count;
    spin_unlock_irqrestore(&page_lock, flags);
    return ref;
}

unsigned int page_ref_get(unsigned long pa)
{
    unsigned int ref;
    unsigned long flags = spin_lock_irqsave(&page_lock);
    unsigned int idx = page_index_from_address(pa);
    if (pa < managed_start || pa >= managed_end) {
        spin_unlock_irqrestore(&page_lock, flags);
        return 0;
    }
    ref = (unsigned int)pages[idx].ref_count;
    spin_unlock_irqrestore(&page_lock, flags);
    return ref;
}

void page_free_contiguous(void *page, unsigned long page_count)
{
    unsigned long page_addr;
    unsigned long offset;

    if (page == (void *)0 || page_count == 0UL) {
        return;
    }

    page_addr = (unsigned long)page;
    if ((page_addr & (PAGE_SIZE - 1UL)) != 0UL) {
        invalid_free_count++;
        page_allocator_warn("ignoring unaligned contiguous page free", page_addr);
        return;
    }

    if (page_addr < managed_start || (page_addr + (page_count * PAGE_SIZE)) > managed_end) {
        invalid_free_count++;
        page_allocator_warn("ignoring out-of-range contiguous page free", page_addr);
        return;
    }

    for (offset = 0UL; offset < page_count; offset++) {
        unsigned long free_addr;

        free_addr = page_addr + (offset * PAGE_SIZE);
        if (pages[page_index_from_address(free_addr)].state != PAGE_STATE_ALLOCATED) {
            double_free_count++;
            page_allocator_warn("ignoring duplicate or invalid contiguous page free", free_addr);
            return;
        }
    }

    for (offset = 0UL; offset < page_count; offset++) {
        unsigned long free_addr;

        free_addr = page_addr + (offset * PAGE_SIZE);
        pages[page_index_from_address(free_addr)].state = PAGE_STATE_FREE;
    }

    free_pages += page_count;
    page_allocator_rebuild_free_list();
}

unsigned long page_allocator_total_pages(void)
{
    return total_pages;
}

unsigned long page_allocator_free_pages(void)
{
    return free_pages;
}

unsigned long page_allocator_reserved_bytes(void)
{
    return reserved_bytes;
}

unsigned long page_allocator_invalid_free_count(void)
{
    return invalid_free_count;
}

unsigned long page_allocator_double_free_count(void)
{
    return double_free_count;
}

const char *page_allocator_page_state_name(unsigned long address)
{
    if ((address & (PAGE_SIZE - 1UL)) != 0UL) {
        return "unaligned";
    }

    if (!page_address_is_in_ram(address)) {
        return "out-of-range";
    }

    return page_state_name_from_value(pages[page_index_from_address(address)].state);
}

void page_allocator_log_page_state(unsigned long address)
{
    if ((address & (PAGE_SIZE - 1UL)) != 0UL) {
        KER_LOGF("[info] page state addr=%lx state=unaligned\n", address);
        return;
    }

    if (!page_address_is_in_ram(address)) {
        KER_LOGF("[info] page state addr=%lx state=out-of-range\n", address);
        return;
    }

    KER_LOGF("[info] page state addr=%lx index=%lu state=%s managed=%s\n",
             address,
             page_index_from_address(address),
             page_allocator_page_state_name(address),
             (address < managed_start || address >= managed_end) ? "no" : "yes");
}

void page_allocator_log_page_range(unsigned long start_address, unsigned long page_count)
{
    unsigned long page_addr;
    unsigned long index;

    page_addr = start_address;
    for (index = 0UL; index < page_count; index++) {
        page_allocator_log_page_state(page_addr);
        page_addr += PAGE_SIZE;
    }
}

unsigned long page_allocator_check_consistency(void)
{
    unsigned long page_addr;
    unsigned long state_free_pages;
    unsigned long state_allocated_pages;
    unsigned long free_list_nodes;
    unsigned long mismatches;

    state_free_pages = 0UL;
    state_allocated_pages = 0UL;
    mismatches = 0UL;

    /* Cross-check page_state[], counter totals, and the actual free-list length. */
    for (page_addr = managed_start; page_addr < managed_end; page_addr += PAGE_SIZE) {
        unsigned char state;

        state = pages[page_index_from_address(page_addr)].state;
        if (state == PAGE_STATE_FREE) {
            state_free_pages++;
        } else if (state == PAGE_STATE_ALLOCATED) {
            state_allocated_pages++;
        } else {
            mismatches++;
        }
    }

    if ((state_free_pages + state_allocated_pages) != total_pages) {
        mismatches++;
    }

    if (state_free_pages != free_pages) {
        mismatches++;
    }

    free_list_nodes = count_free_list_nodes();
    if (free_list_nodes != free_pages) {
        mismatches++;
    }

    return mismatches;
}

void page_allocator_log_consistency(void)
{
    unsigned long page_addr;
    unsigned long state_free_pages;
    unsigned long state_allocated_pages;
    unsigned long free_list_nodes;
    unsigned long mismatches;

    state_free_pages = 0UL;
    state_allocated_pages = 0UL;

    for (page_addr = managed_start; page_addr < managed_end; page_addr += PAGE_SIZE) {
        unsigned char state;

        state = pages[page_index_from_address(page_addr)].state;
        if (state == PAGE_STATE_FREE) {
            state_free_pages++;
        } else if (state == PAGE_STATE_ALLOCATED) {
            state_allocated_pages++;
        }
    }

    free_list_nodes = count_free_list_nodes();
    mismatches = page_allocator_check_consistency();

    KER_LOGF("[info] page allocator consistency mismatches=%lu free_list_nodes=%lu state_free=%lu state_allocated=%lu tracked_free_pages=%lu total_pages=%lu\n",
             mismatches, free_list_nodes, state_free_pages, state_allocated_pages,
             free_pages, total_pages);
}

unsigned long page_allocator_managed_start(void)
{
    return managed_start;
}

void page_allocator_log_managed_head(unsigned long page_count)
{
    KER_LOGF("[info] page managed head count=%lu start=%lx\n", page_count, managed_start);
    page_allocator_log_page_range(managed_start, page_count);
}