# Handoff

## Mục tiêu của repo

Repo này là một kernel ARMv8-A bare-metal chạy trên QEMU `virt`, đang được phát triển theo từng stage từ boot, logging, exception, timer, memory management, đến MMU.

## Trạng thái hiện tại

- Stage 1 đến Stage 9 đã hoàn thành.
- **Stage 9 (EL0 + Syscall ABI)**: user task chạy ở EL0, SVC syscalls hoạt động (SYS_WRITE=1, SYS_YIELD=2, SYS_EXIT=3), user task in "hello from EL0" 3 lần rồi exit sạch sẽ.
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
- Shared debug target framework hiện cũng bao phủ heap arena inspection qua `heap-arenas` và `heap-large-arenas`; heap self-test đã đi qua cùng framework này.
- MMU đã bật ổn định: boot path dùng TTBR0 identity + TTBR1 kernel VA khi virtual, hoặc chỉ TTBR0 khi identity.
- Khi `CONFIG_KERNEL_VIRTUAL=1`, sau trampoline kernel hiện giữ TTBR1 cho kernel runtime và thay TTBR0 identity root bằng một empty lower-half root được kernel sở hữu; TTBR0 walks vẫn bật nhưng low VA kernel aliases giờ fault như mong đợi.
- Đây là increment Stage 10 đầu tiên: runtime đã cắt phụ thuộc vào boot identity map và giữ sẵn lower-half root để đi tiếp tới address spaces riêng.
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
    - sau khi nhảy sang high VA, runtime cài `TTBR0_EL1` sang một empty lower-half root riêng và giải phóng boot TTBR0 identity tables
    - `mmu table pages=6` ở runtime sau handoff (1 empty TTBR0 root + 5 TTBR1 pages)
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

