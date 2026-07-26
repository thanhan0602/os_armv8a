# ARMv8-A OS Roadmap

## Scope

Target platform: QEMU `virt` on ARMv8-A at EL1.

Primary objective: build a small bare-metal kernel that evolves into a minimal operating system with exception handling, memory management, scheduling, syscalls, and user mode.

## Phases

### Stage 1: Boot And Bring-Up

Status: completed

Goals:
- Enter at `_start`
- Set up a valid stack
- Clear `.bss`
- Transfer control into C via `kernel_main`
- Park non-boot cores safely

Implemented:
- Boot entry in `src/arch/arm/start.S`
- Linker-defined stack and `.bss` boundaries
- `kernel_main` as the C entrypoint

### Stage 2: Console And Logging

Status: completed

Goals:
- Bring up PL011 UART on QEMU `virt`
- Print boot progress to the terminal
- Provide a reusable kernel logging surface

Implemented:
- PL011 UART driver
- Console backend
- Log API with string, hex, and decimal output

### Stage 3: Exception Vectors And Fault Reporting

Status: completed

Goals:
- Install an EL1 vector table with `VBAR_EL1`
- Route synchronous exceptions into a C handler
- Dump `ESR_EL1`, `ELR_EL1`, and `SPSR_EL1`
- Prove the exception path with a deliberate `brk` self-test

Implemented:
- 16-slot vector table in `src/arch/arm/exception_vectors.S`
- Exception installation and dump logic in `src/kernel/exception.c`
- Synchronous exception self-test helper for validating fault handling
- General-purpose register dump for synchronous faults, including `x0..x30`, entry SP, `FPCR`, and `FPSR`

Current behavior:
- Kernel boots
- Logging works over UART
- Kernel installs vectors and can report synchronous faults with ESR/ELR/SPSR/FAR plus full general-purpose register dumps

### Stage 4: Timer And Interrupts

Status: completed

Goals:
- Set up the ARM generic timer
- Enable IRQ handling paths
- Generate periodic timer interrupts

Implemented:
- GICv2 bring-up for QEMU `virt`
- IRQ-aware exception vector flow with `eret` return path
- Periodic generic timer interrupts using the EL1 virtual timer registers
- Timer tick logging from the IRQ handler
- Full FP/SIMD context save and restore in exception entry paths

Current behavior:
- Kernel boots to EL1
- Exception vectors are installed
- IRQs are enabled after timer setup
- The generic timer fires periodically and logs timer ticks

### Stage 5: Physical Memory Management

Status: completed

Goals:
- Detect or define usable RAM ranges for QEMU `virt`
- Implement a page allocator
- Expose page allocation APIs to the kernel

Implemented:
- Defined a usable RAM range for QEMU `virt` starting after `__kernel_end`
- Added a 4 KiB physical page free-list allocator with `alloc` and `free`
- Added boot-time allocator statistics and self-check allocations
- Added allocator metadata guards to catch invalid frees and duplicate frees
- Added allocator debug helpers for per-page state, small range dumps, managed-head dumps, and consistency checks

Current behavior:
- Kernel reports RAM base, RAM end, allocator start, and total managed pages
- Kernel can allocate and free physical pages during boot
- Allocated pages are zeroed before use
- Kernel can now report per-page state transitions and verify allocator bookkeeping against the free list during boot

### Stage 6: MMU And Kernel Virtual Memory

Status: completed

Goals:
- Build translation tables
- Map text as read-execute and data as read-write
- Enable MMU and caches safely

Implemented:
- Moved QEMU `virt` memory map constants into an architecture header
- Allocated a 48-bit VA root translation table at L0 from the page allocator
- Installed a hybrid identity map using `L0 -> L1 -> L2 -> L3` for the initial kernel RAM region, with fine-grained mappings covering at least `2` chunks of `2 MiB` each, and `L2` block mappings for the remaining RAM
- Programmed `MAIR_EL1`, `TCR_EL1`, `TTBR0_EL1`, and enabled `SCTLR_EL1.M/C/I` with an explicit known-good control value
- Enabled FP/SIMD access early in boot so the kernel can safely compile without `-mgeneral-regs-only`
- Fixed a page-table descriptor bug where table entries were missing the `VALID` bit during the earlier multi-level MMU experiments
- Added a unified boot-time debug-target framework that now covers allocator windows, managed-head dumps, MMU table pages, software walks, and `PAR_EL1` probes

