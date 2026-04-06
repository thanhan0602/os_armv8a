# Handoff

## Mục tiêu của repo

Repo này là một kernel ARMv8-A bare-metal chạy trên QEMU `virt`, đang được phát triển theo từng stage từ boot, logging, exception, timer, memory management, đến MMU.

## Trạng thái hiện tại

- Stage 1 đến Stage 8 đã hoàn thành.
- Kernel virtual layout hoàn thành: TTBR1 active, trampoline works, PA→VA migration done.
- Scheduler hoạt động: round-robin preemptive scheduling qua timer IRQ.
- Hỗ trợ hai build variant qua `CONFIG_KERNEL_VIRTUAL`:
  - `make` hoặc `make KERNEL_VIRTUAL=1`: kernel chạy tại high VA qua TTBR1 (mặc định)
  - `make KERNEL_VIRTUAL=0`: kernel chạy tại PA qua identity map (TTBR0 only, EPD1=1)
- Build hiện tại thành công qua `make`.
- Runtime hiện tại boot ổn định trên QEMU cho cả hai variant.
- UART logging hoạt động.
- Exception vectors hoạt động.
- Sync fault path hiện dump đầy đủ `x0..x30`, `SP_EL1`, `FPCR`, và `FPSR`.
- GICv2 và generic timer IRQ hoạt động.
- Physical page allocator hoạt động.
- Kernel heap page-backed hiện hỗ trợ allocation nhỏ và allocation lớn hơn một page thông qua contiguous physical spans.
- MMU đã bật ổn định với cả TTBR0 (identity) và TTBR1 (kernel VA) khi virtual, hoặc chỉ TTBR0 khi identity.
- Cache `SCTLR_EL1.M/C/I` đã được bật và verify.
- Boot flow khi `CONFIG_KERNEL_VIRTUAL=1`: `_start` (PA) → `kernel_main_early` (PA) → trampoline → `kernel_main` (VA qua TTBR1).
- Boot flow khi `KERNEL_VIRTUAL=0`: `_start` (PA) → `kernel_main_early` (PA) → `kernel_main` (PA qua TTBR0).

## Trạng thái MMU hiện tại

- AArch64 EL1
- `48-bit VA`
- `4 KiB` granule
- `4-level translation` rooted at `L0`
- Hai build variant:
  - `CONFIG_KERNEL_VIRTUAL=1` (mặc định):
    - `TTBR0_EL1`: identity map (VA == PA) — dùng cho boot path
    - `TTBR1_EL1`: kernel VA map (VA == PA + `0xFFFF000000000000`) — active sau trampoline
    - `mmu table pages=10` (5 cho TTBR0 + 5 cho TTBR1)
  - `KERNEL_VIRTUAL=0`:
    - `TTBR0_EL1`: identity map (VA == PA) — active suốt runtime
    - `TTBR1_EL1`: tắt bằng `EPD1=1`
    - `mmu table pages=5`
- Hybrid mapping (cùng cấu trúc cho cả hai bộ bảng trang):
  - `L0 -> L1 -> L2 -> L3` cho tối thiểu `2` chunk đầu, mỗi chunk `2 MiB`
  - tổng vùng fine-grained hiện tại là ít nhất `4 MiB`
  - phần RAM còn lại dùng `L2` block mappings
- Permission model hiện tại:
  - `.text`: `RO + X`
  - `.rodata`: `RO + NX`
  - `.data/.bss/.boot_stack`: `RW + NX`
- PA→VA migration hoàn tất: page_alloc, heap, mmu walks, MMIO drivers (pl011, gicv2) đều conditional convert qua `mmu_is_enabled()`

## Các bài học kỹ thuật quan trọng

- Một bug MMU lớn trước đây đến từ việc table descriptor thiếu bit `VALID`.
- Table descriptor đúng phải là `VALID | TABLE`.
- `AT S1E1R` và `PAR_EL1` hữu ích nhưng không đủ để thay thế việc kiểm tra instruction fetch thực tế.
- QEMU trace với `-d int,mmu,guest_errors` là công cụ quan trọng khi debug MMU.
- FP/SIMD đã được enable sớm trong boot, nên không còn phụ thuộc vào `-mgeneral-regs-only`.
- Deliberate `brk` đã được verify lại sau khi thêm register dump; fault log hiện có cả `ESR/ELR/SPSR/FAR` lẫn `x0..x30`.
- **VMA=VA pointer-in-data bug**: khi đặt VMA=0xFFFF... trong linker.ld, các `const char *` bên trong `static const struct` arrays (.rodata) chứa VA tuyệt đối. Pre-MMU C code dereference chúng trước khi TTBR1 active → infinite fault loop. Giải pháp: giữ VMA=PA, trampoline dùng `adrp+add+offset` để nhảy sang VA tại runtime.

