#include <kernel/boot.h>
#include <kernel/debug_targets.h>
#include <kernel/exception.h>
#include <kernel/heap.h>
#include <kernel/log.h>
#include <kernel/mmu.h>
#include <kernel/page_alloc.h>
#include <kernel/sched.h>
#include <kernel/timer.h>
#include <kernel/vm.h>

volatile unsigned long boot_stage;
volatile unsigned long boot_heartbeat;

extern char __text_start[];
#ifdef CONFIG_KERNEL_VIRTUAL
extern char user_task_entry[];
extern char user_task_entry_end[];
#endif

#define MMU_PAR_FAULT  (1UL << 0)

static void task_a_func(void)
{
    volatile unsigned long count = 0;

    while (1) {
        count++;
        if (count <= 4UL) {
            KER_LOGF("[task-a] run #%lu\n", count);
        }
        __asm__ volatile("wfe");
    }
}

static void task_b_func(void)
{
    volatile unsigned long count = 0;

    while (1) {
        count++;
        if (count <= 4UL) {
            KER_LOGF("[task-b] run #%lu\n", count);
        }
        __asm__ volatile("wfe");
    }
}

/*
 * Boot order matters here:
 * - logging/exceptions/page allocator must work before MMU setup
 * - mmu_init() consumes allocator pages for translation tables
 * - kernel_main_early runs at PA before the VA trampoline
 * - kernel_main runs at high VA after the trampoline in start.S
 */

/*
 * Called from start.S at physical address before the MMU trampoline.
 * Initialises logging, exceptions, the page allocator, and the MMU.
 * Returns to start.S so it can switch execution to the kernel VA.
 */
void kernel_main_early(void)
{
    void *test_page_a;
    void *test_page_b;

    boot_stage = 6;

    log_init();
    KER_INFO("entering kernel_main_early");
    KER_INFO("stage 2 console online");
    KER_INFO("stack and bss initialized");
    exception_init();
    KER_INFO("stage 3 exception vectors installed");
    page_allocator_init();
    KER_INFO("stage 5 physical page allocator online");
    KER_LOGF("[info] reserved bytes=%lu\n", page_allocator_reserved_bytes());
    KER_LOGF("[info] free pages=%lu\n", page_allocator_free_pages());
    kernel_debug_log_pre_mmu_targets(0UL, 0UL);
    page_allocator_log_consistency();

    test_page_a = page_alloc();
    test_page_b = page_alloc();
    KER_LOGF("[info] alloc page a=%p\n", test_page_a);
    KER_LOGF("[info] alloc page b=%p\n", test_page_b);
    kernel_debug_log_pre_mmu_targets((unsigned long)test_page_a, (unsigned long)test_page_b);
    KER_LOGF("[info] free pages after alloc=%lu\n", page_allocator_free_pages());
    page_allocator_log_consistency();

    page_free(test_page_a);
    page_free(test_page_b);
    kernel_debug_log_pre_mmu_targets((unsigned long)test_page_a, (unsigned long)test_page_b);
    KER_LOGF("[info] free pages after free=%lu\n", page_allocator_free_pages());
    page_allocator_log_consistency();
    KER_LOGF("[info] invalid free count=%lu\n", page_allocator_invalid_free_count());
    KER_LOGF("[info] double free count=%lu\n", page_allocator_double_free_count());

    /*
     * MMU bring-up programs MAIR_EL1, TCR_EL1, TTBR0_EL1, TTBR1_EL1,
     * invalidates stale EL1 translations, then sets SCTLR_EL1.M/C/I.
     * After this point both identity map (TTBR0) and kernel VA map
     * (TTBR1) are live.
     */
    mmu_init();
    KER_LOGF("[info] mmu enabled=%lu\n", (unsigned long)mmu_is_enabled());
    KER_LOGF("[info] free pages after mmu=%lu\n", page_allocator_free_pages());
    kernel_debug_log_post_mmu_targets((unsigned long)&boot_stage);
    page_allocator_log_consistency();

    KER_INFO("kernel_main_early done, returning for VA trampoline");
}

/*
 * Called from start.S at the kernel virtual address after the trampoline.
 * TTBR1_EL1 is now the active translation path for all kernel code.
 * heap init happens here so dynamic kernel memory already runs under the
 * final Stage-1 translation regime at the intended VA.
 */