Current behavior:
- Kernel enables the MMU after page allocator initialization
- UART, GIC, timer, and IRQ handling continue working after MMU enable
- UART, GIC, timer, and IRQ handling continue working after MMU and caches are enabled
- Page table memory is tracked as allocator usage
- Kernel now runs with a 48-bit VA translation regime rooted at L0
- The active runtime map uses page-level `L3` mappings for at least the first `4 MiB` of RAM so text, rodata, data, bss, and stack can carry distinct permissions
- The remaining QEMU RAM range continues to use larger `L2` block mappings for simplicity and lower table overhead
- MMU boot-time debug targets now flow through the same framework as page-allocator debug targets, so software walk and hardware probe logs can be correlated from one target system

Kernel virtual layout (completed):
- Added `vm.h` with `KERNEL_VA_OFFSET`, `pa_to_va()` and `va_to_pa()` macros. Offset is `0xFFFF000000000000` when `CONFIG_KERNEL_VIRTUAL=1`, `0` when identity-only.
- Linker script exports `__kernel_va_offset` symbol and defines `KERNEL_LMA_BASE`, `KERNEL_VA_OFFSET`, `KERNEL_VMA_BASE`
- `mmu_init()` now builds a second set of page tables for `TTBR1_EL1` (kernel VA) alongside a boot-time `TTBR0_EL1` identity map (when `CONFIG_KERNEL_VIRTUAL=1`)
- `TCR_EL1` enables both TTBR0 and TTBR1 walks when virtual, or sets `EPD1=1` to disable TTBR1 when identity-only
- `start.S` calls `kernel_main_early()` at PA (pre-MMU through post-MMU-enable), then uses a trampoline (`adrp+add+KERNEL_VA_OFFSET`) to jump to `kernel_main()` at the kernel VA (when virtual), or calls `kernel_main()` directly (when identity)
- PA→VA migration complete: page_alloc, heap, mmu walks, MMIO drivers all use conditional PA/VA conversion via `mmu_is_enabled()`
- After trampoline in the virtual build, runtime installs an owned empty lower-half root into `TTBR0_EL1` and frees the old boot identity tables, so `TTBR1_EL1` remains the only kernel execution map
- Runtime verified: both `KERNEL_VIRTUAL=1` (high VA, 6 table pages at runtime, heap at `0xFFFF...`) and `KERNEL_VIRTUAL=0` (PA, 5 table pages, heap at `0x4...`) boot clean with timer ticks and zero mismatches
- Runtime proof for the virtual build now shows the low kernel alias faults while the `TTBR1` high alias still translates

Bug encountered during this work:
- First attempt set VMA=VA in linker.ld (`0xFFFF000040080000`). This caused `const char *` fields inside `static const struct` arrays in `.rodata` to contain high VA values. Pre-MMU C code dereferenced these VA pointers before TTBR1 was active, causing an infinite fault loop.
- Resolution: keep VMA=PA in linker so all pointer-in-data values remain physical addresses. The trampoline computes kernel VA at runtime via `adrp+add+offset`. This avoids the pre-MMU pointer problem entirely.
- Lesson: a VMA=VA linker split requires the MMU (with TTBR1 mapping) to be active before any C code touches initialised pointer data. That demands moving page-table construction into assembly, which is significantly more complex.

### Stage 7: Kernel Heap

Status: completed

Goals:
- Add a kernel allocator on top of page allocation
- Support dynamic kernel data structures

Implemented:
- Added a page-backed kernel heap allocator with `kmalloc` and `kfree`
- Added contiguous physical page allocation helpers to the page allocator for larger heap arenas
- Used a page-local free-list layout for small allocations and multi-page contiguous arenas for larger allocations
- Added boot-time heap statistics plus both small and larger-than-one-page self-test allocations in `kernel_main`
- Added shared heap debug targets `heap-arenas` and `heap-large-arenas` backed by heap arena inspection helpers

