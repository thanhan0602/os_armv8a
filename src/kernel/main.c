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

#define MMU_PAR_FAULT  (1UL << 0)

static void task_a_func(void)
{
    volatile unsigned long count = 0;

    while (1) {
        count++;
        if (count <= 4UL) {
            log_write("[task-a] run #");
            log_write_u64(count);
            log_putc('\n');
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
            log_write("[task-b] run #");
            log_write_u64(count);
            log_putc('\n');
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
    log_info("entering kernel_main_early");
    log_info("stage 2 console online");
    log_info("stack and bss initialized");
    exception_init();
    log_info("stage 3 exception vectors installed");
    page_allocator_init();
    log_info("stage 5 physical page allocator online");
    log_write("[info] reserved bytes=");
    log_write_u64(page_allocator_reserved_bytes());
    log_putc('\n');
    log_write("[info] free pages=");
    log_write_u64(page_allocator_free_pages());
    log_putc('\n');
    kernel_debug_log_pre_mmu_targets(0UL, 0UL);
    page_allocator_log_consistency();

    test_page_a = page_alloc();
    test_page_b = page_alloc();
    log_write("[info] alloc page a=");
    log_write_hex((unsigned long)test_page_a);
    log_putc('\n');
    log_write("[info] alloc page b=");
    log_write_hex((unsigned long)test_page_b);
    log_putc('\n');
    kernel_debug_log_pre_mmu_targets((unsigned long)test_page_a, (unsigned long)test_page_b);
    log_write("[info] free pages after alloc=");
    log_write_u64(page_allocator_free_pages());
    log_putc('\n');
    page_allocator_log_consistency();

    page_free(test_page_a);
    page_free(test_page_b);
    kernel_debug_log_pre_mmu_targets((unsigned long)test_page_a, (unsigned long)test_page_b);
    log_write("[info] free pages after free=");
    log_write_u64(page_allocator_free_pages());
    log_putc('\n');
    page_allocator_log_consistency();
    log_write("[info] invalid free count=");
    log_write_u64(page_allocator_invalid_free_count());
    log_putc('\n');
    log_write("[info] double free count=");
    log_write_u64(page_allocator_double_free_count());
    log_putc('\n');

    /*
     * MMU bring-up programs MAIR_EL1, TCR_EL1, TTBR0_EL1, TTBR1_EL1,
     * invalidates stale EL1 translations, then sets SCTLR_EL1.M/C/I.
     * After this point both identity map (TTBR0) and kernel VA map
     * (TTBR1) are live.
     */
    mmu_init();
    log_write("[info] mmu enabled=");
    log_write_u64((unsigned long)mmu_is_enabled());
    log_putc('\n');
    log_write("[info] free pages after mmu=");
    log_write_u64(page_allocator_free_pages());
    log_putc('\n');
    kernel_debug_log_post_mmu_targets((unsigned long)&boot_stage);
    page_allocator_log_consistency();

    log_info("kernel_main_early done, returning for VA trampoline");
}

/*
 * Called from start.S at the kernel virtual address after the trampoline.
 * TTBR1_EL1 is now the active translation path for all kernel code.
 * heap init happens here so dynamic kernel memory already runs under the
 * final Stage-1 translation regime at the intended VA.
 */
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
    log_info("kernel running at high VA");

    /* Replace boot-time TTBR0 identity tables with an owned empty runtime root. */
    mmu_install_empty_ttbr0_root();

    kernel_high_alias = (unsigned long)__text_start;
    kernel_low_alias = va_to_pa((void *)kernel_high_alias);

    mmu_debug_walk_address(kernel_low_alias);
    low_probe_par = mmu_debug_probe_address(kernel_low_alias);
    high_probe_par = mmu_debug_probe_address(kernel_high_alias);

    log_write("[info] ttbr0 empty low probe va=");
    log_write_hex(kernel_low_alias);
    log_write(" par=");
    log_write_hex(low_probe_par);
    log_write(" fault=");
    log_write_u64((low_probe_par & MMU_PAR_FAULT) != 0UL);
    log_putc('\n');

    log_write("[info] ttbr1 high probe va=");
    log_write_hex(kernel_high_alias);
    log_write(" par=");
    log_write_hex(high_probe_par);
    log_write(" fault=");
    log_write_u64((high_probe_par & MMU_PAR_FAULT) != 0UL);
    log_putc('\n');
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
        log_write("[info] heap self-test counter=");
        log_write_u64(heap_counter[0]);
        log_write(" label=");
        log_write(heap_message);
        log_write(" large=");
        log_write_hex((unsigned long)heap_large);
        log_putc('\n');
    } else {
        log_info("heap self-test allocation failed");
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
    log_write("[info] boot_stage=");
    log_write_u64(boot_stage);
    log_putc('\n');

    sched_init();
    task_create(task_a_func, "task-a");
    task_create(task_b_func, "task-b");

    log_info("idle task running");

    while (1) {
        boot_heartbeat++;
        __asm__ volatile("wfe");
    }
}