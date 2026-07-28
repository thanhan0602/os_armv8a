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
- `RUN_OS_DEMOS`

Mặc định ba cờ `DEBUG_*` hiện là `0`, nên boot thành công chỉ còn log tối thiểu rồi vào shell serial: `[info] kernel init complete`, tiếp theo là `[shell] ready` và prompt `os>`. Khi cần soi boot/MMU cũ, có thể bật lại tạm thời bằng cách truyền `DEBUG_PRE_MMU=1 DEBUG_MMU_BOOT=1 DEBUG_POST_MMU=1` vào `make`.

Mặc định `RUN_OS_DEMOS=0`, nên kernel giữ quiet boot. Khi cần verify runtime path của Stage 10/12, hãy dùng `make clean all RUN_OS_DEMOS=1`; profile này sẽ spawn hai user process demo qua đường `ramfs -> loader -> process`, rồi đi qua syscall kiểu Linux, `brk`, scheduler, và EL0 fault path. Ở trạng thái hiện tại, `brk` hỗ trợ cả grow lẫn shrink theo page mapping thực tế, ASID của process được recycle khi reap, user code page chỉ executable tại EL0 (PXN bật ở EL1), và file-backed loader tối thiểu đã được verify end-to-end trên QEMU.

Repo hiện cũng có cây `user/` cho user-space ELF apps thật. Hai app built-in là `hello` và `fault` được embed vào kernel dưới dạng `/bin/hello.elf` và `/bin/fault.elf`; app external mẫu hiện tại là `ticker`, được build ra `build/user/external/ticker.elf` để nạp runtime qua shell.

## Stage 13 Shell

Sau khi boot, kernel hiện mở một shell serial tối thiểu trên UART với prompt `os>`. Bộ lệnh hiện tại:

- `help`
- `read <path> [count]`
- `write <text>`
- `show process` hoặc `ps`
- `show memory` hoặc `memory` hoặc `mem`
- `load <path> [task-name]`
- `unload <task-id>`
- `receive <path> <size>`

Giới hạn hiện tại:

- `read` đọc được cả file built-in lẫn file runtime đã nạp vào dynamic ramfs
- `write` mới chỉ in ra console, chưa có filesystem write path
- `receive` hiện nhận hex stream thô qua UART; để upload file lớn ổn định hơn, nên dùng serial TCP hoặc một host-side script thay vì paste tay

## User ELF workflow

Build user apps cùng kernel:

```bash
make clean all
```

Các artefact chính:

- `build/user/builtin/hello.elf`
- `build/user/builtin/fault.elf`
- `build/user/external/ticker.elf`

Ví dụ thao tác trong shell:

```text
os> read /bin/hello.elf 32
os> load /bin/hello.elf hello
os> receive /ext/ticker.elf 8496
... gửi 8496 byte dưới dạng hex stream ...
os> load /ext/ticker.elf ticker
os> unload <task-id>
```

Ghi chú build: user ELF hiện link với `-Wl,-z,max-page-size=0x1000` để tránh AArch64 `ld` đẩy PT_LOAD offset lên `0x10000`, vì điều đó làm file nhỏ bị phình lên khoảng `70 KiB` và khiến upload runtime kém thực tế.

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

- Makefile hiện mặc định dùng `QEMU_BIN=qemu-system-aarch64` nếu không override.
- Thư mục `build/` là output build và không nên commit.