Current behavior:
- Kernel initializes a heap after MMU bring-up
- Small and larger-than-one-page dynamic allocations can be served and freed during boot in both the high-VA and identity-only runtime variants
- Heap usage and failure counters are logged so allocator growth is visible during bring-up
- Heap self-test inspection now flows through the shared debug-target framework, including dedicated targets for all arenas and multi-page arenas

### Stage 8: Scheduler And Kernel Threads

Status: completed

Goals:
- Create kernel thread contexts
- Switch execution between threads
- Drive preemption from timer interrupts

Implemented:
- Callee-saved context struct (`x19-x30`, `SP`) for cooperative/preemptive switching
- `switch_context()` assembly routine that saves/restores callee-saved registers
- `task_entry_trampoline` that unmasks IRQs and calls the task entry function on first run
- Round-robin `schedule()` called from the timer IRQ handler after EOI
- `task_create()` allocates guard page + usable stack page contiguously from the page allocator
- `task_exit()` marks task dead; dead tasks are reaped by `sched_reap_dead()` at the next schedule, freeing stack pages back to the allocator
- Idle task (task 0) runs on the boot stack, integrated into the circular ready list
- Two demo tasks (`task-a`, `task-b`) created at boot to verify round-robin scheduling

Current behavior:
- Scheduler cycles idle → task-b → task-a → idle every 500ms timer tick
- Context switches logged for the first 8 switches
- Dead tasks are unlinked from the ready list and their guard+stack pages are freed
- Both `KERNEL_VIRTUAL=1` and `KERNEL_VIRTUAL=0` variants verified at runtime

### Stage 9: EL0 And Syscalls

Status: completed

Goals:
- Enter user mode at EL0
- Define a minimal syscall ABI
- Support basic calls such as write, yield, and exit

Implemented:
- `el0_entry_trampoline` in `switch.S`: sets `ELR_EL1`=user entry VA, `SP_EL0`=user stack, `SPSR_EL1`=0 (EL0t, DAIF=0), zeros all GPRs, then `eret` to EL0
- Syscall ABI: `x8`=number, `x0-x5`=args, `x0`=return; `SYS_WRITE=1`, `SYS_YIELD=2`, `SYS_EXIT=3`
- `sys_write`: reads user-VA buffer via kernel mapping and outputs each byte via `log_putc`
- `sys_yield`: calls `schedule()` to voluntarily relinquish the CPU
- `sys_exit`: logs exit and calls `task_exit()` to mark task dead and schedule away
- `exception_handle_sync` updated: EC=0x15 (SVC) dispatches to `syscall_dispatch`; returns 1 for handled (eret), 0 for fatal (park)
- Exception frame extended by 16 bytes (CTX_SIZE 784→800) to save/restore `ELR_EL1` and `SPSR_EL1`: this fixed a critical bug where `sys_yield` followed by a timer IRQ on another task corrupted `ELR_EL1`, causing `eret` to return to kernel EL1 code instead of the user continuation address
- `mmu_map_user_page`: walks/allocates L0→L3 tables in the user `mm_context`; installs a 4 KiB page descriptor with caller-supplied flags
- `task_create_user`: allocates a kernel stack, sets `x19`=user entry VA, `x20`=SP_EL0, `x30`=`el0_entry_trampoline`, attaches `mm`
- Demo user task (`user_task.S`): position-independent EL0 code that prints `[user] hello from EL0\n` 3 times with yield, then accesses `0xDEAD0000` to trigger the Stage 10 fault test

Current behavior:
- Kernel boots to EL1, schedules user task at EL0
- User task executes three `sys_write` + `sys_yield` rounds, printing via UART from EL0
- After iteration 3, user task stores to unmapped VA → handled by Stage 10 fault handler (not sys_exit)
- round-robin of idle/task-a/task-b continues after user task is killed

### Stage 10: Processes And Address Spaces

Status: completed

