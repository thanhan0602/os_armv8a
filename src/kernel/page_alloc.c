#include <kernel/page_alloc.h>

#include <arch/arm/virt.h>
#include <kernel/log.h>

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

static struct page_node *page_free_list;
static unsigned long total_pages;
static unsigned long free_pages;
static unsigned long reserved_bytes;
static unsigned long managed_start;
static unsigned long managed_end;
static unsigned long invalid_free_count;
static unsigned long double_free_count;
static unsigned char page_state[QEMU_VIRT_MAX_PAGES];

/* Align the managed region to whole pages so allocator clients never see partial pages. */
static unsigned long align_up(unsigned long value, unsigned long alignment)
{
    return (value + alignment - 1UL) & ~(alignment - 1UL);
}

static void zero_page(void *page)
{
    unsigned long *words;
    unsigned long index;

    words = (unsigned long *)page;
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
    log_write("[warn] ");
    log_write(message);
    if (address != 0UL) {
        log_write(": ");
        log_write_hex(address);
    }
    log_putc('\n');
}

static unsigned long count_free_list_nodes(void)
{
    struct page_node *node;
    unsigned long count;

    count = 0UL;
    node = page_free_list;
    while (node != (struct page_node *)0) {
        count++;
        node = node->next;
    }

    return count;
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
    managed_start = align_up((unsigned long)__kernel_end, PAGE_SIZE);
    managed_end = ram_end;
    reserved_bytes = managed_start - QEMU_VIRT_RAM_BASE;

    page_free_list = (struct page_node *)0;
    total_pages = 0UL;
    free_pages = 0UL;
    invalid_free_count = 0UL;
    double_free_count = 0UL;

    for (page_index = 0UL; page_index < QEMU_VIRT_MAX_PAGES; page_index++) {
        page_state[page_index] = PAGE_STATE_UNUSED;
    }

    /*
     * Build the initial free list by pushing every managed page. The list order
     * is not important yet; simplicity matters more than locality in early boot.
     */
    for (page_addr = managed_start; (page_addr + PAGE_SIZE) <= managed_end; page_addr += PAGE_SIZE) {
        struct page_node *page;

        page = (struct page_node *)page_addr;
        page->next = page_free_list;
        page_free_list = page;
        page_state[page_index_from_address(page_addr)] = PAGE_STATE_FREE;
        total_pages++;
        free_pages++;
    }

    log_write("[info] ram base=");
    log_write_hex(QEMU_VIRT_RAM_BASE);
    log_putc('\n');
    log_write("[info] ram end=");
    log_write_hex(ram_end);
    log_putc('\n');
    log_write("[info] page allocator start=");
    log_write_hex(managed_start);
    log_putc('\n');
    log_write("[info] page allocator pages=");
    log_write_u64(total_pages);
    log_putc('\n');
}

void *page_alloc(void)
{
    struct page_node *page;

    /* Pop from the head of the free list, mark allocated, then scrub the page. */
    page = page_free_list;
    if (page == (struct page_node *)0) {
        return (void *)0;
    }

    page_free_list = page->next;
    page_state[page_index_from_address((unsigned long)page)] = PAGE_STATE_ALLOCATED;
    free_pages--;
    zero_page((void *)page);
    return (void *)page;
}

void page_free(void *page)
{
    struct page_node *node;
    unsigned long page_addr;

    /* Reject anything that is not a managed, page-aligned allocation. */
    if (page == (void *)0) {
        return;
    }

    page_addr = (unsigned long)page;
    if ((page_addr & (PAGE_SIZE - 1UL)) != 0UL) {
        invalid_free_count++;
        page_allocator_warn("ignoring unaligned page free", page_addr);
        return;
    }

    if (page_addr < managed_start || page_addr >= managed_end) {
        invalid_free_count++;
        page_allocator_warn("ignoring out-of-range page free", page_addr);
        return;
    }

    if (page_state[page_index_from_address(page_addr)] != PAGE_STATE_ALLOCATED) {
        double_free_count++;
        page_allocator_warn("ignoring duplicate or invalid page free", page_addr);
        return;
    }

    /* Return the page to the free-list head and restore its metadata to FREE. */
    node = (struct page_node *)page;
    node->next = page_free_list;
    page_free_list = node;
    page_state[page_index_from_address(page_addr)] = PAGE_STATE_FREE;
    free_pages++;
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

    return page_state_name_from_value(page_state[page_index_from_address(address)]);
}

void page_allocator_log_page_state(unsigned long address)
{
    log_write("[info] page state addr=");
    log_write_hex(address);

    if ((address & (PAGE_SIZE - 1UL)) != 0UL) {
        log_write(" state=unaligned");
        log_putc('\n');
        return;
    }

    if (!page_address_is_in_ram(address)) {
        log_write(" state=out-of-range");
        log_putc('\n');
        return;
    }

    log_write(" index=");
    log_write_u64(page_index_from_address(address));
    log_write(" state=");
    log_write(page_allocator_page_state_name(address));

    if (address < managed_start || address >= managed_end) {
        log_write(" managed=no");
    } else {
        log_write(" managed=yes");
    }

    log_putc('\n');
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

        state = page_state[page_index_from_address(page_addr)];
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

        state = page_state[page_index_from_address(page_addr)];
        if (state == PAGE_STATE_FREE) {
            state_free_pages++;
        } else if (state == PAGE_STATE_ALLOCATED) {
            state_allocated_pages++;
        }
    }

    free_list_nodes = count_free_list_nodes();
    mismatches = page_allocator_check_consistency();

    log_write("[info] page allocator consistency mismatches=");
    log_write_u64(mismatches);
    log_write(" free_list_nodes=");
    log_write_u64(free_list_nodes);
    log_write(" state_free=");
    log_write_u64(state_free_pages);
    log_write(" state_allocated=");
    log_write_u64(state_allocated_pages);
    log_write(" tracked_free_pages=");
    log_write_u64(free_pages);
    log_write(" total_pages=");
    log_write_u64(total_pages);
    log_putc('\n');
}

unsigned long page_allocator_managed_start(void)
{
    return managed_start;
}

void page_allocator_log_managed_head(unsigned long page_count)
{
    log_write("[info] page managed head count=");
    log_write_u64(page_count);
    log_write(" start=");
    log_write_hex(managed_start);
    log_putc('\n');
    page_allocator_log_page_range(managed_start, page_count);
}