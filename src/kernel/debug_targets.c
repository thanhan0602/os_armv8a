#include <kernel/debug_targets.h>

#include <kernel/heap.h>
#include <kernel/log.h>
#include <kernel/mmu.h>
#include <kernel/mmu_debug.h>
#include <kernel/mmu_table.h>
#include <kernel/page_alloc.h>
#include <kernel/vm.h>

#ifndef KERNEL_DEBUG_ENABLE_PRE_MMU
#define KERNEL_DEBUG_ENABLE_PRE_MMU 1
#endif

#ifndef KERNEL_DEBUG_ENABLE_MMU_BOOT
#define KERNEL_DEBUG_ENABLE_MMU_BOOT 1
#endif

#ifndef KERNEL_DEBUG_ENABLE_POST_MMU
#define KERNEL_DEBUG_ENABLE_POST_MMU 1
#endif

struct kernel_debug_target {
    unsigned long phase;
    unsigned long kind;
    unsigned long address_source;
    const char *name;
    unsigned long page_count;
};

#define KERNEL_DEBUG_PHASE_PRE_MMU       1UL
#define KERNEL_DEBUG_PHASE_MMU_BOOT      2UL
#define KERNEL_DEBUG_PHASE_POST_MMU      3UL
#define KERNEL_DEBUG_PHASE_HEAP          4UL

#define KERNEL_DEBUG_TARGET_PAGE_RANGE    1UL
#define KERNEL_DEBUG_TARGET_MANAGED_HEAD  2UL
#define KERNEL_DEBUG_TARGET_MMU_TABLES    3UL
#define KERNEL_DEBUG_TARGET_MMU_WALK      4UL
#define KERNEL_DEBUG_TARGET_MMU_PROBE     5UL
#define KERNEL_DEBUG_TARGET_MMU_BOOT_WALKS 6UL
#define KERNEL_DEBUG_TARGET_MMU_BOOT_PROBES 7UL
#define KERNEL_DEBUG_TARGET_HEAP_ARENAS   8UL

#define KERNEL_DEBUG_ADDRESS_NONE         0UL
#define KERNEL_DEBUG_ADDRESS_PAGE_A       1UL
#define KERNEL_DEBUG_ADDRESS_PAGE_B       2UL
#define KERNEL_DEBUG_ADDRESS_WALK         3UL

static const struct kernel_debug_target kernel_debug_targets[] = {
    { KERNEL_DEBUG_PHASE_PRE_MMU, KERNEL_DEBUG_TARGET_MANAGED_HEAD, KERNEL_DEBUG_ADDRESS_NONE, "managed-head", 4UL },
    { KERNEL_DEBUG_PHASE_PRE_MMU, KERNEL_DEBUG_TARGET_PAGE_RANGE, KERNEL_DEBUG_ADDRESS_PAGE_A, "page-a", 1UL },
    { KERNEL_DEBUG_PHASE_PRE_MMU, KERNEL_DEBUG_TARGET_PAGE_RANGE, KERNEL_DEBUG_ADDRESS_PAGE_B, "page-b", 1UL },
    { KERNEL_DEBUG_PHASE_PRE_MMU, KERNEL_DEBUG_TARGET_PAGE_RANGE, KERNEL_DEBUG_ADDRESS_PAGE_B, "alloc-window", 2UL },
    { KERNEL_DEBUG_PHASE_MMU_BOOT, KERNEL_DEBUG_TARGET_MMU_BOOT_WALKS, KERNEL_DEBUG_ADDRESS_NONE, "mmu-boot-walks", 0UL },
    { KERNEL_DEBUG_PHASE_MMU_BOOT, KERNEL_DEBUG_TARGET_MMU_BOOT_PROBES, KERNEL_DEBUG_ADDRESS_NONE, "mmu-boot-probes", 0UL },
    { KERNEL_DEBUG_PHASE_POST_MMU, KERNEL_DEBUG_TARGET_MANAGED_HEAD, KERNEL_DEBUG_ADDRESS_NONE, "managed-head", 4UL },
    { KERNEL_DEBUG_PHASE_POST_MMU, KERNEL_DEBUG_TARGET_MMU_TABLES, KERNEL_DEBUG_ADDRESS_NONE, "mmu-tables", 0UL },
    { KERNEL_DEBUG_PHASE_POST_MMU, KERNEL_DEBUG_TARGET_MMU_WALK, KERNEL_DEBUG_ADDRESS_WALK, "mmu-walk", 1UL },
    { KERNEL_DEBUG_PHASE_POST_MMU, KERNEL_DEBUG_TARGET_MMU_PROBE, KERNEL_DEBUG_ADDRESS_WALK, "mmu-probe", 1UL },
    { KERNEL_DEBUG_PHASE_HEAP, KERNEL_DEBUG_TARGET_HEAP_ARENAS, KERNEL_DEBUG_ADDRESS_NONE, "heap-arenas", 1UL },
    { KERNEL_DEBUG_PHASE_HEAP, KERNEL_DEBUG_TARGET_HEAP_ARENAS, KERNEL_DEBUG_ADDRESS_NONE, "heap-large-arenas", 2UL },
};

