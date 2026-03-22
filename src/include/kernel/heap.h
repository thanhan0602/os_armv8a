#ifndef KERNEL_HEAP_H
#define KERNEL_HEAP_H

/*
 * Small kernel heap layered on top of the physical page allocator.
 *
 * Current model:
 * - each heap page comes from page_alloc()
 * - allocations are carved from page-local blocks
 * - frees coalesce with adjacent free blocks inside the same page
 * - single allocations cannot currently span multiple pages
 */
void kernel_heap_init(void);
void *kmalloc(unsigned long size);
void kfree(void *ptr);

/* Aggregate heap statistics exposed for boot-time verification. */
unsigned long kernel_heap_total_pages(void);
unsigned long kernel_heap_free_bytes(void);
unsigned long kernel_heap_used_bytes(void);
unsigned long kernel_heap_allocation_count(void);
unsigned long kernel_heap_failed_allocations(void);
void kernel_heap_log_stats(void);

#endif