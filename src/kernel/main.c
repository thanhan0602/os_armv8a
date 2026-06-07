#include <kernel/boot.h>
#include <kernel/debug_targets.h>
#include <kernel/exception.h>
#include <kernel/device_tree.h>
#include <kernel/driver.h>
#include <kernel/fs.h>
#include <kernel/heap.h>
#include <kernel/ipc.h>
#include <kernel/loader.h>
#include <kernel/log.h>
#include <kernel/mmu.h>
#include <kernel/page_alloc.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/shell.h>
#include <kernel/spinlock.h>
#include <kernel/timer.h>
#include <kernel/vm.h>

volatile unsigned long boot_stage;
volatile unsigned long boot_heartbeat;

extern char __text_start[];

#if defined(CONFIG_KERNEL_VIRTUAL) && defined(CONFIG_RUN_OS_DEMOS)
static void kernel_start_stage10_demos(void)
{
    struct process *process_a;
    struct process *process_b;

    process_a = loader_load_process_image("/bin/hello.elf");
    if (process_a != (struct process *)0 && task_create_user(process_a, "user-a") == (struct task *)0) {
        KER_INFO("task_create_user failed: user-a");
        process_destroy(process_a);
    }

    process_b = loader_load_process_image("/bin/fault.elf");
    if (process_b != (struct process *)0 && task_create_user(process_b, "user-b") == (struct task *)0) {
        KER_INFO("task_create_user failed: user-b");
        process_destroy(process_b);
    }
}
#endif

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
void kernel_main_early(unsigned long boot_fdt_pa)
{
    boot_stage = 6;

    log_init();
    exception_init();
    page_allocator_init();
    if (device_tree_init(boot_fdt_pa)) {
        KER_LOGF("[boot] dtb pa=%lx size=%lu\n",
                 device_tree_blob_pa(),
                 device_tree_blob_size());
    } else {
        KER_INFO("[boot] no valid dtb supplied");
    }
    driver_system_init();

    /*
     * MMU bring-up programs MAIR_EL1, TCR_EL1, TTBR0_EL1, TTBR1_EL1,
     * invalidates stale EL1 translations, then sets SCTLR_EL1.M/C/I.
     * After this point both identity map (TTBR0) and kernel VA map
     * (TTBR1) are live.
     */
    mmu_init();
}

/*
 * Called from start.S at the kernel virtual address after the trampoline.
 * TTBR1_EL1 is now the active translation path for all kernel code.
 * heap init happens here so dynamic kernel memory already runs under the
 * final Stage-1 translation regime at the intended VA.
 */
void kernel_main(void)
{
    KER_INFO("kernel_main: jumped to high virtual address");
#ifdef CONFIG_KERNEL_VIRTUAL
    exception_init();

    /* Replace boot-time TTBR0 identity tables with an owned empty runtime root. */
    mmu_install_empty_ttbr0_root();
#endif

    kernel_heap_init();
    fs_init();
    ipc_init();

    sched_init();
#if defined(CONFIG_KERNEL_VIRTUAL) && defined(CONFIG_RUN_OS_DEMOS)
    kernel_start_stage10_demos();
#endif
    KER_INFO("kernel init complete");
    driver_system_dump();
    shell_init();

    while (1) {
        boot_heartbeat++;
        if (!shell_poll()) {
            cpu_relax();
        }
    }
}