static unsigned long kernel_debug_resolve_address(unsigned long source, unsigned long page_a, unsigned long page_b, unsigned long walk_address)
{
    if (source == KERNEL_DEBUG_ADDRESS_PAGE_A) {
        return page_a;
    }

    if (source == KERNEL_DEBUG_ADDRESS_PAGE_B) {
        return page_b;
    }

    if (source == KERNEL_DEBUG_ADDRESS_WALK) {
        return walk_address;
    }

    return 0UL;
}

static const char *kernel_debug_target_name(const char *name)
{
    if (mmu_is_enabled()) {
        return (const char *)pa_to_va(name);
    }

    return name;
}

static int kernel_debug_phase_enabled(unsigned long phase)
{
    if (phase == KERNEL_DEBUG_PHASE_PRE_MMU) {
        return KERNEL_DEBUG_ENABLE_PRE_MMU != 0;
    }

    if (phase == KERNEL_DEBUG_PHASE_MMU_BOOT) {
        return KERNEL_DEBUG_ENABLE_MMU_BOOT != 0;
    }

    if (phase == KERNEL_DEBUG_PHASE_POST_MMU) {
        return KERNEL_DEBUG_ENABLE_POST_MMU != 0;
    }

    if (phase == KERNEL_DEBUG_PHASE_HEAP) {
        return KERNEL_DEBUG_ENABLE_POST_MMU != 0;
    }

    return 0;
}

