# SMP Support Handoff

## Summary
The system has been upgraded from a single-core OS to a 4-core SMP environment. This involved implementing hardware wake-up, inter-core synchronization, and per-cpu data management.

## Key Components

### 1. Bootstrapping (PSCI)
- **Primary CPU (0)**: Initialized as before, then calls `psci_cpu_on` (SMC/HVC) to wake up CPUs 1, 2, and 3.
- **Secondary CPUs (1-3)**: Enter at `secondary_entry`, enable MMU (sharing CPU 0's tables), and jump to `secondary_main`.

### 2. Synchronization (Spinlocks)
- Implemented in `src/arch/arm/cpu.h` (locking) and `src/kernel/spinlock.c`.
- Uses `ldaxr` (Load-Acquire Exclusive Register) and `stxr` (Store Exclusive Register) for atomic operations.
- Critical paths in `page_alloc.c`, `heap.c`, and `sched.c` are now protected. IRQs are disabled while holding locks.

### 3. Per-CPU State
- **TPIDR_EL1**: Each core stores its current `struct task` pointer here. Access via `arch_get_current_task()`.
- **System Stacks**: Each core has an independent 16KB stack defined in `start.S`.

### 4. Scheduler (SMP Safe)
- **Global Lock**: All operations on the task list use `sched_lock`.
- **IPI (Inter-Processor Interrupt)**: SGI ID 0 is used for reschedule requests. When a task is woken up, an IPI is sent to all other cores to check if they should reschedule.

## Verification
- **QEMU Log**: Shows `[info] CPU 1 online`, `[info] CPU 2 online`, `[info] CPU 3 online`.
- **Preemption**: Tasks running on different cores are correctly preempted by their local timer interrupts.
- **Race conditions**: Stress tested by spawning multiple tasks that allocate memory simultaneously.

## Known Limitations / Future Work
- **Lock Contention**: The global `sched_lock` might become a bottleneck with more cores.
- **Load Balancing**: Currently uses a simple round-robin where any core can pick any task. A more advanced scheduler with per-cpu runqueues could be implemented.
- **Kernel Preemption**: The kernel itself is not yet fully preemptible.

## Supplementary Documentation
- [SMP Postmortem](smp_postmortem.md): Detailed analysis of bugs and race conditions encountered during SMP integration (UART sync, VA/PA consistency).
