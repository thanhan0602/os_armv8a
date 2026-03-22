#include <kernel/heap.h>

#include <kernel/log.h>
#include <kernel/page_alloc.h>

#define KERNEL_HEAP_PAGE_MAGIC 0x4b48454150414745UL
#define KERNEL_HEAP_ALIGNMENT  16UL

struct kernel_heap_block {
    unsigned long size;
    unsigned long is_free;
    struct kernel_heap_block *next;
    struct kernel_heap_block *prev;
};

struct kernel_heap_page {
    unsigned long magic;
    struct kernel_heap_page *next;
    unsigned long usable_bytes;
    struct kernel_heap_block *first_block;
};

static struct kernel_heap_page *kernel_heap_pages;
static unsigned long kernel_heap_pages_used;
static unsigned long kernel_heap_free_space;
static unsigned long kernel_heap_used_space;
static unsigned long kernel_heap_allocations;
static unsigned long kernel_heap_failed;

static unsigned long kernel_heap_align_up(unsigned long value, unsigned long alignment)
{
    return (value + alignment - 1UL) & ~(alignment - 1UL);
}

static unsigned long kernel_heap_min_split_size(void)
{
    return sizeof(struct kernel_heap_block) + KERNEL_HEAP_ALIGNMENT;
}

static struct kernel_heap_page *kernel_heap_page_from_pointer(void *ptr)
{
    unsigned long page_base;
    struct kernel_heap_page *page;

    page_base = ((unsigned long)ptr) & ~(PAGE_SIZE - 1UL);
    page = (struct kernel_heap_page *)page_base;
    if (page->magic != KERNEL_HEAP_PAGE_MAGIC) {
        return (struct kernel_heap_page *)0;
    }

    return page;
}

static struct kernel_heap_page *kernel_heap_add_page(void)
{
    struct kernel_heap_page *page;
    struct kernel_heap_block *block;
    unsigned long block_start;
    unsigned long usable_bytes;

    page = (struct kernel_heap_page *)page_alloc();
    if (page == (struct kernel_heap_page *)0) {
        kernel_heap_failed++;
        return (struct kernel_heap_page *)0;
    }

    block_start = kernel_heap_align_up((unsigned long)page + sizeof(struct kernel_heap_page), KERNEL_HEAP_ALIGNMENT);
    usable_bytes = PAGE_SIZE - (block_start - (unsigned long)page) - sizeof(struct kernel_heap_block);
    block = (struct kernel_heap_block *)block_start;

    page->magic = KERNEL_HEAP_PAGE_MAGIC;
    page->next = kernel_heap_pages;
    page->usable_bytes = usable_bytes;
    page->first_block = block;
    kernel_heap_pages = page;
    kernel_heap_pages_used++;
    kernel_heap_free_space += usable_bytes;

    block->size = usable_bytes;
    block->is_free = 1UL;
    block->next = (struct kernel_heap_block *)0;
    block->prev = (struct kernel_heap_block *)0;

    return page;
}

static void kernel_heap_split_block(struct kernel_heap_block *block, unsigned long size)
{
    struct kernel_heap_block *new_block;
    unsigned long remaining_size;
    unsigned long new_block_address;

    if (block->size < (size + kernel_heap_min_split_size())) {
        return;
    }

    remaining_size = block->size - size - sizeof(struct kernel_heap_block);
    new_block_address = (unsigned long)(block + 1) + size;
    new_block = (struct kernel_heap_block *)new_block_address;

    new_block->size = remaining_size;
    new_block->is_free = 1UL;
    new_block->next = block->next;
    new_block->prev = block;

    if (new_block->next != (struct kernel_heap_block *)0) {
        new_block->next->prev = new_block;
    }

    block->size = size;
    block->next = new_block;
}

static void *kernel_heap_allocate_from_page(struct kernel_heap_page *page, unsigned long size)
{
    struct kernel_heap_block *block;

    block = page->first_block;
    while (block != (struct kernel_heap_block *)0) {
        if (block->is_free != 0UL && block->size >= size) {
            kernel_heap_split_block(block, size);
            block->is_free = 0UL;
            kernel_heap_free_space -= block->size;
            kernel_heap_used_space += block->size;
            kernel_heap_allocations++;
            return (void *)(block + 1);
        }

        block = block->next;
    }

    return (void *)0;
}

