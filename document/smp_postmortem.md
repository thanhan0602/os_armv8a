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

---

## 4. MMU, Page Allocator và Process Lifetime sau SMP

### Vấn đề phát hiện

Các subsystem MMU và page allocator được phát triển trước SMP nên từng có các composite operation không được khóa đầy đủ:

- contiguous free có thể rebuild free list đồng thời với allocation
- hai thread dùng chung MM có thể cùng fault/map một VA
- CoW có check-then-act race trên PTE và page refcount
- ASID bitmap/counter có thể được cập nhật đồng thời
- page refcount về zero nhưng page chưa được trả về allocator
- fork failure có thể để lại source PTE chuyển sang CoW mà TLB vẫn giữ writable translation

### Cách khắc phục

- Bảo vệ toàn bộ allocator state và consistency snapshot bằng `page_lock`.
- Thêm `mm_context.lock` và các helper nội bộ yêu cầu caller giữ lock.
- Serialize lazy fault, CoW, map, unmap, clone và software page-table walk.
- Dùng broadcast ASID/VA TLB invalidation với barrier phù hợp sau PTE mutation.
- Bảo vệ ASID allocator và giới hạn range theo cấu hình `TCR_EL1.AS=0`.
- Đồng bộ process VM metadata với fault handler theo thứ tự `process -> mm -> page allocator`.
- Chặn multithreaded exec cho đến khi sibling-thread termination được hỗ trợ.

### Kết quả verify

Hai commit liên quan trên branch `fix/mmu-page-allocator-smp` là:

- `8d301d4 fix(mm): harden MMU and page allocator for SMP`
- `ee16d51 fix(smp): harden process and MM lifetime paths`

Clean build và QEMU 4-core pthread workload đã pass. Tuy nhiên đây không phải bằng chứng rằng mọi interleaving SMP đã được bao phủ.

---

## 5. Scheduler, IPC, mutex và MM lifetime hardening

Audit tiếp theo xác định scheduler wait/wake là vùng rủi ro lớn nhất:

- scheduler reaper giữ `sched_lock` rồi đi vào IPC/process cleanup
- IPC và mutex giữ subsystem lock rồi gọi scheduler block/wake
- task state có một số đường ghi ngoài `sched_lock`, gồm nanosleep và remote kill
- mutex pool có thể destroy/reuse slot đồng thời với operation trên CPU khác

Các đường này đã được xử lý như sau:

- `sched_park_task()`/`sched_unpark_task()` dùng wake token để đóng cửa sổ lost wakeup giữa publish waiter và block.
- IPC và mutex lấy waiter dưới lock riêng, nhả lock, rồi mới gọi scheduler.
- Reaper chuyển task sang `TASK_STATE_REAPING` và tháo khỏi run queue dưới `sched_lock`, sau đó mới cleanup subsystem ngoài scheduler lock.
- Remote kill chỉ đặt `kill_pending` cho task RUNNING và gửi IPI; CPU sở hữu task tự chuyển nó sang DEAD.
- Nanosleep, exit status và task-state update đi qua scheduler API có khóa.
- Mutex pool dùng `active_ops` và `destroying`; task chết được detach khỏi wait queue và ownership được handoff an toàn.
- `mm_context` có refcount, `dying`, active CPU mask và deferred release sau khi CPU cuối cùng tháo context khỏi TTBR0.

QEMU SMP 4 CPU đã chạy hoàn tất workload pthread/mutex với `Complex Test Finished.`, các task thoát `code=0` và không có fatal marker. Các test interleaving chuyên biệt vẫn cần được bổ sung vì một stress run không chứng minh mọi race đã bị loại bỏ.

## Bài học bổ sung

3. Có spinlock chưa đủ; phải định nghĩa **một thứ tự khóa toàn hệ thống** và không tạo chu trình giữa scheduler, process, MM, allocator, IPC và mutex.
4. Block task và enqueue waiter phải là một transaction quan sát được nguyên tử, nếu không sẽ có lost wakeup.
5. TLB invalidation chỉ giải quyết stale translation; nó không thay thế lifetime protocol cho page table đang được CPU khác dùng.
6. Stress test thành công là điều kiện cần, không phải chứng minh không còn race. Cần test riêng các interleaving kill/reap, IPC wait/wake, mutex destroy/waiter, fork/CoW và mmap/fault.