- **ELR_EL1/SPSR_EL1 must be saved in the exception frame**: these are hardware registers that get overwritten by any new exception (e.g. timer IRQ on another task). If not saved in `save_context` and restored in `restore_context`, `eret` after `sys_yield` jumps to the wrong address (another task's interrupted EL1 PC instead of back to EL0 user code). Fix: extend frame CTX_SIZE 784→800, save ELR at offset 248 and SPSR at offset 256; `restore_context` does `msr elr_el1`/`msr spsr_el1` + `isb` before `eret`. This also fixes a latent bug affecting EL1 tasks.
- Một bug MMU lớn trước đây đến từ việc table descriptor thiếu bit `VALID`.
- Table descriptor đúng phải là `VALID | TABLE`.
- `AT S1E1R` và `PAR_EL1` hữu ích nhưng không đủ để thay thế việc kiểm tra instruction fetch thực tế.
- QEMU trace với `-d int,mmu,guest_errors` là công cụ quan trọng khi debug MMU.
- FP/SIMD đã được enable sớm trong boot, nên không còn phụ thuộc vào `-mgeneral-regs-only`.
- Deliberate `brk` đã được verify lại sau khi thêm register dump; fault log hiện có cả `ESR/ELR/SPSR/FAR` lẫn `x0..x30`.
- **VMA=VA pointer-in-data bug**: khi đặt VMA=0xFFFF... trong linker.ld, các `const char *` bên trong `static const struct` arrays (.rodata) chứa VA tuyệt đối. Pre-MMU C code dereference chúng trước khi TTBR1 active → infinite fault loop. Giải pháp: giữ VMA=PA, trampoline dùng `adrp+add+offset` để nhảy sang VA tại runtime.
- **Post-TTBR0 static-table pointer lesson**: vì linker VMA vẫn là PA, các con trỏ string lưu trong `static const` tables tiếp tục là PA. Code chạy sau khi TTBR1 active và TTBR0 bị tắt phải translate các con trỏ đã lưu này (ví dụ `pa_to_va`) trước khi dereference.
- **Post-empty-TTBR0 allocator lesson**: free-list metadata hoặc stored pointers không được ngầm assume lower-half identity còn sống. Page allocator free-list hiện giữ physical addresses để tiếp tục hoạt động sau khi TTBR0 runtime root trở thành empty map.

## Tài liệu nên đọc trước khi tiếp tục

- `roadmap.md`
- `mmu_design.md`
- `heap_design.md`
- `mmu_report_bug.md`
- `mmu_postmortem.md`

## Các file code quan trọng

- `src/kernel/main.c`
- `src/kernel/syscall.c`
- `src/kernel/debug_targets.c`
- `src/kernel/heap.c`
- `src/kernel/mmu.c`
- `src/kernel/page_alloc.c`
- `src/kernel/timer.c`
- `src/kernel/exception.c`
- `src/kernel/sched.c`
- `src/arch/arm/switch.S`
- `src/arch/arm/user_task.S`
- `src/arch/arm/start.S`
- `src/arch/arm/exception_vectors.S`
- `src/linker.ld`

## Trạng thái AI tooling hiện tại

- `.github/copilot-instructions.md` đã define workflow đọc context theo lớp: bắt đầu từ `handoff.md`, rồi chỉ đọc đúng doc/subsystem cần thiết.
- Workspace hiện có custom agent set trong `.github/agents` theo flow `Orchestrator -> Code -> Review`: `Orchestrator` điều phối, `Code` implement và validate, `Review` kiểm tra correctness/regression mà không sửa code.
- Local RAG API hiện nằm ở `tools/rag`, dùng shared SQLite + FAISS/Numpy retrieval tại `.rag-store/index.sqlite3`.
- MCP prototype hiện nằm ở `tools/mcp` với topology `1 + 3`:
  - `gateway`: route query và merge kết quả
  - `design`: doc-first design retrieval
  - `coding`: code-only retrieval, giữ symbol-aware search
  - `document`: document retrieval cho handoff/README/note
- Gateway và specialist MCP đều reuse cùng RAG store, không tạo index riêng.
- Gateway hiện giữ warm in-process specialist adapters để tránh respawn routing logic qua từng request; các specialist MCP server riêng vẫn còn để chạy standalone.
- Tài liệu thiết kế MCP hiện ở `document/mcp_architecture.md`.
- `tools/vscode-qemu-log` hiện có thêm command `qemuInspector.start`: một webview visualizer cho page/MMU state. Inspector resolve symbol từ `build/kernel8.elf` bằng `aarch64-linux-gnu-nm`, rồi đọc physical memory live qua QEMU **HMP monitor socket** (`xp`, `stop`, `cont`, `info status`) thay vì dựa vào UART log hoặc external GDB.
- Hướng QMP/GDB đã được loại trong workspace hiện tại vì `aarch64-linux-gnu-gdb` không có sẵn và QMP greeting không ổn định khi validate; HMP monitor đã được verify runtime thành công.

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
- `src/kernel/heap.c`
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
- `heap-arenas`
- `heap-large-arenas`

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
- `heap-arenas`: dump mọi heap arena qua shared target framework
- `heap-large-arenas`: chỉ dump các arena có ít nhất `2` page, hữu ích để soi allocation lớn

Heap self-test trong `kernel_main()` hiện gọi `kernel_debug_log_heap_targets()` thay cho ad hoc page-range dump, nên log heap arena và log page allocator dùng cùng naming và flow debug target.

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

Nếu log heap cần đọc nhanh, nên nhìn theo thứ tự:

1. `debug target=heap-arenas`
2. `debug target=heap-large-arenas`
3. `heap pages=... used_bytes=... free_bytes=...`

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
3. Dùng empty TTBR0 runtime root hiện có làm base cho lower-half mappings riêng theo process/user.
4. Thêm guard pages cho stack hoặc các vùng nhạy cảm.
5. Tiếp tục các increment Stage 10 tiếp theo cho address spaces riêng.

## Ghi chú quyết định kiến trúc

- TTBR1 kernel VA map đã active (khi `CONFIG_KERNEL_VIRTUAL=1`). TTBR0 identity map chỉ giữ cho boot path; runtime đổi sang empty lower-half root riêng.
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
- MCP ops now exposes `git_push` through both the ops specialist and the gateway; it defaults to `origin`, uses the current branch when possible, rejects missing-upstream and no-op pushes, and never enables force push behavior.