Goals:
- Give each process its own page tables
- Load user stacks and images
- Handle translation faults cleanly

Implemented (first increment):
- Virtual runtime now keeps `TTBR1` as the only kernel execution map after trampoline
- `TTBR0` is replaced with an owned empty lower-half root instead of being disabled, so the lower half is ready to be populated later
- The old boot identity tables are freed after the handoff, reducing virtual runtime MMU table usage from `10` to `6` pages

Implemented (second increment — EL0 fault handling):
- Added `ESR_EC_DABT_LOW` (EC=0x24) and `ESR_EC_IABT_LOW` (EC=0x20) cases to `exception_handle_sync`
- On EL0 data or instruction abort: logs `FAR_EL1`, `ELR_EL1`, `ESR_EL1`, and the fault status code (FSC), then calls `task_exit()` to kill the offending user task cleanly
- Kernel scheduler reaps the dead user task; timer and remaining kernel tasks continue unaffected
- User task demo updated to trigger a deliberate store to `0xDEAD0000` after its normal work, validating the full fault → kill → reap path

Current behavior:
- In `KERNEL_VIRTUAL=1`, the low kernel alias faults at runtime while the high `TTBR1` alias still translates
- User task runs at EL0, does 3 `sys_write` + `sys_yield` iterations, then accesses unmapped VA
- Fault handler logs `[fault] EL0 data abort: FAR=0xdead0000 ELR=... ESR=... FSC=5` (translation fault level 1)
- User task is killed and reaped; idle/task-a/task-b continue scheduling normally

### Stage 11: IPC And Synchronization

Status: completed

Goals:
- Add spinlocks and simple waiting primitives
- Support kernel coordination between tasks

Implemented (first increment — spinlock primitive):
- Added `spinlock` primitive in `src/kernel/spinlock.c` and `src/include/kernel/spinlock.h`
- Lock acquire uses AArch64 `ldaxr/stxr`; unlock uses `stlr`
- Added `spin_lock_irqsave()` / `spin_unlock_irqrestore()` helpers so kernel code can serialize shared state without being preempted by local IRQ handlers while holding the lock
- Added `cpu_wait()` / `cpu_wake()` wrappers around `wfe` / `sev` so contended spin paths can block briefly instead of pure busy-waiting
- Kernel logger (`src/kernel/log.c`) is now the first user of this primitive, serializing console output across normal task, syscall, and fault-reporting paths

Current behavior:
- Kernel still boots and logs correctly in quiet-boot mode
- `RUN_OS_DEMOS=1` runtime verify still shows Stage 10 user/syscall/fault traces after the logger moved under the new spinlock

Implemented (second increment — fixed IPC channels + blocking receive):
- Added `src/include/kernel/ipc.h` and `src/kernel/ipc.c` with fixed global channels (`IPC_CHANNEL_MAX=8`) and one-message mailboxes (`IPC_MESSAGE_MAX=64`)
- Added `ipc_send()` and `ipc_receive()`; `recv` publishes its waiter under the channel lock and then uses `sched_park_task()`, while `send` removes the waiter before calling `sched_unpark_task()` after releasing the channel lock
- Scheduler now uses a pending-wake token in `sched_park_task()` / `sched_unpark_task()` so Stage 11 can suspend and resume tasks without spinning or losing a wake that arrives before the receiver parks
- Added `ipc_detach_task()` so a killed/reaped task is removed from channel wait state before its task slot and process resources are freed
- Added user-visible syscalls `SYS_IPC_SEND=451` and `SYS_IPC_RECV=452`, plus user wrappers `user_ipc_send()` / `user_ipc_recv()`
- Added built-in user demos `/bin/ipc_recv.elf` and `/bin/ipc_send.elf` for shell-driven validation
- While validating this increment, fixed a latent exception-frame bug by saving/restoring `SP_EL0` in addition to `ELR_EL1` and `SPSR_EL1`; this is required when a user task blocks inside a syscall and later resumes at EL0 with live stack locals

