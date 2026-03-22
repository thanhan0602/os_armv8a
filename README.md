# os_armv8a

Kernel bare-metal ARMv8-A chạy trên QEMU `virt`, được phát triển theo từng stage từ boot ban đầu đến memory management và MMU.

## Trạng thái hiện tại

- Boot vào EL1 qua `_start`
- UART PL011 hoạt động
- Logging kernel hoạt động
- Exception vectors đã được cài đặt
- GICv2 và generic timer IRQ hoạt động
- Physical page allocator hoạt động
- Kernel heap page-backed hoạt động
- MMU và cache đã được bật ổn định

Chi tiết tiến độ nằm trong `roadmap.md` và `handoff.md`.

## Yêu cầu môi trường

- `make`
- Toolchain AArch64 GNU, mặc định dùng prefix `aarch64-linux-gnu-`
- QEMU AArch64

Các biến có thể override:

- `CROSS_COMPILE`
- `QEMU_BIN`
- `DEBUG_PRE_MMU`
- `DEBUG_MMU_BOOT`
- `DEBUG_POST_MMU`

## Build

```bash
make
```

## Chạy trên QEMU

```bash
make run
```

## Chạy debug QEMU

```bash
make debug
```

Nếu cần chỉ định binary QEMU riêng:

```bash
make QEMU_BIN=/path/to/qemu-system-aarch64 run
```

## Cấu trúc chính

```text
src/
  arch/arm/         Boot code và exception vectors
  drivers/          PL011 UART, GICv2
  include/          Header files
  kernel/           Core kernel subsystems
  linker.ld         Linker script
scripts/
  run_qemu.sh
  run_qemu_debug.sh
document/
reference/
```

## Tài liệu nên đọc

- `handoff.md`
- `roadmap.md`
- `mmu_design.md`
- `page_alloc_design.md`
- `heap_design.md`

## Lệnh thường dùng

```bash
make
make run
make debug
make clean
```

## Ghi chú

- Makefile hiện mặc định dùng `QEMU_BIN=/home/a/qemu/build/qemu-system-aarch64` nếu không override.
- Thư mục `build/` là output build và không nên commit.