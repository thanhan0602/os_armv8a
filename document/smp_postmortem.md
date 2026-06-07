# Hậu kiểm SMP (Symmetric Multi-Processing)

Tài liệu này ghi lại các lỗi nghiêm trọng phát sinh trong quá trình chuyển đổi từ Single-Core sang Multi-Core (4 CPUs) trên kiến trúc ARMv8-A.

## 1. UART Race Condition & Shell Hang

### Vấn đề
Khi chạy đa nhân, shell bị treo, không phản hồi input hoặc in ra các ký tự rác/đứt đoạn.

### Nguyên nhân gốc
- **Thiếu đồng bộ**: Cả 4 core đều có thể truy cập vào thanh ghi UART (PL011) cùng lúc để in log hoặc đọc input.
- **Race Condition**: Một core đang kiểm tra `FR_RXFE` (FIFO empty) thì bị ngắt, core khác vào đọc mất dữ liệu, hoặc hai core cùng ghi vào `DR` làm dữ liệu bị trộn lẫn.
- **MMU Consistency**: Khi MMU bật, địa chỉ UART chuyển từ Physical Address sang Virtual Address. Nếu một core truy cập PA trong khi core khác đang dùng VA (hoặc ngược lại), sẽ dẫn đến lỗi truy cập vùng nhớ.

### Cách khắc phục
- **Spinlock**: Sử dụng `uart_lock` để bảo vệ mọi thao tác đọc/ghi vào PL011.
- **Interrupt Safety**: Sử dụng `spin_lock_irqsave` để đảm bảo không bị ngắt giữa chừng khi đang giữ lock UART.
- **MMU-Aware Access**: Tạo hàm helper `mmio_va` để tự động chọn PA hoặc VA tùy theo trạng thái MMU hiện tại.

---

## 2. Kernel Panic (Data Abort) trong Scheduler

### Vấn đề
Kernel bị panic với lỗi `EL1 data abort` ngay sau khi bật MMU và đánh thức các core thứ cấp. Địa chỉ lỗi thường nằm ở vùng Physical Address (ví dụ `0x4008xxxx`).

### Nguyên nhân gốc
- **Pointer Inconsistency**: Cấu trúc `tasks` và con trỏ `current` (lưu trong `tpidr_el1`) được khởi tạo trước khi bật MMU (sử dụng Physical Address).
- **VA/PA Mismatch**: Khi MMU được bật, kernel chạy ở dải địa chỉ cao (`0xffff0000...`). Tuy nhiên, con trỏ `prev->next` trong hàm `schedule()` vẫn trỏ tới địa chỉ vật lý thấp do danh sách liên kết vòng được thiết lập sớm.
- **tpidr_el1 corrupt**: Trên Core 0, `tpidr_el1` vẫn giữ giá trị PA của `tasks[0]`. Khi `schedule()` cố gắng dereference con trỏ này trong môi trường VA, CPU ném ra exception vì không có mapping cho vùng nhớ thấp ở EL1.

### Cách khắc phục
- **Force VA for TPIDR**: Trong `kernel_main` (sau khi nhảy lên VA), thực hiện ghi lại `tpidr_el1` bằng địa chỉ ảo của task hiện tại:
  ```c
  arch_set_current_task(pa_to_va(&tasks[0]));
  ```
- **Update Linker Pointers**: Cập nhật lại toàn bộ các con trỏ `next` trong danh sách `tasks` sang địa chỉ ảo trước khi bắt đầu lập lịch đa nhân.
- **Late Sched Init**: Di chuyển phần lớn logic khởi tạo scheduler (đặc biệt là thiết lập task list) vào sau khi MMU đã ổn định ở chế độ VA.

---

## 3. Secondary Cores Wake-up Race

### Vấn đề
Các core thứ cấp (CPU 1-3) không online ổn định hoặc gây panic ngay khi vừa thoát khỏi `psci_cpu_on`.

### Nguyên nhân gốc
- **Stack Collision**: Nếu các core dùng chung một vùng stack khởi tạo hoặc stack không được căn chỉnh 16-byte trên ARM64.
- **Early Interrupts**: GICv2 chưa được cấu hiệu đúng cho từng CPU (Per-CPU Interface) dẫn đến việc core nhận được ngắt khi chưa sẵn sàng.

### Cách khắc phục
- **Per-CPU Stacks**: Cấp phát vùng stack riêng biệt cho từng core dựa trên `mpidr_el1`.
- **Barrier Synchronization**: Sử dụng các flag trạng thái (`cpu_ready`) và `dmb` (Data Memory Barrier) để đảm bảo CPU 0 đã chuẩn bị xong dữ liệu trước khi các CPU khác bắt đầu thực thi code kernel chính.

## Bài học kinh nghiệm
1. Trong SMP, **mọi** tài nguyên dùng chung (UART, Scheduler, Page Allocator) bắt buộc phải có khóa (Spinlocks).
2. Phải đảm bảo tính nhất quán của địa chỉ (VA vs PA) trên **tất cả** các core ngay khi MMU được kích hoạt. Con trỏ lưu trong thanh ghi hệ thống (`tpidr_el1`, `ttbrx_el1`) là nơi dễ bị bỏ quên nhất.