#ifdef CONFIG_KERNEL_VIRTUAL
static void create_user_task(void)
{
    struct mm_context *mm;
    void *code_pa;
    void *stack_pa;
    unsigned char *code_va;
    unsigned long code_size;
    unsigned long i;

    mm = mmu_context_create();
    if (mm == (struct mm_context *)0) {
        KER_INFO("[user] mmu_context_create failed");
        return;
    }

    code_pa = page_alloc();
    if (code_pa == (void *)0) {
        KER_INFO("[user] code page alloc failed");
        return;
    }

    stack_pa = page_alloc();
    if (stack_pa == (void *)0) {
        KER_INFO("[user] stack page alloc failed");
        return;
    }

    /* Copy user task code to the allocated page (accessed via kernel VA). */
    code_size = (unsigned long)user_task_entry_end - (unsigned long)user_task_entry;
    code_va = (unsigned char *)pa_to_va(code_pa);
    for (i = 0; i < code_size; i++) {
        code_va[i] = ((unsigned char *)user_task_entry)[i];
    }

    /* Ensure I-cache coherency after writing instructions. */
    __asm__ volatile("dsb ish\n ic iallu\n dsb nsh\n isb\n" ::: "memory");

    /* Map code page (EL0 RO, executable). */
    if (!mmu_map_user_page(mm, USER_CODE_VA, (unsigned long)code_pa,
                           MMU_USER_PAGE_NORMAL | MMU_USER_PAGE_AF |
                           MMU_USER_PAGE_INNER_SH | MMU_USER_PAGE_AP_RO)) {
        KER_INFO("[user] code page map failed");
        return;
    }

    /* Map stack page (EL0 RW, non-executable). */
    if (!mmu_map_user_page(mm, USER_STACK_TOP - PAGE_SIZE, (unsigned long)stack_pa,
                           MMU_USER_PAGE_NORMAL | MMU_USER_PAGE_AF |
                           MMU_USER_PAGE_INNER_SH | MMU_USER_PAGE_AP_RW |
                           MMU_USER_PAGE_UXN | MMU_USER_PAGE_PXN)) {
        KER_INFO("[user] stack page map failed");
        return;
    }

    task_create_user(USER_CODE_VA, USER_STACK_TOP, mm, "user");
}
#endif /* CONFIG_KERNEL_VIRTUAL */

void kernel_main(void)
{
    unsigned long *heap_counter;
    char *heap_large;
    char *heap_message;

#ifdef CONFIG_KERNEL_VIRTUAL
    /* Re-install VBAR_EL1 so it holds the kernel VA of the vector table. */
    unsigned long kernel_low_alias;
    unsigned long kernel_high_alias;
    unsigned long low_probe_par;
    unsigned long high_probe_par;

    exception_init();
    KER_INFO("kernel running at high VA");

    /* Replace boot-time TTBR0 identity tables with an owned empty runtime root. */
    mmu_install_empty_ttbr0_root();

    kernel_high_alias = (unsigned long)__text_start;
    kernel_low_alias = va_to_pa((void *)kernel_high_alias);

    mmu_debug_walk_address(kernel_low_alias);
    low_probe_par = mmu_debug_probe_address(kernel_low_alias);
    high_probe_par = mmu_debug_probe_address(kernel_high_alias);

    KER_LOGF("[info] ttbr0 empty low probe va=%lx par=%lx fault=%lu\n",
             kernel_low_alias, low_probe_par,
             (low_probe_par & MMU_PAR_FAULT) != 0UL);

    KER_LOGF("[info] ttbr1 high probe va=%lx par=%lx fault=%lu\n",
             kernel_high_alias, high_probe_par,
             (high_probe_par & MMU_PAR_FAULT) != 0UL);
#endif

    kernel_heap_init();
    kernel_heap_log_stats();

    heap_counter = (unsigned long *)kmalloc(sizeof(unsigned long));
    heap_message = (char *)kmalloc(48UL);
    heap_large = (char *)kmalloc(PAGE_SIZE + 512UL);
    if (heap_counter != (unsigned long *)0 && heap_message != (char *)0 && heap_large != (char *)0) {
        heap_counter[0] = 7UL;
        heap_message[0] = 'h';
        heap_message[1] = 'e';
        heap_message[2] = 'a';
        heap_message[3] = 'p';
        heap_message[4] = '\0';
        heap_large[0] = 'L';
        heap_large[PAGE_SIZE + 511UL] = 'Z';
        KER_LOGF("[info] heap self-test counter=%lu label=%s large=%p\n",
                 heap_counter[0], heap_message, (void *)heap_large);
    } else {
        KER_INFO("heap self-test allocation failed");
    }
    kernel_debug_log_heap_targets();
    kernel_heap_log_stats();
    page_allocator_log_consistency();
    kfree(heap_large);
    kfree(heap_message);
    kfree(heap_counter);
    kernel_heap_log_stats();
    page_allocator_log_consistency();

    timer_init();
    KER_LOGF("[info] boot_stage=%lu\n", boot_stage);

    sched_init();
    task_create(task_a_func, "task-a");
    task_create(task_b_func, "task-b");
#ifdef CONFIG_KERNEL_VIRTUAL
    create_user_task();
#endif

    KER_INFO("idle task running");

    while (1) {
        boot_heartbeat++;
        __asm__ volatile("wfe");
    }
}