Current behavior:
- Loading `/bin/ipc_recv.elf` then `/bin/ipc_send.elf` from the shell delivers `hello from ipc_send\n` over channel `1`
- The receiver sleeps inside `SYS_IPC_RECV` until the sender posts a message, then resumes and exits cleanly
- Both IPC demo tasks now exit without the post-wakeup EL0 stack corruption that existed before `SP_EL0` was preserved across task switches

Implemented (third increment — blocking Mutex):
- Added `struct mutex` in `src/include/kernel/mutex.h` and `src/kernel/mutex.c`
- Implemented `mutex_lock` and `mutex_unlock` with wait-queue support using `task->wait_next`
- Mutex operations are SMP-safe using internal spinlocks and `irqsave/irqrestore`
- Verified by spawning multiple kernel tasks on different cores competing for a shared variable with deliberate delays

Implemented (fourth increment — SMP scheduler and lifetime hardening):
- Removed scheduler/subsystem lock inversion by performing IPC, mutex, process, MM, stack, and allocator cleanup after releasing `sched_lock`
- Added `TASK_STATE_REAPING`, waiter detach, mutex ownership handoff, and mutex-pool `active_ops`/`destroying` lifetime protection
- Made remote task kill SMP-safe with `kill_pending` plus IPI; the owning CPU transitions a running task to DEAD before reaping
- Routed nanosleep and task-state updates through scheduler APIs protected by `sched_lock`
- Added `mm_context` owner references, `dying`, active CPU tracking, and deferred release after the final CPU switches TTBR0 away
- Verified on QEMU with four CPUs: all secondary CPUs came online, the pthread workload printed `Complex Test Finished.`, all user tasks exited with code 0, and no fatal marker was found

### Stage 12: Filesystem And Program Loading

Status: completed

Goals:
- Start with ramfs or initramfs
- Add an ELF loader
- Launch an init program from the kernel

Implemented (first increment — read-only ramfs + flat-binary loader):
- Added `src/include/kernel/fs.h` and `src/kernel/fs.c` with a kernel-only read-only `ramfs`
- Added `src/include/kernel/loader.h` and `src/kernel/loader.c` so program loading now goes through `file -> loader -> process`
- `ramfs` currently exports `/bin/user-a` and `/bin/user-b`, backed by the existing demo user images linked into the kernel
- Added `process_create_from_buffer()` so process creation can consume bytes loaded from a file instead of depending on linker symbol ranges
- `kernel_main()` with `RUN_OS_DEMOS=1` now loads both demo processes from `/bin/user-a` and `/bin/user-b`
- Runtime verified on QEMU: both user processes run again, `user-a` still passes the `brk` smoke test, and both EL0 fault paths remain correct after the filesystem/loader refactor

Implemented (second increment — ELF loader + dynamic external files):
- Added a dedicated `user/` tree with real ELF64 AArch64 user apps, a minimal `_start`, syscall wrappers, and a user linker script
- Built-in programs are now embedded as `/bin/hello.elf` and `/bin/fault.elf` instead of flat binaries
- `process_create_from_elf()` now parses ELF headers/program headers and maps PT_LOAD segments into a fresh user address space
- `src/kernel/fs.c` now supports dynamic ramfs nodes through `fs_register_file()` / `fs_unregister_file()`
- `src/kernel/shell.c` now supports `receive <path> <size>` to upload an external ELF as a hex stream at runtime
- Runtime verified on QEMU: `/ext/ticker.elf` uploads successfully, `read` shows a valid ELF header, `load` starts a long-running user task, and `unload` reaps it cleanly

Implemented (third increment — multiple isolated user processes):
- `create_user_task()` in `main.c` refactored to accept `code_start`, `code_end`, `name` params; can now spawn arbitrary user tasks from any user code blob
- Added `user_task_b.S`: second independent EL0 task that prints `[user-b] hello from EL0\n` three times with yields, then faults at `0xCAFE0000`
- Two user tasks (`user-a` and `user-b`) created at boot, each with a completely separate `mm_context` (distinct `root_pa`, separate L0→L3 page tables for code and stack)
- Both user tasks interleave with each other and with kernel tasks on each timer tick via `sys_yield`
- `user-b` faults at `0xCAFE0000` → killed; `user-a` continues running unaffected
- `user-a` subsequently faults at `0xDEAD0000` → killed; scheduler continues with kernel tasks
- Address isolation confirmed: each `mm_context` maps `USER_CODE_VA=0x10000` to a different physical page

