# UART handoff

## Mục tiêu
- Giao tiếp UART PL011, nhận/gửi dữ liệu serial, hỗ trợ shell và debug log.

## Trạng thái hiện tại
- Đã hoàn thành: polling RX/TX, pl011_can_read, pl011_read, pl011_write, shell qua UART.
- Đã verify: shell nhận input/output qua UART, log qua UART ổn định.
- Known issues: chưa có IRQ-driven UART, chưa có flow control.

## File chính
- src/drivers/uart/pl011.c
- src/include/drivers/uart/pl011.h
- src/kernel/console.c
- src/include/kernel/console.h

## Invariant/Assumption
- UART chỉ dùng polling, không có IRQ.
- Shell/console luôn qua UART.

## Lệnh verify nhanh
- make clean all
- QEMU boot, nhập lệnh shell qua UART, kiểm tra log output.

## Pitfall/Debug note
- Khi không nhận input, kiểm tra polling RX.
- Khi log không ra UART, kiểm tra pl011_write.

## Next steps
- Thêm IRQ-driven UART.
- Thêm flow control.

---

*Handoff này chỉ tóm tắt trạng thái, invariant, file chính, và trap debug. Khi có thay đổi lớn, cập nhật delta vào đây.*