static void kernel_debug_log_targets(unsigned long phase, unsigned long page_a, unsigned long page_b, unsigned long walk_address)
{
    unsigned long index;
    unsigned long table_index;

    if (!kernel_debug_phase_enabled(phase)) {
        return;
    }

    for (index = 0UL; index < (sizeof(kernel_debug_targets) / sizeof(kernel_debug_targets[0])); index++) {
        unsigned long address;

        if (kernel_debug_targets[index].phase != phase) {
            continue;
        }

        address = kernel_debug_resolve_address(kernel_debug_targets[index].address_source, page_a, page_b, walk_address);
        if ((kernel_debug_targets[index].kind == KERNEL_DEBUG_TARGET_PAGE_RANGE ||
             kernel_debug_targets[index].kind == KERNEL_DEBUG_TARGET_MMU_WALK ||
             kernel_debug_targets[index].kind == KERNEL_DEBUG_TARGET_MMU_PROBE) &&
            address == 0UL) {
            continue;
        }

        KER_LOGF("[info] debug target=%s", kernel_debug_target_name(kernel_debug_targets[index].name));

        if (kernel_debug_targets[index].kind == KERNEL_DEBUG_TARGET_PAGE_RANGE) {
            KER_LOGF(" count=%lu\n", kernel_debug_targets[index].page_count);
            page_allocator_log_page_range(address, kernel_debug_targets[index].page_count);
        } else if (kernel_debug_targets[index].kind == KERNEL_DEBUG_TARGET_MANAGED_HEAD) {
            KER_LOGF(" count=%lu\n", kernel_debug_targets[index].page_count);
            page_allocator_log_managed_head(kernel_debug_targets[index].page_count);
        } else if (kernel_debug_targets[index].kind == KERNEL_DEBUG_TARGET_MMU_TABLES) {
            KER_LOGF(" count=%lu\n", mmu_table_page_count());
            for (table_index = 0UL; table_index < mmu_table_page_count(); table_index++) {
                KER_LOGF("[info] debug target=%s count=1\n", mmu_table_page_name(table_index));
                page_allocator_log_page_state(mmu_table_page_address(table_index));
            }
        } else if (kernel_debug_targets[index].kind == KERNEL_DEBUG_TARGET_MMU_WALK) {
            KER_LOGF(" count=1\n");
            mmu_debug_walk_address(address);
        } else if (kernel_debug_targets[index].kind == KERNEL_DEBUG_TARGET_MMU_PROBE) {
            KER_LOGF(" count=1\n");
            KER_LOGF("[info] probe %s par=%lx\n",
                     kernel_debug_target_name(kernel_debug_targets[index].name),
                     mmu_debug_probe_address(address));
        } else if (kernel_debug_targets[index].kind == KERNEL_DEBUG_TARGET_MMU_BOOT_WALKS) {
            KER_LOGF(" count=%lu\n", mmu_debug_boot_target_count());
            for (table_index = 0UL; table_index < mmu_debug_boot_target_count(); table_index++) {
                KER_LOGF("[info] debug target=%s count=1\n", mmu_debug_boot_target_name(table_index));
                mmu_debug_walk_address(mmu_debug_boot_target_address(table_index));
            }
        } else if (kernel_debug_targets[index].kind == KERNEL_DEBUG_TARGET_MMU_BOOT_PROBES) {
            KER_LOGF(" count=%lu\n", mmu_debug_boot_target_count());
            for (table_index = 0UL; table_index < mmu_debug_boot_target_count(); table_index++) {
                KER_LOGF("[info] probe %s par=%lx\n",
                         mmu_debug_boot_target_name(table_index),
                         mmu_debug_probe_address(mmu_debug_boot_target_address(table_index)));
            }
        } else if (kernel_debug_targets[index].kind == KERNEL_DEBUG_TARGET_HEAP_ARENAS) {
            KER_LOGF(" count=%lu\n", kernel_heap_debug_arena_count(kernel_debug_targets[index].page_count));
            for (table_index = 0UL; table_index < kernel_heap_debug_arena_count(kernel_debug_targets[index].page_count); table_index++) {
                KER_LOGF("[info] debug target=%s arena_index=%lu\n",
                         kernel_debug_target_name(kernel_debug_targets[index].name),
                         table_index);
                kernel_heap_debug_log_arena(table_index, kernel_debug_targets[index].page_count);
            }
        }
    }
}

void kernel_debug_log_pre_mmu_targets(unsigned long page_a, unsigned long page_b)
{
    kernel_debug_log_targets(KERNEL_DEBUG_PHASE_PRE_MMU, page_a, page_b, 0UL);
}

void kernel_debug_log_mmu_boot_targets(void)
{
    kernel_debug_log_targets(KERNEL_DEBUG_PHASE_MMU_BOOT, 0UL, 0UL, 0UL);
}

void kernel_debug_log_post_mmu_targets(unsigned long walk_address)
{
    kernel_debug_log_targets(KERNEL_DEBUG_PHASE_POST_MMU, 0UL, 0UL, walk_address);
}

void kernel_debug_log_heap_targets(void)
{
    kernel_debug_log_targets(KERNEL_DEBUG_PHASE_HEAP, 0UL, 0UL, 0UL);
}