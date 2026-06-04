# Tài liệu kiến trúc tổng quan — os_armv8a

> Phiên bản: tháng 5 năm 2026. Mô tả trạng thái hiện tại của implementation thực tế. Không phải thiết kế tương lai.
>
> Đối tượng: developer quay lại sau một khoảng thời gian nghỉ và cần nắm lại toàn bộ bức tranh kiến trúc từ một tài liệu duy nhất.

---

## Mục lục

1. [Nền tảng phần cứng](#1-nền-tảng-phần-cứng)
2. [Bố cục bộ nhớ vật lý](#2-bố-cục-bộ-nhớ-vật-lý)
3. [Build variants](#3-build-variants)
4. [Luồng boot](#4-luồng-boot)
5. [Bố cục linker](#5-bố-cục-linker)
6. [MMU và bảng trang](#6-mmu-và-bảng-trang)
7. [Không gian địa chỉ ảo](#7-không-gian-địa-chỉ-ảo)
8. [Page allocator vật lý](#8-page-allocator-vật-lý)
9. [Kernel heap](#9-kernel-heap)
10. [Exception và interrupt](#10-exception-và-interrupt)
11. [Scheduler](#11-scheduler)
12. [Console shell, ramfs, và user ELF loader](#12-console-shell-ramfs-và-user-elf-loader)
13. [Debug target framework](#13-debug-target-framework)
14. [Stack các hệ thống con](#14-stack-các-hệ-thống-con)
15. [Quyết định thiết kế quan trọng và bài học kỹ thuật](#15-quyết-định-thiết-kế-quan-trọng-và-bài-học-kỹ-thuật)
16. [Trạng thái các stage](#16-trạng-thái-các-stage)

---

## 1. Nền tảng phần cứng

| Mục | Giá trị |
|---|---|
| Platform | QEMU `virt`, ARMv8-A |
| Execution level | EL1 |
| RAM | `0x40000000` – `0x48000000` (128 MiB) |
| Kernel load PA | `0x40080000` |
| GICD base | `0x08000000` |
| GICC base | `0x08010000` |
| PL011 UART | `0x09000000` |
| Page granule | 4 KiB |
| VA bits | 48-bit |

Kernel được build bằng AArch64 cross-toolchain, chạy dưới QEMU với `-machine virt`. Non-boot cores được park sớm trong `_start`.

---

## 2. Bố cục bộ nhớ vật lý

```
0x00000000 ┌──────────────────────────────────┐
           │  Device MMIO (GICD, GICC, UART)  │
0x09000000 │  PL011 UART                      │
           │  ...                             │
0x40000000 ├──────────────────────────────────┤
           │  (RAM bắt đầu)                   │
0x40080000 ├──────────────────────────────────┤
           │  Kernel image                    │
           │    .text  .rodata  .data  .bss   │
           │    .stack_guard   .boot_stack    │
           ├──────────────────────────────────┤  ← __kernel_end
           │  Page tables (alloc tại boot)    │
           │    L0/L1/L2/L3 entries           │
           ├──────────────────────────────────┤
           │  Managed RAM                     │
           │  (page allocator free list)      │
0x48000000 └──────────────────────────────────┘
```

Tất cả địa chỉ MMIO được map trong bảng trang với thuộc tính Device-nGnRnE.

---

## 3. Build variants

Kernel hỗ trợ hai cấu hình build thông qua biến `KERNEL_VIRTUAL`:

| Flag | Lệnh build | Chế độ chạy | TTBR1 |
|---|---|---|---|
| `KERNEL_VIRTUAL=1` | `make` hoặc `make KERNEL_VIRTUAL=1` | High VA `0xFFFF...` qua TTBR1 | bật |
| `KERNEL_VIRTUAL=0` | `make KERNEL_VIRTUAL=0` | PA qua identity map | tắt (`EPD1=1`) |

Cả hai variant đã được xác minh runtime: timer tick, exception handling, heap, scheduler đều hoạt động.

Macro chuyển đổi địa chỉ:

```c
// src/include/kernel/vm.h
#if CONFIG_KERNEL_VIRTUAL
#define KERNEL_VA_OFFSET  0xFFFF000000000000UL
#else
#define KERNEL_VA_OFFSET  0UL
#endif

#define pa_to_va(pa)  ((void *)((uintptr_t)(pa) + KERNEL_VA_OFFSET))
#define va_to_pa(va)  ((uintptr_t)(va) - KERNEL_VA_OFFSET)
```

---

## 4. Luồng boot

### KERNEL_VIRTUAL=1 (mặc định)

```
┌─────────────────────────────────────────────────────────────┐
│  start.S                                                     │
│  _start  (PA)                                               │
│    ├─ park non-boot cores                                    │
│    ├─ enable FP/SIMD (CPACR_EL1)                            │
│    ├─ clear .bss                                             │
│    └─ call kernel_main_early()  (PA)                        │
└──────────────────────┬──────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────┐
│  kernel_main_early()  (PA, pre-MMU→post-MMU)                │
│    ├─ uart/console init                                     │
│    ├─ page_alloc_init()                                     │
│    └─ mmu_init()                                            │
│         ├─ build TTBR0 identity tables                      │
│         ├─ build TTBR1 kernel VA tables                     │
│         ├─ program MAIR_EL1, TCR_EL1                        │
│         ├─ set TTBR0_EL1, TTBR1_EL1                        │
│         └─ enable SCTLR_EL1.M/C/I  ← MMU ON               │
└──────────────────────┬──────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────┐
│  Trampoline  (start.S)                                      │
│    adrp + add + KERNEL_VA_OFFSET                            │
│    br x_target  ← nhảy sang high VA                        │
└──────────────────────┬──────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────┐
│  kernel_main()  (VA qua TTBR1)                              │
│    ├─ mmu_install_empty_ttbr0_root()                        │
│    │     └─ TTBR0 → owned empty root, free boot tables     │
│    ├─ exception_init()  (VBAR_EL1)                          │
│    ├─ heap_init()                                           │
│    ├─ gic_init(), timer_init()                              │
│    ├─ sched_init(), task_create(...)                        │
│    └─ enable IRQ, enter idle loop                           │
└─────────────────────────────────────────────────────────────┘
```

### KERNEL_VIRTUAL=0

```
_start (PA) → kernel_main_early (PA)
  → mmu_init (TTBR0 only, EPD1=1) → MMU ON
  → kernel_main (PA, qua TTBR0) → ...
```

Không có trampoline. Kernel ở lại PA suốt runtime.

---

## 5. Bố cục linker

Linker script: [src/linker.ld](../src/linker.ld)

```
PA 0x40080000
│
├─ .text          (RO + X,   page-aligned)   ← __text_start
│    .text.boot   (entry point, ưu tiên đầu)
│    .text.vectors (exception vector table)
│    ...
│                                            ← __text_end
├─ .rodata        (RO + NX,  page-aligned)   ← __rodata_start
│                                            ← __rodata_end
├─ .data          (RW + NX,  page-aligned)   ← __data_start
│                                            ← __data_end
├─ .bss           (RW + NX,  page-aligned)   ← __bss_start
│                                            ← __bss_end
├─ .stack_guard   (1 page, NOLOAD)           ← __stack_guard
│    [guard page — không unmap ở hw, chỉ isolation logic]
├─ .boot_stack    (16 KiB, NOLOAD)           ← __stack_bottom
│                                            ← __stack_top
│
└─ __kernel_end
     ↑
     Page tables boot + managed RAM bắt đầu từ đây
```

**Điểm thiết kế quan trọng**: VMA = PA (không phải VA cao). Tất cả pointer-in-data trong `.rodata` đều là PA. Trampoline tính VA tại runtime bằng `adrp+add+KERNEL_VA_OFFSET`. Điều này đảm bảo code C trước MMU không dereference VA chưa được map.

---

## 6. MMU và bảng trang

### Cấu hình phần cứng

| Tham số | Giá trị |
|---|---|
| Granule | 4 KiB |
| VA width | 48-bit (T0SZ=T1SZ=16) |
| Số level | 4 (L0 → L1 → L2 → L3) |
| MAIR index 0 | Normal WB WA (RAM) |
| MAIR index 1 | Device-nGnRnE (MMIO) |

### Hybrid mapping

Kernel dùng chiến lược hybrid để cân bằng kiểm soát chi tiết và overhead bảng trang:

```
Vùng kernel image (≥ 4 MiB đầu RAM):
  L0 → L1 → L2 → L3   (fine-grained, 4 KiB/entry)
  → kiểm soát permission từng page

Phần RAM còn lại:
  L0 → L1 → L2 block  (2 MiB/entry)
  → ít overhead bảng trang hơn
```

### Cấu trúc bảng trang (KERNEL_VIRTUAL=1)

```
TTBR0_EL1 (boot: identity map PA=VA, runtime: empty lower-half root)
  └─ L0 root
       ├─ L1 → device block (MMIO)
       └─ L1 → L2 (RAM)
            ├─ L2 → L3 (chunk 0, 2 MiB, fine-grained)
            ├─ L2 → L3 (chunk 1, 2 MiB, fine-grained)
            └─ L2 block (RAM còn lại)

TTBR1_EL1 (kernel VA map, prefix t1-)
  └─ t1-L0 root
       ├─ t1-L1 → device block (MMIO, cùng PA)
       └─ t1-L1 → t1-L2 (RAM)
            ├─ t1-L2 → t1-L3-chunk-0 (fine-grained)
            ├─ t1-L2 → t1-L3-chunk-1 (fine-grained)
            └─ t1-L2 block (RAM còn lại)
```

Sau trampoline: TTBR0 identity root bị thay bằng empty owned root; 5 boot TTBR0 pages được freed. Runtime giữ 6 pages bảng trang (1 empty TTBR0 root + 5 TTBR1 pages).

Với `KERNEL_VIRTUAL=0`: 5 pages bảng trang, không có TTBR1.

### Permission model

| Vùng | AP | UXN/PXN |
|---|---|---|
| `.text` | RO (AP=10) | PXN=0 (executable) |
| `.rodata` | RO (AP=10) | PXN=1 (NX) |
| `.data` / `.bss` / `.boot_stack` | RW (AP=00) | PXN=1 (NX) |
| MMIO | RW (AP=00) | PXN=1 (NX) |

---

## 7. Không gian địa chỉ ảo

```
0xFFFFFFFFFFFFFFFF ┐
                   │
0xFFFF000048000000 │  RAM cao (upper half, TTBR1)
                   │  VA = PA + 0xFFFF000000000000
0xFFFF000040080000 │  ← Kernel image tại VA cao
                   │
0xFFFF000040000000 │  ← RAM thấp nhất được map (TTBR1)
                   │
0xFFFF000000000000 ┘  ← Ranh giới upper half (TTBR1)

      ... (lỗ hổng canonical VA) ...

0x0000FFFFFFFFFFFF ┐
                   │
0x0000000040080000 │  (Boot: identity map PA=VA)
                   │  (Runtime KERNEL_VIRTUAL=1: FAULT — lower half empty)
                   │  (Runtime KERNEL_VIRTUAL=0: identity map còn active)
0x0000000040000000 │
                   │
0x0000000000000000 ┘  ← Lower half (TTBR0)
```

Sau trampoline trong build `KERNEL_VIRTUAL=1`:
- Low alias `0x40080000` → fault (TTBR0 root rỗng)
- High alias `0xFFFF000040080000` → translates qua TTBR1

---

## 8. Page allocator vật lý

**File**: [src/kernel/page_alloc.c](../src/kernel/page_alloc.c), [src/include/kernel/page_alloc.h](../src/include/kernel/page_alloc.h)

### Thiết kế

- Free-list allocator, đơn vị 4 KiB
- Nguồn: RAM sau `__kernel_end`
- Mảng `page_state[]` theo dõi từng page: `free` / `allocated` / `unused`

### API chính

```c
void  *page_alloc(void);
void   page_free(void *page);
void  *page_alloc_contiguous(size_t count);   // heap sử dụng
void   page_free_contiguous(void *base, size_t count);
```

### Observability

```c
page_allocator_check_consistency()   // → mismatches (0 = healthy)
page_allocator_log_consistency()     // in snapshot
page_allocator_log_page_state(addr)  // state một page
page_allocator_log_managed_head(n)   // n page đầu managed region
```

Kết quả mong đợi: `mismatches=0` tại mọi checkpoint.

---

## 9. Kernel heap

**File**: [src/kernel/heap.c](../src/kernel/heap.c), [src/include/kernel/heap.h](../src/include/kernel/heap.h)

### Thiết kế

Heap dùng page allocator làm backend, tổ chức theo arena:

```
┌─ struct kernel_heap_page  (header đầu arena) ─────────────┐
│  magic, page_count, ...                                   │
├─ struct kernel_heap_block  (block đầu tiên) ───────────── │
│  size, is_free=1, next, prev                              │
│  [free space]                                             │
├─ struct kernel_heap_block  (sau alloc) ─────────────────  │
│  size, is_free=0                                          │
│  [allocated data, 16-byte aligned]                        │
├─ ...                                                      │
└───────────────────────────────────────────────────────────┘
```

- **Alloc nhỏ**: first-fit trong các arena hiện có
- **Alloc lớn** (> 1 page): `page_alloc_contiguous()` → arena mới
- **kfree**: coalesce với block liền kề nếu free

### Giới hạn hiện tại

- Arena rỗng chưa được trả về page allocator
- Chưa có virtual heap range riêng (dùng PA hoặc VA tương ứng)
- Không có guard zone giữa các allocation

### Observability

```c
kmalloc(size)
kfree(ptr)
kernel_heap_log_stats()       // tổng kết heap
```

Debug targets: `heap-arenas`, `heap-large-arenas`.

---

## 10. Exception và interrupt

**Files**: [src/arch/arm/exception_vectors.S](../src/arch/arm/exception_vectors.S), [src/kernel/exception.c](../src/kernel/exception.c), [src/drivers/interrupt/gicv2.c](../src/drivers/interrupt/gicv2.c)

### Vector table

16-slot EL1 vector table được cài vào `VBAR_EL1` khi boot.

```
EL1t sync  │ EL1h sync  │ EL1t irq  │ EL1h irq
EL1t fiq   │ EL1h fiq   │ EL1t serr │ EL1h serr
EL0 sync   │ EL0 irq    │ ...
```

### Sync fault dump

Khi có synchronous exception, kernel dump:

```
ESR_EL1   ELR_EL1   SPSR_EL1   FAR_EL1
x0 .. x30
SP_EL1    FPCR      FPSR
```

### GICv2

| Mục | Giá trị |
|---|---|
| GICD base | `0x08000000` |
| GICC base | `0x08010000` |
| Timer PPI | 27 (generic timer EL1 virtual) |
| Timer interval | ~500 ms |

Luồng IRQ: exception vector → `exception_handle_irq()` → GIC ACK → dispatch → GIC EOI → `schedule()` → `eret`.

### FP/SIMD

FP/SIMD được bật sớm trong `_start` (`CPACR_EL1`). Exception vectors save/restore toàn bộ `q0–q31`, `FPCR`, `FPSR`. Kernel không cần `-mgeneral-regs-only`.

---

## 11. Scheduler

**Files**: [src/kernel/sched.c](../src/kernel/sched.c), [src/arch/arm/switch.S](../src/arch/arm/switch.S), [src/include/kernel/sched.h](../src/include/kernel/sched.h)

### Thiết kế

Round-robin preemptive, driven bằng timer IRQ (~500 ms/tick).

```c
struct task_context {
    uint64_t x19, x20, ..., x30;   // callee-saved GPR
    uint64_t sp;
};

struct task {
    struct task_context  context;
    uint32_t             id;
    enum task_state      state;     // RUNNING / READY / DEAD
    void                *stack_base;
    size_t               stack_size;
    const char          *name;
    struct mm_context   *mm;        // reserved for Stage 10
    struct task         *next;      // circular linked list
};
```

### Luồng context switch

```
Timer IRQ
  → exception_vectors.S  (save full frame GPR+SIMD)
  → exception_handle_irq()
  → GIC EOI
  → schedule()
       → sched_reap_dead()  (free stack pages của dead tasks)
       → chọn task tiếp theo (round-robin)
       → switch_context()   (save/restore x19-x30, SP)
  → eret  (restore full frame, return to new task)
```

Lần đầu chạy một task mới: `task_entry_trampoline` unmask IRQ rồi gọi entry function.

### Stack layout của task

```
page N   ┌─────────────────┐  ← stack_base (guard page, không dùng)
         │  (guard page)   │    allocated nhưng không unmap ở hw
page N+1 ├─────────────────┤  ← usable stack page
         │  stack (4 KiB)  │
         └─────────────────┘
```

Guard page nằm trong L2 block mapping nên không thể unmapped ở hardware level; isolation chỉ ở mức allocation.

### Thông số

| Mục | Giá trị |
|---|---|
| MAX_TASKS | 16 |
| Idle task id | 0 (boot stack) |
| Stack per task | 1 guard page + 1 usable page |
| Demo tasks | `task-a`, `task-b` |

---

## 12. Console shell, ramfs, và user ELF loader

**Files**: [src/kernel/shell.c](../src/kernel/shell.c), [src/kernel/fs.c](../src/kernel/fs.c), [src/kernel/loader.c](../src/kernel/loader.c), [src/kernel/process.c](../src/kernel/process.c), [user/linker.ld](../user/linker.ld), [Makefile](../Makefile)

### Console shell

Shell hiện chạy trực tiếp từ idle loop của `kernel_main()` và poll input qua PL011.

Các command runtime hiện có:

- `read <path> [count]`
- `write <text>`
- `show process` / `ps`
- `show memory` / `mem` / `memory`
- `load <path> [task-name]`
- `unload <task-id>`
- `receive <path> <size>`

`receive` nhận hex stream từ UART, decode trực tiếp thành bytes, rồi đăng ký file runtime vào dynamic ramfs. Cách này cho phép nạp ELF từ bên ngoài mà không cần rebuild kernel image.

### Ramfs và external file path

Filesystem hiện là kernel-only ramfs với hai nhóm node:

- built-in nodes embed vào kernel image: `/bin/hello.elf`, `/bin/fault.elf`, `/bin/shared_client.elf`, `/lib/libshared.so`
- dynamic nodes đăng ký lúc runtime qua `fs_register_file()`, hiện được shell sử dụng cho các path kiểu `/ext/*.elf`

`loader_load_process_image()` giữ vai trò nối `fs_open()` với `process_create_from_elf()`: đọc toàn bộ file vào heap kernel, validate ELF, rồi tạo process user từ buffer đó.

### User ELF runtime

User apps hiện được build từ cây `user/` thành ELF64 AArch64 thật. Các app tự chứa dùng linker script zero-based và được link kiểu `ET_DYN`/PIE thay vì `ET_EXEC`, nên image không còn phụ thuộc vào một load address cố định.

Luồng load hiện tại:

1. đọc toàn bộ ELF image vào heap kernel
2. validate ELF header và program headers
3. chọn `load_bias` cho object nếu là `ET_DYN`
4. map từng `PT_LOAD` tại `load_bias + p_vaddr`
5. đặt `entry_va = load_bias + e_entry`
6. đặt `heap_start` và `brk` ngay sau vùng image đã map, kèm một page guard

Vì `brk` bắt đầu sau image thay vì tại một địa chỉ cố định, user heap cũng di chuyển theo load bias của mỗi process.

### Shared-library groundwork

Loader user-space hiện đã hỗ trợ một bước tiến thực dụng tới shared library:

- đọc `DT_NEEDED` để tự load dependency object từ `/lib`
- parse `DT_STRTAB`, `DT_SYMTAB`, `DT_STRSZ`, `DT_SYMENT`, `DT_SONAME`, `DT_RELA`, `DT_JMPREL`, `DT_PLTRELSZ`, `DT_PLTREL`
- resolve symbol xuyên qua tập object đã load cho cùng process
- apply các relocation `R_AARCH64_RELATIVE`, `R_AARCH64_JUMP_SLOT`, `R_AARCH64_GLOB_DAT`, `R_AARCH64_ABS64`

Sample đã được verify runtime:

- `/lib/libshared.so`: shared object mẫu export `shared_write()`
- `/bin/shared_client.elf`: app built-in phụ thuộc `libshared.so`
- `/ext/shared_client.elf`: cùng app nhưng nạp từ bên ngoài qua shell `receive`

Hiện tại đây mới là shared-library support ở mức loader và relocation. Mỗi process vẫn map một bản private của library; chưa có cơ chế chia sẻ page text/data giữa các process, chưa có object cache dùng chung, và chưa có ABI/version resolver.

### IPC channels

Stage 11 hiện đã có increment IPC đầu tiên ở mức kernel primitive:

- fixed global channels với id `0..7`
- mailbox một-message, kích thước tối đa `64` bytes
- `SYS_IPC_SEND=451` và `SYS_IPC_RECV=452`
- `recv` là blocking: task chuyển sang `TASK_STATE_BLOCKED` và chỉ được wake khi sender ghi message vào channel tương ứng

Thiết kế này cố tình hẹp: mục tiêu hiện tại là chứng minh kernel có thể sleep/wake user task bên trong syscall path một cách đúng đắn, không spin polling ở EL0.

Luồng runtime hiện tại:

1. receiver gọi `SYS_IPC_RECV`
2. nếu mailbox rỗng, kernel đăng ký task đó là waiter của channel và đổi state sang `BLOCKED`
3. scheduler chuyển CPU sang task khác
4. sender gọi `SYS_IPC_SEND`, kernel copy bytes vào mailbox và wake waiter nếu có
5. receiver được schedule lại, `SYS_IPC_RECV` hoàn tất, rồi copy message về user buffer

Validation cho slice này dùng hai app built-in:

- `/bin/ipc_recv.elf`
- `/bin/ipc_send.elf`

Khi chạy `ipc_recv` trước rồi `ipc_send` sau, receiver block đúng, sender wake đúng, message được deliver đúng, và cả hai task exit sạch.

---

## 13. Debug target framework

**File**: [src/kernel/debug_targets.c](../src/kernel/debug_targets.c), [src/include/kernel/debug_targets.h](../src/include/kernel/debug_targets.h)

Framework thống nhất cho boot-time inspection. Mỗi target có thể được gated bằng build flag.

### Build flags

| Flag | Phase |
|---|---|
| `DEBUG_PRE_MMU` | Trước khi MMU bật |
| `DEBUG_MMU_BOOT` | Trong khi MMU init |
| `DEBUG_POST_MMU` | Sau khi MMU bật, kernel_main() |

### Danh sách targets

| Target | Mô tả |
|---|---|
| `managed-head` | N page đầu vùng managed |
| `page-a` | Trạng thái page A (test alloc) |
| `page-b` | Trạng thái page B (test alloc) |
| `alloc-window` | Cửa sổ page quanh vùng vừa alloc |
| `mmu-tables` | Các page bảng trang đã được alloc |
| `mmu-walk` | Software walk một VA quan trọng |
| `mmu-probe` | `AT S1E1R` + `PAR_EL1` cho cùng VA |
| `mmu-boot-walks` | Software walks tại boot |
| `mmu-boot-probes` | Hardware probes tại boot |
| `heap-arenas` | Tất cả heap arenas |
| `heap-large-arenas` | Chỉ arenas ≥ 2 pages |

---

## 14. Stack các hệ thống con

```
┌────────────────────────────────────────────────────────────┐
│                    kernel_main()                           │
│   (chạy tại VA qua TTBR1 hoặc PA qua TTBR0)              │
├────────────────┬───────────────┬───────────────────────────┤
│ Shell / loader │  Heap kmalloc │  Exception / IRQ handler  │
│ shell.c fs.c   │  heap.c       │  exception.c              │
│ loader.c       │               │  exception_vectors.S      │
│ process.c      │               │                           │
├────────────────┴───────────────┴───────────────────────────┤
│               Page allocator  page_alloc.c                 │
├────────────────────────────────────────────────────────────┤
│               MMU  mmu.c  (bảng trang, vm.h)              │
├────────────────────────────────────────────────────────────┤
│       Drivers:  pl011.c (UART)   gicv2.c (GIC)            │
│       console.c   log.c   timer.c                          │
├────────────────────────────────────────────────────────────┤
│              start.S + linker.ld (boot, stack)             │
├────────────────────────────────────────────────────────────┤
│              QEMU virt  ARMv8-A  EL1  RAM 128 MiB          │
└────────────────────────────────────────────────────────────┘
```

---

## 15. Quyết định thiết kế quan trọng và bài học kỹ thuật

### VMA = PA trong linker.ld

**Quyết định**: Giữ VMA bằng PA, không dùng VMA cao.

**Lý do**: Nếu VMA = `0xFFFF...`, các `const char *` bên trong `static const struct` trong `.rodata` sẽ chứa VA tuyệt đối. Pre-MMU C code dereference chúng trước khi TTBR1 active → infinite fault loop.

**Giải pháp**: Trampoline tính VA tại runtime qua `adrp + add + KERNEL_VA_OFFSET`. VMA=PA đảm bảo tất cả pointer-in-data luôn là PA hợp lệ trước và sau MMU.

### Table descriptor phải có bit VALID

Lỗi cổ điển: table entry thiếu bit `VALID`. Phần cứng không đi tiếp translation walk → multi-level fault. Descriptor đúng: `VALID | TABLE | (pa & mask)`.

### Post-TTBR0 pointer translation

Sau khi TTBR0 empty root được cài, code chạy qua TTBR1 nhưng các pointer đã lưu trong `static const` tables vẫn là PA. Phải gọi `pa_to_va()` trước khi dereference.

### Page allocator giữ PA, không giữ VA

Free-list metadata lưu PA để tiếp tục hoạt động sau khi TTBR0 runtime root trở thành empty map. Nếu lưu VA của lower-half identity, allocator sẽ hỏng sau bước install empty TTBR0 root.

### FP/SIMD bật sớm

`CPACR_EL1.FPEN = 3` được set trong `_start`, trước mọi C code. Exception vectors save/restore full FP context. Kernel không cần `-mgeneral-regs-only`.

### Debugging MMU

Công cụ hữu ích nhất: QEMU trace với `-d int,mmu,guest_errors`. `AT S1E1R` + `PAR_EL1` hữu ích nhưng không thay thế được việc kiểm tra instruction fetch thực tế.

### Loader phải snapshot metadata file trước `fs_close()`

`fs_close()` reset lại `struct file`. Vì vậy mọi loader helper phải snapshot các field như `file.size` trước khi đóng file. Nếu đọc xong rồi mới so `read_count` với `file.size` sau `fs_close()`, kết quả sẽ fail giả dù I/O đã thành công.

### User ELF và shared library cần layout linker nhất quán

- user app tự chứa dùng linker script zero-based + `-pie` để tạo `ET_DYN` thật, rồi runtime cộng `load_bias`
- shared object và app có `DT_NEEDED` phải giữ nguyên metadata động cần thiết; vì vậy build hiện strip bằng `--strip-debug`, không dùng `--strip-all`
- AArch64 `ld` cần được force `-Wl,-z,max-page-size=0x1000` để tránh PT_LOAD đầu tiên bị đẩy tới offset `0x10000` và làm file phình vô ích

### Blocking syscall path phải preserve cả `SP_EL0`

`ELR_EL1` và `SPSR_EL1` chỉ giải quyết nửa bài toán return-to-user. Khi user task có thể bị schedule-out ngay bên trong syscall handler, `SP_EL0` cũng phải được save/restore qua exception frame. Nếu không, task có thể quay lại đúng EL0 PC nhưng với stack pointer user cũ hoặc sai task, dẫn tới fault trên local stack accesses ngay sau khi syscall return.

---

## 16. Trạng thái các stage

| Stage | Tên | Trạng thái |
|---|---|---|
| 1 | Boot and bring-up | ✅ Hoàn thành |
| 2 | Console and logging | ✅ Hoàn thành |
| 3 | Exception vectors and fault reporting | ✅ Hoàn thành |
| 4 | Timer and interrupts | ✅ Hoàn thành |
| 5 | Physical memory management | ✅ Hoàn thành |
| 6 | MMU and kernel virtual memory | ✅ Hoàn thành |
| 7 | Kernel heap | ✅ Hoàn thành |
| 8 | Scheduler and kernel threads | ✅ Hoàn thành |
| 9 | EL0 and syscalls | ✅ Hoàn thành |
| 10 | Processes and address spaces | 🔄 Đang tiến hành (increments 1-8 đã chạy ổn) |
| 11 | IPC and synchronization | 🔄 Đang tiến hành (spinlock + fixed IPC channels đã có) |
| 12 | Filesystem and program loading | ✅ Hoàn thành |
| 13 | Console shell | ✅ Hoàn thành |

### Stage 10 — trạng thái increment 1

- TTBR0 runtime root đã là owned empty lower-half root (không còn boot identity map)
- Low VA kernel alias fault như mong đợi
- TTBR1 high alias vẫn translates
- Boot TTBR0 identity tables đã được freed (runtime: 6 table pages)
- Bước tiếp theo: physical sharing cho shared library pages, object cache/refcount, và ABI/version policy cho dependency resolution

---

*Tài liệu này mô tả trạng thái implementation thực tế. Để biết chi tiết từng subsystem, đọc các tài liệu chuyên sâu: [mmu_design.md](../mmu_design.md), [heap_design.md](../heap_design.md), [page_alloc_design.md](../page_alloc_design.md). Để nắm trạng thái làm việc gần nhất, đọc [handoff.md](../handoff.md).*