Current behavior:
- Kernel boots, creates 5 tasks: idle, task-a, task-b, user-a, user-b
- user-a and user-b run interleaved, printing messages and yielding
- user-b's fault (`FAR=0xCAFE0000`) does not affect user-a's execution
- After both user tasks are killed, idle/task-a/task-b continue round-robin scheduling normally

Implemented (fourth increment — fault classification):
- EL0 DABT/IABT handler now decodes FSC type (translation / permission / access-flag) and the WnR bit
- Logs: `[fault] EL0 data abort (write translation L1): FAR=...` or `(write permission L3): ...`
- `user-a` triggers translation fault at `0xDEAD0000` (L1: no page table entry); `user-b` triggers permission fault at `0x10000` (L3: AP_RO code page)
- All string-literal selection uses `log_write()` calls (not `const char *` pointer tables) to avoid GCC -O2 PA-pointer jump-table bug

Implemented (fifth increment — ASID support):
- `struct mm_context` now carries an `unsigned int asid` (1–254; ASID=0 reserved for kernel/empty root)
- `mmu_context_create()` assigns the next ASID from a global `next_asid` counter
- `mmu_context_switch()` writes `root_pa | (asid << 48)` to TTBR0_EL1; the full `tlbi vmalle1` on every context switch is removed — ASID tags partition the TLB automatically
- User L3 page descriptors have nG=1 forced inside `mmu_map_user_page()` so TLB entries are ASID-tagged (non-global)
- `mmu_map_user_page()` uses `tlbi vae1is` (per-VA per-ASID) instead of `tlbi vmalle1`
- TCR_EL1.AS=0 (8-bit ASID, values 0–255) and A1=0 (TTBR0 provides ASID) — both are hardware defaults, no TCR changes needed

## Immediate Next Steps

1. ~~Stage 16: Multicore Support (SMP).~~ ✅ Done.
2. Stage 16 regression increment: ✅ Added an opt-in SMP regression suite via `SMP_REGRESSION_TESTS=1`. Deterministic four-core QEMU tests now validate wake-before-park, remote kill/reap, mutex owner/waiter detach, rejection of destroy while a mutex is locked, and deferred `mm_context` release after the final active CPU detaches TTBR0. Success markers include `[stress] mutex-concurrent-destroy PASS` and `[stress] mm-deferred-release PASS`.
3. Stage 16 regression runner: ✅ Added spinlock-protected suite aggregation, `[stress] ALL PASS`, and deterministic QEMU exit through PSCI `SYSTEM_OFF`. A full four-core run exits with status 0 after all six checks pass.
4. Stage 16 repeated mutex lifetime race: ✅ Added a deterministic 512-round lock/trylock/unlock versus destroy regression. The full four-core suite prints `[stress] mutex-destroy-race PASS`, then `[stress] ALL PASS`, and QEMU exits with status 0.
5. Stage 16 multi-CPU MM lifetime: ✅ Added a deterministic shared-MM regression with the same context active on two CPUs. The first detach leaves the DYING context alive, the final detach releases it exactly once, and the full suite exits with status 0 after `[stress] mm-multi-cpu-detach PASS` and `[stress] ALL PASS`.
6. Add generation-tagged mutex pool handles and synchronous cross-CPU MM shootdown acknowledgement.
4. Stage 16 increment 3: Kernel Preemption. Currently, the kernel only reschedules on explicit yield or return from interrupt. Full kernel preemption would allow higher priority tasks to interrupt lower priority kernel work.
5. Stage 11 increment 3: add a real wait-queue abstraction or multi-waiter channel queue so IPC is no longer limited to one blocked receiver per channel.
6. Stage 12 increment 3: add a real init-style launch path so the kernel boots one named program from the filesystem instead of hardcoding demo pairs in `main.c`.
7. User runtime increment 1: add a tiny libc surface for argv/env-style startup or reusable printing/string helpers beyond the current syscall wrappers.
8. Stage 17: Support FAT32/SD Card via VIRTIO or dedicated driver for persistent storage.

