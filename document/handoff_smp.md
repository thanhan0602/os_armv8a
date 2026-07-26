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

### MMU and process-lifetime hardening

The following SMP fixes are available on branch `fix/mmu-page-allocator-smp`:

- `8d301d4 fix(mm): harden MMU and page allocator for SMP`
  - Serializes per-address-space page-table mutation, lazy faults, CoW, map, and unmap operations.
  - Protects contiguous page free and allocator consistency checks.
  - Makes page reference release atomic with returning the page to the free list.
  - Serializes ASID allocation and restricts active ASIDs to the configured 8-bit range.
- `ee16d51 fix(smp): harden process and MM lifetime paths`
  - Prevents multithreaded `execve()` until sibling-thread termination is implemented.
  - Fixes process reference counting for legacy thread creation.
  - Serializes page-fault access to process VM metadata.
  - Snapshots process VM metadata under `process->lock` during fork.

The combined changes passed a clean build and a 4-core QEMU pthread run. CPUs 1-3 came online, kernel initialization completed, all pthread tasks exited with code 0, and the test printed `Complex Test Finished.`. No panic, abort, invalid free, double free, allocator corruption, or deadlock marker appeared in that validation run.

The opt-in SMP regression build now aggregates all six deterministic checks under a spinlock-protected pass mask. After every check succeeds it prints `[stress] ALL PASS` and calls PSCI `SYSTEM_OFF`, allowing QEMU to terminate normally with status 0 instead of depending on an external timeout.

### Scheduler, IPC, mutex, and MM lifetime hardening

- Scheduler wait/wake now uses `sched_park_task()` and `sched_unpark_task()` with a pending-wake token, preventing wake-before-park notifications from being lost.
- IPC and mutex release their subsystem locks before entering the scheduler. Task reaping releases `sched_lock` before IPC, mutex, process, MM, stack, or allocator cleanup, removing the previous lock-order cycle.
- A task running on another CPU is killed by setting `kill_pending` and sending an IPI. Its owning CPU transitions it to DEAD in `schedule()` before reaping can occur.
- Nanosleep and task-state updates are serialized by scheduler APIs under `sched_lock`.
- Mutex pool operations pin slots with `active_ops`; destroy rejects active, locked, owned, or queued mutexes. Reaping detaches dead tasks from mutex wait queues and transfers ownership when necessary.
- `mm_context` now has owner references, a `dying` state, an active CPU mask, and deferred release after the last CPU switches its TTBR0 away from the context.
- A fresh 4-CPU QEMU run completed the pthread synchronization workload with `Complex Test Finished.`, all four user tasks exited with code 0, and no fatal marker was found.

## Known Limitations / Future Work
- **Lock Contention**: The global `sched_lock` might become a bottleneck with more cores.
- **Load Balancing**: Currently uses a simple round-robin where any core can pick any task. A more advanced scheduler with per-cpu runqueues could be implemented.
- **Kernel Preemption**: The kernel itself is not yet fully preemptible.
- **Synchronous remote stop**: remote kill is safe against premature reaping, but the API does not wait for an explicit stop acknowledgement before returning.
- **Targeted MM shootdown**: MM lifetime is protected, but context switch still uses broad TLB invalidation and there is no synchronous targeted detach API.
- **Handle generations**: IPC waiter safety relies on detach-before-slot-reuse, and mutex IDs do not yet include a generation counter for stale-handle detection.
- **Clone semantics**: `CLONE_VM` without `CLONE_THREAD` is not implemented with Linux-like shared-address-space semantics. Unsupported flag combinations should be rejected explicitly until implemented.
- **Boot handshake**: Secondary cores are brought up sequentially through one shared `secondary_ready` flag. Use per-CPU acquire/release flags before parallel bring-up or CPU hotplug is introduced.

## Supplementary Documentation
- [SMP Postmortem](smp_postmortem.md): Detailed analysis of bugs and race conditions encountered during SMP integration (UART sync, VA/PA consistency).
