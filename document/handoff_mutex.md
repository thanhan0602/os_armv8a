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
    - **Mutex Pool**: Quản lý tập trung 32 mutex bằng ID, cho phép các process forked từ cùng một cha hoặc khác cha (trong tương lai) tham chiếu cùng một tài nguyên khóa.
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
- [ ] Hỗ trợ **Condition Variables** (`pthread_cond_t`) để hoàn thiện bộ công cụ POSIX sync.
- [ ] Cải thiện bộ cấp phát Mutex Pool (dynamic allocation thay vì static array nếu có nhu cầu).
- [ ] Hỗ trợ Priority Inheritance nếu hệ thống gặp vấn đề Priority Inversion nghiêm trọng.