### Stage 13: Console Shell

Status: completed

Goals:
- Add an interactive serial console shell for bring-up and inspection
- Expose minimal file, process, and memory inspection commands
- Support loading and unloading user programs without rebuilding the image

Implemented (first increment — polling shell over UART):
- Added non-blocking PL011 RX polling and `console_try_getc()`
- Added a minimal shell in `src/kernel/shell.c` that runs from the idle loop after boot completes
- Current commands: `help`, `read <path> [count]`, `write <text>`, `show process` / `ps`, `show memory` / `memory` / `mem`, `load <path> [task-name]`, `unload <task-id>`
- `read` currently dumps a file from the read-only `ramfs` as hex + ASCII; `load` reuses the Stage 12 loader path to spawn a user task from the named file
- `show process` is backed by new scheduler inspection helpers; `unload` marks a task dead by id so the normal scheduler reap path can reclaim it

Current behavior:
- Default boot now reaches a serial shell prompt after `[info] kernel init complete`
- `ps`, `memory`, `read`, `write`, `load`, `receive`, and `unload` have been verified on QEMU
- External user ELF flow is now validated end-to-end with `/ext/ticker.elf`: upload through shell, `read` header check, `load`, repeated user output, then `unload`

### Stage 14: Lazy Loading & Copy-on-Write

Status: completed

Goals:
- Implement demand paging (Loading pages only when accessed)
- Support Copy-on-Write for memory efficiency and process forking
- Handle software-walk faults for kernel syscalls

Implemented:
- Added `struct vm_region` and region management in `process.c`
- Updated `mmu_handle_process_page_fault` to load data from ELF buffers or zero-fill for anonymous regions (Stack/Heap)
- Implemented physical page reference counting in `page_alloc.c`
- Handled permission faults to implement Copy-on-Write: pages are shared with RO permissions and duplicated only on write
- Instrumented `mmu_copy_user_range` to manually call the page fault handler during software walks, fixing syscall string access issues
- Verified isolation with `test_cow.elf` and performance/correctness with existing ELF apps

Current behavior:
- User apps boot without copy-overhead for their entire image
- Syscalls like `write` correctly trigger page loads when accessing unmapped buffers
- Parent/Child isolation via CoW is verified

### Stage 15: Optimizations & Advanced VFS

Status: completed

Goals:
- ASID Recycling & TLB Optimization (Minimize flushes)
- Multi-bit ASID support (8/16-bit)
- Bitmask-based ASID management
- Removed full TLB flushes on context switch

Implemented:
- Hardware capability detection for ASID bits (8 vs 16)
- Enabled 16-bit ASID range (1-65535) via TCR_EL1.AS=1
- Implemented efficient bitmask allocation for ASIDs (8KB BSS)
- Optimized TLB maintenance: using `ASIDE1IS` for ASID reuse and `VAE1IS` for CoW
- Confirmed kernel mappings are Global (`nG=0`) and shared across contexts

### Stage 16: Multicore Support (SMP)

Status: completed

Goals:
- Support multi-core execution (4 cores)
- Implement hardware wake-up (PSCI)
- Add thread-safe synchronization (Spinlocks)
- Implement Inter-Processor Interrupts (IPI) for scheduling

Implemented:
- **PSCI Booting:** CPU 0 wakes secondary cores (1, 2, 3) using `psci_cpu_on` via `hvc`.
- **Per-CPU Isolation:** Each core has its own 16KB stack in the kernel VA space and uses `tpidr_el1` to point to its current `struct task`.
- **SMP-Safe Scheduler:** Refactored `sched.c` to use a global `sched_lock`. Idle tasks are created for each core (slots 0-3). `schedule()` holds the lock across `switch_context`, which is then released by the incoming task.
- **GICv2 Secondary Initialization:** Added `gicv2_init_secondary` to enable the CPU interface and timers for each core.
- **Inter-Processor Interrupts (IPI):** Implemented `gicv2_send_ipi` using GICv2 SGIs. `sched_wake_task` sends an IPI to all other cores to trigger a reschedule, ensuring low latency for task wakeup.
- **Concurrency Safety:** Added spinlocks to `page_alloc.c` and `heap.c` to prevent race conditions during memory allocation from multiple cores.

