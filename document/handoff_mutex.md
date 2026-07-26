# Mutex Subsystem Handoff

## Mục tiêu
- Cung cấp cơ chế đồng bộ hóa chặn (blocking synchronization) cho kernel và user-space.
- Thay thế spinlocks trong các đoạn code có critical section dài để tránh lãng phí CPU.
- Đảm bảo an toàn trong môi trường đa nhân (SMP).
- Hỗ trợ đồng bộ xuyên process (cross-process sync) thông qua Mutex Pool.

## Trạng thái hiện tại
- **Đã hoàn thành**:
    - Kernel mutex với hàng đợi chờ (wait-queue) dựa trên `task->wait_next`.
    - Hỗ trợ SMP-safe thông qua spinlock nội bộ và IRQ disabling.
    - **Mutex Pool**: Quản lý tập trung 64 mutex bằng ID, cho phép các process chia sẻ cùng một tài nguyên khóa.
    - **Syscall interface**: 
        - `SYS_MUTEX_ALLOC` (500)
        - `SYS_MUTEX_LOCK` (501)
        - `SYS_MUTEX_UNLOCK` (502)
        - `SYS_MUTEX_TRYLOCK` (503)
        - `SYS_MUTEX_FREE` (504)
    - **Pthread library**: Implement đầy đủ `pthread_mutex_init`, `lock`, `unlock`, `trylock`, `destroy` trong [user/include/pthread.h](user/include/pthread.h).
- **Verify**:
    - User app [user/apps/test_pthread.c](user/apps/test_pthread.c) mô phỏng tranh chấp giữa 3 thread (process forked), kiểm tra cả blocking path và non-blocking path (trylock).

## File chính
- [src/include/kernel/mutex.h](src/include/kernel/mutex.h): Định nghĩa `struct mutex`.
- [src/kernel/mutex.c](src/kernel/mutex.c): Thực thi logic lock/unlock, pool management.
- [src/kernel/syscall.c](src/kernel/syscall.c): Dispatcher cho các syscall synchronization.
- [user/include/pthread.h](user/include/pthread.h): Cung cấp API POSIX-like cho EL0.

## Invariant/Assumption
- `mutex_lock` **phải** được gọi trong context có thể sleep (không được gọi trong interrupt handler).
- Việc giải phóng mutex (`mutex_unlock`) sẽ chuyển trực tiếp quyền sở hữu cho task đứng đầu hàng đợi chờ (FIFO) để tránh thundering herd và đảm bảo fairness.
- User-space giữ ID của mutex trong cấu trúc `pthread_mutex_t`. Memory tại EL0 không được dùng để lưu trạng thái khóa nhằm hỗ trợ fork-based threading.
- Waiter được thêm dưới `mutex->lock`, nhưng mutex lock luôn được nhả trước khi gọi `sched_park_task()` hoặc `sched_unpark_task()`.
- Wake xảy ra trước park được giữ lại bằng `wake_pending`, do đó ownership handoff không làm mất wakeup.
- `mutex_detach_task()` xóa task chết khỏi wait queue; nếu task là owner, ownership được chuyển cho waiter kế tiếp hoặc mutex được mở khóa.
- Mỗi thao tác trên mutex pool phải pin slot bằng `active_ops`. Slot có `destroying` không nhận operation mới và chỉ được tái sử dụng khi unlocked, không owner, không waiter và không operation đang hoạt động.

## SMP hardening đã hoàn thành

- Đã loại bỏ cạnh khóa `mutex->lock -> sched_lock`; scheduler chỉ được gọi sau khi nhả mutex lock.
- `sched_park_task()`/`sched_unpark_task()` cung cấp handshake một-bit để đóng cửa sổ lost wakeup giữa enqueue và block.
- Reaper gọi `mutex_detach_task()` trước khi clear task slot, loại bỏ dangling waiter và owner pointer.
- Các syscall pool dùng `mutex_pool_pin()`/`mutex_pool_unpin()` thay cho việc trả raw pointer không được bảo vệ.
- `mutex_pool_free()` đặt `destroying`, từ chối destroy khi còn active operation, owner, waiter hoặc trạng thái locked.
- Test pthread/mutex trên QEMU 4 CPU đã hoàn tất với `Complex Test Finished.` và các thread thoát `code=0`.
- SMP regression đã xác minh trường hợp owner bị kill khi có waiter đang BLOCKED: reaper detach owner, handoff mutex cho waiter, waiter được wake và pool slot có thể được giải phóng. Marker: `[stress] mutex-owner-detach PASS`.
- SMP regression cũng xác minh trường hợp waiter đang BLOCKED bị kill. `mutex_detach_task()` xóa waiter khỏi queue và giải phóng `active_ops` pin mà task bị kill không thể tự unpin khi quay về từ `mutex_pool_lock()`. Owner sau đó unlock bình thường và pool slot được giải phóng. Marker: `[stress] mutex-waiter-detach PASS`.
- Trước khi cho owner unlock, regression gọi `mutex_pool_free()` đồng thời và xác minh destroy bị từ chối khi mutex còn locked/có owner. Marker: `[stress] mutex-concurrent-destroy PASS`.

## Lệnh verify nhanh
- Build và chạy QEMU:
  ```bash
  make all
  ./scripts/run_qemu.sh
  ```
- Trong shell của OS:
  ```bash
  run test_pthread.elf
  ```

## Next steps
- [x] Thêm regression test kill owner và handoff cho waiter.
- [x] Thêm regression test kill waiter đang BLOCKED và phát hiện operation-pin leak.
- [x] Xác minh destroy bị từ chối khi mutex còn locked/có owner.
- [ ] Thêm stress test nhiều vòng cho destroy cạnh tranh với lock/trylock/unlock.
- [ ] Cân nhắc generation counter cho mutex ID để phát hiện stale handle sau khi slot được tái sử dụng.
- [ ] Hỗ trợ **Condition Variables** (`pthread_cond_t`) để hoàn thiện bộ công cụ POSIX sync.
- [ ] Cải thiện bộ cấp phát Mutex Pool (dynamic allocation thay vì static array nếu có nhu cầu).
- [ ] Hỗ trợ Priority Inheritance nếu hệ thống gặp vấn đề Priority Inversion nghiêm trọng.
