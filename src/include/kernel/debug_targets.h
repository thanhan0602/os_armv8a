#ifndef KERNEL_DEBUG_TARGETS_H
#define KERNEL_DEBUG_TARGETS_H

void kernel_debug_log_pre_mmu_targets(unsigned long page_a, unsigned long page_b);
void kernel_debug_log_mmu_boot_targets(void);
void kernel_debug_log_post_mmu_targets(unsigned long walk_address);
void kernel_debug_log_heap_targets(void);

#endif