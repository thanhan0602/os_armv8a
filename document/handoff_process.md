# Process handoff

## Mục tiêu
- Quản lý process abstraction, tạo/hủy process, quản lý image, stack, heap, ASID, context switch.

## Trạng thái hiện tại
- Đã hoàn thành: process_create_from_image, process_destroy, process_brk, ASID tagging, heap grow/shrink, multi-process isolation.
- Đã hoàn thành (Mới): `process_execve` (syscall 221) hỗ trợ thay thế image tiến trình.
- Đã verify: user-a, user-b, shared_client chạy/exit sạch, heap grow/shrink, ASID recycle, `test_exec.elf` gọi thành công `execve`.
- Known issues: chưa có process signal, chưa có process group, chưa có fork mới (đã có fork nhưng cần test thêm sau execve).

## File chính
- src/kernel/process.c
- src/include/kernel/process.h
- src/kernel/sched.c
- src/include/kernel/sched.h

## Invariant/Assumption
- Mỗi process có mm_context, heap, stack riêng.
- Khi gọi `execve`, con trỏ `task->process` và `task->mm` phải được cập nhật đồng thời để tránh crash trong fault handler.
- ASID được recycle an toàn khi destroy.

## Lệnh verify nhanh
- make clean all RUN_OS_DEMOS=1
- QEMU boot, load nhiều user app, kiểm tra isolation, heap grow/shrink.

## Pitfall/Debug note
- Khi process không exit sạch, kiểm tra heap/stack free và ASID recycle.
- Khi context switch lỗi, kiểm tra mm_context và sched.

## Next steps
- Thêm process signal, group, fork/exec.

---

*Handoff này chỉ tóm tắt trạng thái, invariant, file chính, và trap debug. Khi có thay đổi lớn, cập nhật delta vào đây.*
