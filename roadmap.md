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

Current behavior:
- Kernel boots
- Logging works over UART
- Kernel installs vectors and can report synchronous faults with ESR/ELR/SPSR dumps

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

Design note for later consideration:
- The current Stage 6 baseline still relies on broad identity mapping for simplicity and debugability.
- A later architectural step can introduce a separate kernel virtual layout so kernel text, rodata, data, heap, stacks, MMIO, and optional physical direct-map regions live at intentionally chosen virtual addresses instead of broadly using `VA = PA`.
- This is intentionally deferred until after the current MMU, cache, timer, and exception paths remain stable under the simpler identity-mapped baseline.

### Stage 7: Kernel Heap

Status: completed

Goals:
- Add a kernel allocator on top of page allocation
- Support dynamic kernel data structures

Implemented:
- Added a page-backed kernel heap allocator with `kmalloc` and `kfree`
- Used a page-local free-list layout so heap allocations do not require physically contiguous pages
- Added boot-time heap statistics and a simple self-test allocation path in `kernel_main`

Current behavior:
- Kernel initializes a heap after MMU bring-up
- Small dynamic allocations can be served and freed during boot
- Heap usage and failure counters are logged so allocator growth is visible during bring-up

### Stage 8: Scheduler And Kernel Threads

Status: planned

Goals:
- Create kernel thread contexts
- Switch execution between threads
- Drive preemption from timer interrupts

### Stage 9: EL0 And Syscalls

Status: planned

Goals:
- Enter user mode at EL0
- Define a minimal syscall ABI
- Support basic calls such as write, yield, and exit

### Stage 10: Processes And Address Spaces

Status: planned

Goals:
- Give each process its own page tables
- Load user stacks and images
- Handle translation faults cleanly

### Stage 11: IPC And Synchronization

Status: planned

Goals:
- Add spinlocks and simple waiting primitives
- Support kernel coordination between tasks

### Stage 12: Filesystem And Program Loading

Status: planned

Goals:
- Start with ramfs or initramfs
- Add an ELF loader
- Launch an init program from the kernel

## Immediate Next Steps

1. Add register dump helpers for general-purpose registers during faults.
2. Extend the heap beyond the current single-page-allocation limit by introducing a dedicated virtual heap range or multi-page allocation strategy.
3. Extend the fine-grained `L2/L3` mapping beyond the initial kernel region as preparation for more advanced VM work.