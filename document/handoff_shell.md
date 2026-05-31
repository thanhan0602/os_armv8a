# Shell handoff

## Mục tiêu
- Cung cấp giao diện shell tối thiểu trên UART, nhận lệnh quản lý process, memory, file, load app, debug.

## Trạng thái hiện tại
- Đã hoàn thành: polling RX, command read/write/show process/show memory/load/unload/help, shell chạy từ idle loop.
- Đã verify: shell prompt ổn định, các lệnh hoạt động, unload kill task theo id.
- Known issues: chưa có IRQ-driven console, chưa có command history, chưa có scripting.

## File chính
- src/kernel/shell.c
- src/include/kernel/shell.h
- src/kernel/console.c
- src/include/kernel/console.h

## Invariant/Assumption
- Shell chạy trực tiếp từ idle loop, không có thread riêng.
- Command parse đơn giản, không có pipeline hay redirect.

## Lệnh verify nhanh
- make clean all
- QEMU boot, nhập lệnh ps, memory, read, write, load, unload, kiểm tra output đúng.

## Pitfall/Debug note
- Khi shell không nhận input, kiểm tra UART RX và polling loop.
- Khi command không chạy, kiểm tra parse và dispatch.

## Next steps
- Thêm IRQ-driven console.
- Thêm command history, scripting.

---

*Handoff này chỉ tóm tắt trạng thái, invariant, file chính, và trap debug. Khi có thay đổi lớn, cập nhật delta vào đây.*