## Tài liệu nên đọc trước khi tiếp tục

- `roadmap.md`
- `mmu_design.md`
- `heap_design.md`
- `mmu_report_bug.md`
- `mmu_postmortem.md`

## Các file code quan trọng

- `src/kernel/main.c`
- `src/kernel/debug_targets.c`
- `src/kernel/heap.c`
- `src/kernel/mmu.c`
- `src/kernel/page_alloc.c`
- `src/kernel/timer.c`
- `src/kernel/exception.c`
- `src/kernel/sched.c`
- `src/arch/arm/switch.S`
- `src/arch/arm/start.S`
- `src/arch/arm/exception_vectors.S`
- `src/linker.ld`

## Trạng thái AI tooling hiện tại

- `.github/copilot-instructions.md` đã define workflow đọc context theo lớp: bắt đầu từ `handoff.md`, rồi chỉ đọc đúng doc/subsystem cần thiết.
- Local RAG API hiện nằm ở `tools/rag`, dùng shared SQLite + FAISS/Numpy retrieval tại `.rag-store/index.sqlite3`.
- MCP prototype hiện nằm ở `tools/mcp` với topology `1 + 3`:
  - `gateway`: route query và merge kết quả
  - `design`: doc-first design retrieval
  - `coding`: code-only retrieval, giữ symbol-aware search
  - `document`: document retrieval cho handoff/README/note
- Gateway và specialist MCP đều reuse cùng RAG store, không tạo index riêng.
- Gateway hiện giữ warm in-process specialist adapters để tránh respawn routing logic qua từng request; các specialist MCP server riêng vẫn còn để chạy standalone.
- Tài liệu thiết kế MCP hiện ở `document/mcp_architecture.md`.

## Trạng thái Scheduler hiện tại

- Round-robin preemptive scheduling, driven bằng timer IRQ (~500ms/tick).
- `schedule()` gọi từ `exception_handle_irq()` sau khi EOI.
- Context switch lưu callee-saved registers (`x19-x30`, `SP`) — full GPR/SIMD frame đã được save/restore ở exception vectors.
- Task mới nhận 2 contiguous pages: 1 guard page (bottom) + 1 usable stack page (top). Guard page allocated nhưng không unmap được ở hardware level (vì nằm trong L2 block mapping) — chỉ isolation qua allocation.
- Dead task được reap tại đầu `schedule()`: unlink khỏi circular list, free guard+stack pages.
- Idle task (id=0) chạy trên boot stack, không cần alloc riêng.
- Demo tasks (`task-a`, `task-b`) verify round-robin cycling.

## Framework debug hiện tại

Repo hiện đã có một framework `debug target` thống nhất cho log boot-time, nằm chủ yếu ở:

- `src/kernel/debug_targets.c`
- `src/kernel/mmu.c`
- `src/kernel/page_alloc.c`

Các target hiện có gồm:

- `managed-head`
- `page-a`
- `page-b`
- `alloc-window`
- `mmu-tables`
- `mmu-walk`
- `mmu-probe`

Các phase này hiện có thể bật hoặc tắt qua build flags:

- `DEBUG_PRE_MMU`
- `DEBUG_MMU_BOOT`
- `DEBUG_POST_MMU`

Ý nghĩa nhanh:

- `managed-head`: dump vài page đầu của vùng allocator managed
- `page-a`, `page-b`, `alloc-window`: theo dõi vòng đời page vừa `alloc/free`
- `mmu-tables`: liệt kê các page allocator đang bị MMU giữ làm bảng trang, với tên semantic như `l0-root`, `l1-root`, `l2-ram`, `l3-chunk-0`
- `mmu-walk`: software walk qua `L0/L1/L2/L3`
- `mmu-probe`: hardware probe bằng `AT S1E1R` + `PAR_EL1`

