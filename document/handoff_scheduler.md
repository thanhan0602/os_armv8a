# Scheduler handoff

## Mục tiêu
- Quản lý chuyển đổi context, lập lịch preemptive round-robin, đảm bảo công bằng giữa các task.

## Trạng thái hiện tại
- Đã hoàn thành: round-robin preemptive scheduling qua timer IRQ, context switch lưu full GPR/SIMD, reap dead task, idle task.
- Đã verify: demo task-a, task-b cycling, user-a, user-b chạy/exit sạch, idle giữ boot stack.
- Known issues: chưa có ưu tiên, chưa có deadline, chưa có multi-core.

## File chính
- src/kernel/sched.c
- src/include/kernel/sched.h
- src/arch/arm/switch.S

## Invariant/Assumption
- Context switch luôn save/restore ELR_EL1/SPSR_EL1.
- Idle task id=0, không alloc stack riêng.
- Dead task được reap ở đầu schedule().

## Lệnh verify nhanh
- make clean all RUN_OS_DEMOS=1
- QEMU boot, kiểm tra round-robin, task-a/b cycling, user task exit sạch.

## Pitfall/Debug note
- Khi task không quay lại đúng PC, kiểm tra save/restore ELR/SPSR.
- Khi dead task không bị reap, kiểm tra schedule() đầu vòng.

## Next steps
- Thêm ưu tiên, deadline, multi-core support.

---

*Handoff này chỉ tóm tắt trạng thái, invariant, file chính, và trap debug. Khi có thay đổi lớn, cập nhật delta vào đây.*
