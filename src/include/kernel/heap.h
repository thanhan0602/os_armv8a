#ifndef KERNEL_HEAP_H
#define KERNEL_HEAP_H

void kernel_heap_init(void);
void *kmalloc(unsigned long size);
void kfree(void *ptr);
unsigned long kernel_heap_total_pages(void);
unsigned long kernel_heap_free_bytes(void);
unsigned long kernel_heap_used_bytes(void);
unsigned long kernel_heap_allocation_count(void);
unsigned long kernel_heap_failed_allocations(void);
void kernel_heap_log_stats(void);

#endif