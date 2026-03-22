#include <kernel/boot.h>
#include <kernel/debug_targets.h>
#include <kernel/exception.h>
#include <kernel/heap.h>
#include <kernel/log.h>
#include <kernel/mmu.h>
#include <kernel/page_alloc.h>
#include <kernel/timer.h>

volatile unsigned long boot_stage;
volatile unsigned long boot_heartbeat;

/*
 * Boot order matters here:
 * - logging/exceptions/page allocator must work before MMU setup
 * - mmu_init() consumes allocator pages for translation tables
 * - heap init happens after MMU so dynamic kernel memory already runs under the
 *   final Stage-1 translation regime
 */
void kernel_main(void)
{
    unsigned long *heap_counter;
    char *heap_message;
    void *test_page_a;
    void *test_page_b;

    boot_stage = 6;

    log_init();
    log_info("entering kernel_main");
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
     * MMU bring-up programs MAIR_EL1, TCR_EL1, TTBR0_EL1, invalidates stale
     * EL1 translations, then sets SCTLR_EL1.M/C/I to enable translation and
     * caches. After this point the kernel continues in the same identity-mapped
     * layout, but under the new permission model.
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

    kernel_heap_init();
    kernel_heap_log_stats();

    heap_counter = (unsigned long *)kmalloc(sizeof(unsigned long));
    heap_message = (char *)kmalloc(48UL);
    if (heap_counter != (unsigned long *)0 && heap_message != (char *)0) {
        heap_counter[0] = 7UL;
        heap_message[0] = 'h';
        heap_message[1] = 'e';
        heap_message[2] = 'a';
        heap_message[3] = 'p';
        heap_message[4] = '\0';
        log_write("[info] heap self-test counter=");
        log_write_u64(heap_counter[0]);
        log_write(" label=");
        log_write(heap_message);
        log_putc('\n');
    } else {
        log_info("heap self-test allocation failed");
    }
    kernel_heap_log_stats();
    kfree(heap_message);
    kfree(heap_counter);
    kernel_heap_log_stats();

    timer_init();
    log_write("[info] boot_stage=");
    log_write_u64(boot_stage);
    log_putc('\n');
    log_info("waiting for timer interrupts");

    while (1) {
        boot_heartbeat++;
        __asm__ volatile("wfe");
    }
}