static void kernel_heap_coalesce(struct kernel_heap_block *block)
{
    if (block->next != (struct kernel_heap_block *)0 && block->next->is_free != 0UL) {
        block->size += sizeof(struct kernel_heap_block) + block->next->size;
        block->next = block->next->next;
        if (block->next != (struct kernel_heap_block *)0) {
            block->next->prev = block;
        }
    }

    if (block->prev != (struct kernel_heap_block *)0 && block->prev->is_free != 0UL) {
        block->prev->size += sizeof(struct kernel_heap_block) + block->size;
        block->prev->next = block->next;
        if (block->next != (struct kernel_heap_block *)0) {
            block->next->prev = block->prev;
        }
    }
}

void kernel_heap_init(void)
{
    kernel_heap_pages = (struct kernel_heap_page *)0;
    kernel_heap_pages_used = 0UL;
    kernel_heap_free_space = 0UL;
    kernel_heap_used_space = 0UL;
    kernel_heap_allocations = 0UL;
    kernel_heap_failed = 0UL;

    if (kernel_heap_add_page() == (struct kernel_heap_page *)0) {
        log_info("kernel heap init failed");
        return;
    }

    log_info("stage 7 kernel heap online");
}

void *kmalloc(unsigned long size)
{
    struct kernel_heap_page *page;
    void *allocation;
    unsigned long aligned_size;

    if (size == 0UL) {
        return (void *)0;
    }

    aligned_size = kernel_heap_align_up(size, KERNEL_HEAP_ALIGNMENT);
    if (aligned_size > (PAGE_SIZE - sizeof(struct kernel_heap_page) - sizeof(struct kernel_heap_block) - KERNEL_HEAP_ALIGNMENT)) {
        kernel_heap_failed++;
        return (void *)0;
    }

    page = kernel_heap_pages;
    while (page != (struct kernel_heap_page *)0) {
        allocation = kernel_heap_allocate_from_page(page, aligned_size);
        if (allocation != (void *)0) {
            return allocation;
        }

        page = page->next;
    }

    page = kernel_heap_add_page();
    if (page == (struct kernel_heap_page *)0) {
        return (void *)0;
    }

    return kernel_heap_allocate_from_page(page, aligned_size);
}

void kfree(void *ptr)
{
    struct kernel_heap_page *page;
    struct kernel_heap_block *block;

    if (ptr == (void *)0) {
        return;
    }

    page = kernel_heap_page_from_pointer(ptr);
    if (page == (struct kernel_heap_page *)0) {
        log_write("[warn] ignoring invalid heap free: ");
        log_write_hex((unsigned long)ptr);
        log_putc('\n');
        return;
    }

    block = ((struct kernel_heap_block *)ptr) - 1;
    if (block->is_free != 0UL) {
        log_write("[warn] ignoring duplicate heap free: ");
        log_write_hex((unsigned long)ptr);
        log_putc('\n');
        return;
    }

    block->is_free = 1UL;
    kernel_heap_used_space -= block->size;
    kernel_heap_free_space += block->size;
    kernel_heap_coalesce(block);
}

unsigned long kernel_heap_total_pages(void)
{
    return kernel_heap_pages_used;
}

unsigned long kernel_heap_free_bytes(void)
{
    return kernel_heap_free_space;
}

unsigned long kernel_heap_used_bytes(void)
{
    return kernel_heap_used_space;
}

unsigned long kernel_heap_allocation_count(void)
{
    return kernel_heap_allocations;
}

unsigned long kernel_heap_failed_allocations(void)
{
    return kernel_heap_failed;
}

void kernel_heap_log_stats(void)
{
    log_write("[info] heap pages=");
    log_write_u64(kernel_heap_total_pages());
    log_write(" used_bytes=");
    log_write_u64(kernel_heap_used_bytes());
    log_write(" free_bytes=");
    log_write_u64(kernel_heap_free_bytes());
    log_write(" allocations=");
    log_write_u64(kernel_heap_allocation_count());
    log_write(" failed_allocations=");
    log_write_u64(kernel_heap_failed_allocations());
    log_putc('\n');
}