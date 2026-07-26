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
- IPC và mutex dùng handshake `sched_park_task()`/`sched_unpark_task()` với một-bit wake token. Subsystem luôn nhả lock riêng trước khi gọi scheduler, nên wake xảy ra trước park cũng không bị mất.
- Mọi chuyển đổi task state, `sleep_ticks`, `kill_pending`, `wake_pending` và exit status đều được serialize dưới `sched_lock`.
- Reaper chỉ detach task khỏi run queue và chuyển sang `TASK_STATE_REAPING` khi giữ `sched_lock`; cleanup IPC, mutex, process, MM và stack diễn ra sau khi nhả khóa.
- Remote kill đối với task đang RUNNING chỉ đặt `kill_pending` và gửi IPI. CPU đang sở hữu task tự chuyển nó sang DEAD trong `schedule()` trước khi task được reap.
- `sched_new_task_kickoff()` chỉ nhả `sched_lock`; không bật IRQ sớm trước khi `fork_child_exit` restore exception frame và `eret`.

## Giao thức SMP hiện tại

1. Waiter được publish dưới subsystem lock, sau đó subsystem lock được nhả trước khi gọi `sched_park_task()`.
2. Producer lấy waiter khỏi queue dưới subsystem lock, nhả khóa, rồi gọi `sched_unpark_task()`.
3. Nếu unpark đến trước park, `wake_pending` được ghi dưới `sched_lock`; park kế tiếp tiêu thụ token thay vì block, loại bỏ lost wakeup.
4. Cleanup không giữ `sched_lock` khi lấy IPC, mutex, process, MM hoặc allocator lock.
5. `sched_sleep_current()` là đường duy nhất của nanosleep để cập nhật deadline và state.

## Rủi ro SMP còn lại

- `sched_dump_tasks()` vẫn là debug best-effort vì đọc task/process fields không khóa xuyên suốt snapshot.
- `sched_lock` toàn cục có thể trở thành bottleneck khi tăng số CPU; chưa có per-CPU run queue hay load balancing có affinity.
- Remote kill đã ngăn reaping khi task còn chạy trên CPU khác, nhưng chưa cung cấp API synchronous kill có acknowledgement cho caller.

## Lệnh verify nhanh
- make clean all RUN_OS_DEMOS=1
- Mutex test: xem `src/kernel/mutex_test.c` (được verify trên QEMU thành công).
- SMP regression wake-before-park:
	`make clean all SMP_REGRESSION_TESTS=1 RUN_OS_DEMOS=0`
- Marker thành công: `[stress] wake-before-park PASS`, `[stress] remote-kill PASS`, `[stress] mutex-owner-detach PASS`, `[stress] mutex-waiter-detach PASS` và `[stress] mutex-concurrent-destroy PASS`.

## Pitfall/Debug note
- Khi task không quay lại đúng PC, kiểm tra save/restore ELR/SPSR.
- Khi dead task không bị reap, kiểm tra schedule() đầu vòng.
- Mutex Deadlock: Luôn đảm bảo thứ tự khóa nếu dùng nhiều mutex.
- Với GICv2 SGI, `IAR` chứa source CPU ở bits [12:10]. Chỉ dùng `iar & 0x3ff` để dispatch intid, nhưng phải ghi full `iar` vào EOIR; nếu không timer IRQ có thể bị chặn sau IPI.

## Next steps
- [x] Thêm regression test xác định cho wake-before-park.
- [x] Thêm regression test cho remote kill/reap trên CPU khác.
- [x] Thêm regression test kill mutex owner, handoff ownership và wake waiter.
- [x] Thêm regression test kill mutex waiter và giải phóng operation pin bị bỏ dở.
- [x] Thêm regression test từ chối destroy mutex đang locked/có owner.
- [x] Thêm regression test deferred `mm_context` release khi owner cuối biến mất trong lúc context vẫn active trên CPU khác.
- [x] Tổng hợp sáu regression bằng pass mask có khóa, in `[stress] ALL PASS` và tắt QEMU xác định qua PSCI `SYSTEM_OFF`.
- [x] Thêm stress test 512 vòng lock/trylock/unlock cạnh tranh với destroy; xác minh destroy luôn bị từ chối khi mutex đang được giữ và thành công sau khi mọi operation kết thúc.
- Cân nhắc synchronous remote-stop acknowledgement nếu API kill cần bảo đảm task đã dừng trước khi trả về.
- Sau đó mới thêm priority, deadline, per-CPU run queue và load balancing.

---

*Handoff này chỉ tóm tắt trạng thái, invariant, file chính, và trap debug. Khi có thay đổi lớn, cập nhật delta vào đây.*