Page allocator hiện cũng có `page_alloc_contiguous/page_free_contiguous` để heap có thể tạo arena lớn hơn một page mà chưa cần heap VA riêng.

Các log consistency quan trọng của page allocator vẫn được in riêng dưới dạng:

```text
[info] page allocator consistency mismatches=0 ...
```

Nếu số `mismatches` khác `0`, nên coi đó là dấu hiệu đầu tiên rằng metadata allocator đã lệch khỏi free list hoặc bookkeeping tổng.

Nếu log MMU cần đọc nhanh, nên nhìn theo thứ tự:

1. `debug target=mmu-tables`
2. `debug target=mmu-walk`
3. `debug target=mmu-probe`
4. `stage 6 mmu enabled`

Nếu log page allocator cần đọc nhanh, nên nhìn theo thứ tự:

1. `debug target=managed-head`
2. `debug target=page-a/page-b/alloc-window`
3. `page allocator consistency mismatches=...`

## Lệnh thường dùng

Build (virtual, mặc định):

```text
make
```

Build (identity only):

```text
make KERNEL_VIRTUAL=0
```

Run:

```text
make run
```

Debug QEMU:

```text
bash scripts/run_qemu_debug.sh
```

## Việc đáng cân nhắc tiếp theo

1. **Hoàn thành**: PA→VA migration — tất cả module đã được chuyển.
2. **Hoàn thành**: Dual build variant (`CONFIG_KERNEL_VIRTUAL=1` / `0`).
3. Xóa TTBR0 identity map khi `CONFIG_KERNEL_VIRTUAL=1`, hoặc repurpose cho user-space.
4. Thêm guard pages cho stack hoặc các vùng nhạy cảm.
5. Chuẩn bị cho address spaces riêng ở các stage sau (Stage 10).

## Ghi chú quyết định kiến trúc

- TTBR1 kernel VA map đã active (khi `CONFIG_KERNEL_VIRTUAL=1`). TTBR0 identity map giữ cho boot path.
- Khi `KERNEL_VIRTUAL=0`, TTBR1 bị tắt bằng EPD1=1, kernel chạy hoàn toàn trên identity map.
- VMA vẫn là PA trong linker.ld để tránh pointer-in-data bug (xem mục bài học kỹ thuật).
- Trampoline dùng `adrp+add+offset` thay vì `ldr =symbol` vì VMA=PA khiến literal pool chứa PA, cần cộng offset runtime.
- `kernel_main` giờ gồm hai phần: `kernel_main_early` (PA) và `kernel_main` (VA hoặc PA tùy variant).
- `KERNEL_VA_OFFSET` được define là `0xFFFF000000000000` khi virtual, `0` khi identity — `pa_to_va()` trở thành no-op trong identity mode.

## Mục tiêu khi mở lại phiên sau

Khi quay lại repo này, nên làm theo thứ tự:

1. Đọc `handoff.md` này.
2. Đọc `roadmap.md` để biết stage hiện tại.
3. Đọc `mmu_design.md` nếu làm tiếp phần MMU.
4. Đọc `page_alloc_design.md` nếu đụng đến allocator hoặc debug target framework.
5. Đọc `heap_design.md` nếu làm tiếp Stage 7.
6. Nếu đụng đến bug MMU cũ, đọc `mmu_report_bug.md` trước khi sửa code.
7. Chạy `make run` để xác nhận baseline vẫn ổn trước khi thay đổi gì thêm.

## MCP Session Updates

- Gateway now keeps warm MCP specialist sessions and document specialist exposes append-only write tools.
- Focused MCP verification confirmed warm specialist reuse and append-only document write tools.
- Gateway now keeps warm MCP specialist sessions and document specialist exposes append-only write tools.
- MCP now includes an `ops` specialist plus gateway proxy tools for `build_kernel`, `start_qemu`, `qemu_status`, `read_qemu_log`, and `stop_qemu`; QEMU state/logs live under `.mcp-runtime/`.
- MCP ops now also supports `build_and_run`, `restart_qemu`, and `gdb_attach_info`; an ops demo client is available via `tools/mcp/run_ops_demo.sh`.
- MCP ops now understands the existing VS Code debug setup via `.vscode/launch.json` and `.vscode/tasks.json`, and exposes `vscode_debug_info` plus `prepare_vscode_debug` for the QEMU attach flow.
