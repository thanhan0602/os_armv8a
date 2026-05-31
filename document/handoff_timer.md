# Timer handoff

## Mục tiêu
- Quản lý timer IRQ, tick scheduling, hỗ trợ preemptive scheduling, timeout, sleep.

## Trạng thái hiện tại
- Đã hoàn thành: generic timer IRQ, schedule() qua timer, tick ~500ms, hỗ trợ sleep/yield.
- Đã verify: round-robin scheduling, tick đúng, sleep/yield hoạt động.
- Known issues: chưa có high-res timer, chưa có user timer, chưa có profiling.

## File chính
- src/kernel/timer.c
- src/include/kernel/timer.h

## Invariant/Assumption
- Timer IRQ luôn gọi schedule().
- Tick interval cố định ~500ms.

## Lệnh verify nhanh
- make clean all RUN_OS_DEMOS=1
- QEMU boot, kiểm tra tick, sleep/yield.

## Pitfall/Debug note
- Khi tick không đều, kiểm tra timer IRQ và schedule().
- Khi sleep/yield fail, kiểm tra timer và sched.

## Next steps
- Thêm high-res timer, user timer, profiling.

---

*Handoff này chỉ tóm tắt trạng thái, invariant, file chính, và trap debug. Khi có thay đổi lớn, cập nhật delta vào đây.*
