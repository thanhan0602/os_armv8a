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
#include <kernel/driver.h>
#include <kernel/mmu.h>
#include <kernel/page_alloc.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/shell.h>
#include <kernel/spinlock.h>
#include <kernel/timer.h>
#include <kernel/vm.h>
#include <arch/arm/cpu.h>

#include <arch/arm/psci.h>

volatile unsigned long boot_stage;
volatile unsigned long boot_heartbeat;
volatile int secondary_ready = 0;

extern char __text_start[];

#if defined(CONFIG_KERNEL_VIRTUAL) && defined(CONFIG_RUN_OS_DEMOS)
static void kernel_start_stage10_demos(void)
{
    struct process *process_a;
    struct process *process_b;
    struct process *process_cow;

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

    process_cow = loader_load_process_image("/bin/test_cow.elf");
    if (process_cow != (struct process *)0 && task_create_user(process_cow, "user-cow") == (struct task *)0) {
        KER_INFO("task_create_user failed: user-cow");
        process_destroy(process_cow);
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

    mmu_init();
    log_write("[boot] MMU initialized\n");

    /* Initialise scheduler early so idle tasks exist for secondary cores */
    sched_init();

    /* Wake up secondary CPUs */
    extern void secondary_entry(void);
    unsigned long entry_pa = (unsigned long)va_to_pa(secondary_entry);
    
    /* log_write("[boot] waking up secondary CPUs...\n"); */
    
    KER_LOGF("[boot] entry_pa=%lx psci_version=%x\n", entry_pa, psci_version());
    
    unsigned long boot_mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(boot_mpidr));
    unsigned int boot_cpu_id = (unsigned int)(boot_mpidr & 0xFF);
    KER_LOGF("[boot] Boot CPU MPIDR=%lx (ID=%u)\n", boot_mpidr, boot_cpu_id);

    for (unsigned int i = 0; i < 4; i++) {
        if (i == boot_cpu_id) continue;
        
        unsigned long mpidr = i;
        secondary_ready = 0;
        int status = psci_cpu_on(mpidr, entry_pa, 0); 
        
        KER_LOGF("[boot] Waking CPU %u (MPIDR=%lx) return=%d\n", i, mpidr, status);

        if (status == 0) {
            /* Wait for the CPU to signal readiness with a timeout */
            volatile unsigned long timeout = 0x1000000;
            while (1) {
                /* dsb ish ensures we see the update from the secondary core */
                __asm__ volatile("dsb ish" ::: "memory");
                if (secondary_ready) break;
                if (timeout == 0) break;
                timeout--;
                cpu_yield();
            }
            if (secondary_ready) {
                KER_LOGF("[boot] CPU %u is ready\n", i);
            } else {
                KER_LOGF("[boot] CPU %u timed out (final check secondary_ready=%d)\n", i, secondary_ready);
            }
        } else {
            KER_LOGF("[boot] CPU %u wake failed: %d\n", i, status);
        }
    }
}

/*
 * Called from start.S at the kernel virtual address after the trampoline.
 * TTBR1_EL1 is now the active translation path for all kernel code.
 * heap init happens here so dynamic kernel memory already runs under the
 * final Stage-1 translation regime at the intended VA.
 */
void secondary_main_early(void)
{
    mmu_init_secondary();
}

/*
 * Called from start.S at the kernel virtual address after the trampoline.
 * TTBR1_EL1 is now the active translation path for all kernel code.
 */
extern struct task tasks[];
void secondary_main(void)
{
    unsigned int cpu_id = arch_get_cpu_id();
    
    /* Set current task to the per-CPU idle task initialized in sched_init */
    arch_set_current_task(&tasks[cpu_id]);

    /* Init Core-local GIC and Timer */
    driver_secondary_init();
    
    /* Enable interrupts for this core */
    arch_local_irq_enable();

    /* Ensure secondary_ready update is visible across cores */
    secondary_ready = 1;
    __asm__ volatile("dsb ish; sev" ::: "memory");
    
    /* Now we are at VA, we can safely log and continue */
    KER_LOGF("[cpu] CPU %u online\n", cpu_id);
    
    while (1) {
        schedule();
    }
}

void kernel_main(void)
{
    /* 
     * CPU 0 was initialized with a physical address for its current task
     * during kernel_main_early. Now that we are in high VA, update it to
     * use the proper virtual address of its idle task.
     */
    extern struct task tasks[];
    for (int i = 0; i < 4; i++) {
        tasks[i].next = &tasks[(i + 1) % 4];
    }
    arch_set_current_task(&tasks[0]);

    KER_INFO("kernel_main: jumped to high virtual address");

    /* 
     * Update exception vectors to use virtual addresses before we remove
     * the identity map. This ensures any faults during or after unmapping
     * can be handled and logged.
     */
    exception_init();

#ifdef CONFIG_KERNEL_VIRTUAL
    /* 
     * Now that we are safely running in the high virtual address space (TTBR1),
     * we can remove the identity map (TTBR0 bridge) used during boot.
     * This prepares TTBR0 for use by user processes.
     */
    mmu_install_empty_ttbr0_root();
#endif

    kernel_heap_init();
    fs_init();
    ipc_init();

#if defined(CONFIG_KERNEL_VIRTUAL) && defined(CONFIG_RUN_OS_DEMOS)
    kernel_start_stage10_demos();
#endif
    KER_INFO("kernel init complete");
    driver_system_dump();
    shell_init();

    while (1) {
        boot_heartbeat++;
        if (!shell_poll()) {
            schedule();
        }
    }
}