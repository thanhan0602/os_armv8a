# Báo cáo Debug CoW và SMP Scheduler

## 1. Vấn đề 1: Lỗi nhận diện lỗi CoW (Copy-on-Write)
- **Triệu chứng**: Khi user process ghi vào vùng nhớ dùng chung, kernel coi đó là lỗi truy cập bộ nhớ nghiêm trọng và giết process thay vì thực hiện CoW.
- **Nguyên nhân**: Trong trình xử lý lỗi MMU, bitmask để lấy mã `DFSC` (Data Fault Status Code) từ thanh ghi `ESR_EL1` bị sai (`0xF` thay vì `0x3C`).
- **Khắc phục**: 
    - Cập nhật bitmask trong [src/kernel/mmu_vmsa.c](src/kernel/mmu_vmsa.c) (hoặc file liên quan) thành `0x3C`.
    - Điều này giúp kernel nhận diện đúng `0xF` (Permission Fault, Level 3) là sự kiện cần kích hoạt CoW.

## 2. Vấn đề 2: Kernel Panic (EL1 Instruction Abort) trên SMP
- **Triệu chứng**: Hệ thống crash với `ELR` trỏ đến một chuỗi ASCII (ví dụ: `0x31337830...` - "at 0x31") khi chạy demo CoW trên nhiều core.
- **Nguyên nhân (Race Condition)**: 
    - Khi `process_fork` tạo task mới, `task_create_user` mặc định đặt trạng thái là `TASK_STATE_READY`.
    - Do chạy đa nhân, Core 1-3 có thể lập tức "cướp" task mới này để chạy ngay khi nó chưa kịp copy xong Stack từ cha.
    - Dẫn đến việc nhảy vào vùng stack rác hoặc bị ghi đè dữ liệu, gây hỏng thanh ghi `x30` (từ `fork_child_exit`).
- **Khắc phục**:
    - Chuyển trạng thái mặc định của task mới tạo sang `TASK_STATE_BLOCKED`.
    - Chỉ sau khi cha đã hoàn tất việc chuẩn bị Context và Stack cho con, mới gọi `sched_wake_task(child)`.
    - `sched_wake_task` cũng thực hiện gửi **IPI (Inter-Processor Interrupt)** để báo cho các core khác biết có task mới.

## 3. Cải tiến quan sát (Observability)
- **Định danh log**: Sửa đổi `log_printf` để tự động thêm Prefix `[name:pid]` vào mỗi dòng log. Điều này cực kỳ hữu ích khi debug đa nhân để biết log đến từ process nào.
- **Syscall mới**: Thêm `sys_getpid` (ID 172) để user process tự biết PID của mình.

## 4. Tự động hóa và Ổn định
- **QEMU Timeout**: Thêm `timeout 15s` vào script chạy để tự động đóng emulator sau khi demo kết thúc, tránh treo resource.
- **Default Loading**: Cấu hình lại [src/kernel/main.c](src/kernel/main.c) để luôn load các process mặc định (`hello`, `fault`, `test_cow`) mà không phụ thuộc vào flag debug.

## Kết quả hiện tại
- Demo CoW chạy thành công: `CoW SUCCESS: Parent data unchanged!`.
- Không còn hiện tượng Panic ngẫu nhiên khi Fork trên SMP.
- Log hệ thống rõ ràng, dễ theo dõi flow giữa Cha và Con.
