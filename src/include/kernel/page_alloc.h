#ifndef KERNEL_PAGE_ALLOC_H
#define KERNEL_PAGE_ALLOC_H

#define PAGE_SIZE 4096UL

void page_allocator_init(void);
void *page_alloc(void);
void page_free(void *page);
unsigned long page_allocator_total_pages(void);
unsigned long page_allocator_free_pages(void);
unsigned long page_allocator_reserved_bytes(void);
unsigned long page_allocator_invalid_free_count(void);
unsigned long page_allocator_double_free_count(void);
const char *page_allocator_page_state_name(unsigned long address);
void page_allocator_log_page_state(unsigned long address);
void page_allocator_log_page_range(unsigned long start_address, unsigned long page_count);
unsigned long page_allocator_check_consistency(void);
void page_allocator_log_consistency(void);
unsigned long page_allocator_managed_start(void);
void page_allocator_log_managed_head(unsigned long page_count);

#endif