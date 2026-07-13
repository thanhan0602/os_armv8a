# Scheduler handoff

## Mục tiêu
- Quản lý chuyển đổi context, lập lịch preemptive round-robin, đảm bảo công bằng giữa các task.

## Trạng thái hiện tại
- Đã hoàn thành: round-robin preemptive scheduling qua timer IRQ, context switch lưu full GPR/SIMD, reap dead task, idle task.
- SMP Support: Hỗ trợ đa nhân (4 cores), mỗi core có idle task riêng, đồng bộ qua `sched_lock`. Hỗ trợ IPI để kích hoạt lập lịch lại khi có task được wake up.
- Mutex Support: Đã implement `struct mutex` với hàng đợi chờ (wait queue) tích hợp scheduler. Task sẽ bị block khi không lấy được khóa và được wake up khi khóa giải phóng.
- pthread/SMP stability: idle task pinned theo CPU, scheduler chỉ reap DEAD task khi `current_cpu == TASK_NO_CPU`, và idle loops đã bật lại `wfe` sau khi fix IPI/timer lost wakeup.
- Đã verify: demo task-a, task-b cycling, user-a, user-b chạy/exit sạch, idle giữ boot stack. Mutex đã được verify qua 2 kernel tasks chạy trên các CPU khác nhau. `test_pthread.elf` pass repeated 4-core QEMU stress runs.

## File chính
- src/kernel/sched.c
- src/include/kernel/sched.h
- src/kernel/mutex.c
- src/include/kernel/mutex.h
- src/arch/arm/switch.S

## Invariant/Assumption
- Context switch luôn save/restore ELR_EL1/SPSR_EL1.
- Idle task cho CPU n có id = n, không alloc stack riêng.
- User task đang chạy có `current_cpu=<cpu>`; khi switch ra khỏi user task thì scheduler đặt `current_cpu=TASK_NO_CPU`. Dead task chỉ được reap ở đầu `schedule()` khi đã rời CPU để không free kernel stack đang active.
- Mutex dùng spinlock nội bộ để bảo vệ wait queue, và gọi `sched_block_task`/`sched_wake_task`.
- `sched_new_task_kickoff()` chỉ nhả `sched_lock`; không bật IRQ sớm trước khi `fork_child_exit` restore exception frame và `eret`.

## Lệnh verify nhanh
- make clean all RUN_OS_DEMOS=1
- Mutex test: xem `src/kernel/mutex_test.c` (được verify trên QEMU thành công).

## Pitfall/Debug note
- Khi task không quay lại đúng PC, kiểm tra save/restore ELR/SPSR.
- Khi dead task không bị reap, kiểm tra schedule() đầu vòng.
- Mutex Deadlock: Luôn đảm bảo thứ tự khóa nếu dùng nhiều mutex.
- Với GICv2 SGI, `IAR` chứa source CPU ở bits [12:10]. Chỉ dùng `iar & 0x3ff` để dispatch intid, nhưng phải ghi full `iar` vào EOIR; nếu không timer IRQ có thể bị chặn sau IPI.

## Next steps
- Thêm ưu tiên (priority), deadline, load balancing giữa các core.

---

*Handoff này chỉ tóm tắt trạng thái, invariant, file chính, và trap debug. Khi có thay đổi lớn, cập nhật delta vào đây.*
