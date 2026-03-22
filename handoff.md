# Handoff

## Mục tiêu của repo

Repo này là một kernel ARMv8-A bare-metal chạy trên QEMU `virt`, đang được phát triển theo từng stage từ boot, logging, exception, timer, memory management, đến MMU.

## Trạng thái hiện tại

- Stage 1 đến Stage 7 đã hoàn thành.
- Build hiện tại thành công qua `make`.
- Runtime hiện tại boot ổn định trên QEMU.
- UART logging hoạt động.
- Exception vectors hoạt động.
- GICv2 và generic timer IRQ hoạt động.
- Physical page allocator hoạt động.
- Kernel heap page-backed hoạt động cho allocation nhỏ.
- MMU đã bật ổn định.
- Cache `SCTLR_EL1.M/C/I` đã được bật và verify.

## Trạng thái MMU hiện tại

- AArch64 EL1
- `48-bit VA`
- `4 KiB` granule
- `4-level translation` rooted at `L0`
- `TTBR0_EL1` đang được dùng cho kernel
- Hybrid mapping:
  - `L0 -> L1 -> L2 -> L3` cho tối thiểu `2` chunk đầu, mỗi chunk `2 MiB`
  - tổng vùng fine-grained hiện tại là ít nhất `4 MiB`
  - phần RAM còn lại dùng `L2` block mappings
- Permission model hiện tại:
  - `.text`: `RO + X`
  - `.rodata`: `RO + NX`
  - `.data/.bss/.boot_stack`: `RW + NX`

## Các bài học kỹ thuật quan trọng

- Một bug MMU lớn trước đây đến từ việc table descriptor thiếu bit `VALID`.
- Table descriptor đúng phải là `VALID | TABLE`.
- `AT S1E1R` và `PAR_EL1` hữu ích nhưng không đủ để thay thế việc kiểm tra instruction fetch thực tế.
- QEMU trace với `-d int,mmu,guest_errors` là công cụ quan trọng khi debug MMU.
- FP/SIMD đã được enable sớm trong boot, nên không còn phụ thuộc vào `-mgeneral-regs-only`.

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
- `src/arch/arm/start.S`
- `src/arch/arm/exception_vectors.S`
- `src/linker.ld`

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

Build:

```text
make
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

1. Mở rộng heap để hỗ trợ object lớn hơn một page hoặc một virtual heap range riêng.
2. Mở rộng vùng `L3` fine-grained nếu có thêm vùng cần permission chi tiết.
3. Thiết kế một `kernel virtual layout` riêng thay vì tiếp tục dựa nhiều vào identity map.
4. Thêm guard pages cho stack hoặc các vùng nhạy cảm.
5. Chuẩn bị cho address spaces riêng ở các stage sau.

## Ghi chú quyết định kiến trúc

- Hiện tại hệ thống vẫn giữ broad identity mapping để ưu tiên ổn định và dễ debug.
- Việc tách sang `kernel virtual layout` là bước kiến trúc hợp lý về sau, nhưng đang được hoãn có chủ ý cho đến khi baseline MMU/cache/timer/exception đủ ổn định.

## Mục tiêu khi mở lại phiên sau

Khi quay lại repo này, nên làm theo thứ tự:

1. Đọc `handoff.md` này.
2. Đọc `roadmap.md` để biết stage hiện tại.
3. Đọc `mmu_design.md` nếu làm tiếp phần MMU.
4. Đọc `page_alloc_design.md` nếu đụng đến allocator hoặc debug target framework.
5. Đọc `heap_design.md` nếu làm tiếp Stage 7.
6. Nếu đụng đến bug MMU cũ, đọc `mmu_report_bug.md` trước khi sửa code.
7. Chạy `make run` để xác nhận baseline vẫn ổn trước khi thay đổi gì thêm.