Current behavior:
- All 4 cores boot successfully and enter the scheduler.
- Shell runs on CPU 0 but can interact with tasks on other cores.
- Preemptive multitasking works across all cores using independent timers.

### Stage 17: Process Lifecycle & Execve

Status: completed

Goals:
- Implement `execve` syscall for process image replacement
- Support reloading ELF images into an existing task context
- Ensure architectural consistency during address space transitions

Implemented:
- **Execve Syscall:** Added `SYS_EXECVE=221` and `process_execve` in `src/kernel/process.c`. This replaces the current task's memory space, parses a new ELF, and sets up a fresh stack/entry point.
- **Architectural Safety:** Fixed a critical bug where `task->process` wasn't updated alongside `task->mm`, causing kernel panic in the page fault handler during transitions.
- **MMU Refactor:** Unified table allocation (`mmu_alloc_sub_table`) and fixed a descriptor bug where kernel VAs were incorrectly stored in place of PAs.
- **Runtime Verified:** `test_exec.elf` successfully loads `/bin/hello.elf` via syscall, prints its message, and terminates without corrupting the kernel.

### Stage 18: Enhanced User Runtime & Linker

Status: in-progress

Goals:
- Move ELF loading and symbol resolution to user-space (`ld.so`)
- Support environment variables and `LD_DEBUG`
- Document and implement AArch64-specific PLT resolution

Implemented:
- **Linker Debugging:** Added `LD_DEBUG` environment support in the dynamic linker (`user/linker/ld_main.c`) for tracing symbol resolution.
- **ABI Documentation:** Documented the 80-byte stack frame requirements for the AArch64 PLT resolver to preserve all volatile registers (x0-x9).

### Stage 19: Synchronization & Pthread API

Status: completed

Goals:
- Implement sleep-based Mutexes to replace spinlocks for long-duration critical sections
- Support SMP-safe locking with wait-queues and task blocking
- Provide a POSIX-like `pthread` API for user-space synchronization

Implemented:
- **Blocking Mutex:** Added `struct mutex` in [src/include/kernel/mutex.h](src/include/kernel/mutex.h) and implementation in [src/kernel/mutex.c](src/kernel/mutex.c). Uses a linked-list wait queue (`task->wait_next`) to manage sleeping tasks.
- **Syscall Interface:** Added `SYS_MUTEX_LOCK` (500), `SYS_MUTEX_UNLOCK` (501), mutex trylock/init/destroy support, and standard `SYS_CLONE` (224) in [src/include/kernel/syscall.h](src/include/kernel/syscall.h).
- **User-space API:** Created [user/include/pthread.h](user/include/pthread.h) providing `pthread_create`, `pthread_join`, `pthread_yield`, and pthread mutex operations.
- **TLS and clone ABI:** `CLONE_VM | CLONE_THREAD | CLONE_SETTLS` shares the process address space and installs per-thread `TPIDR_EL0`; `switch_context` saves/restores the thread pointer.
- **SMP Safety:** Mutex internals are protected by spinlocks with IRQ disabling, scheduler wakeups send reschedule IPIs, and GICv2 SGI handling now masks IAR[9:0] for dispatch while writing the full IAR to EOIR.
- **Runtime verification:** `test_pthread.elf` completed cleanly on 4-core QEMU in repeated stress runs with idle `wfe` re-enabled.

### Stage 20: Swap to Disk & Advanced VFS (Next Steps)

Status: planned

Goals:
- Swap to Disk (Ramfs-backed basic implementation)
- Support FAT32/SD Card via VFS
- Advanced IPC (Shared memory regions, Pipes)
- Signal handling and process lifecycle management
