# Process handoff

## Mục tiêu
- Quản lý process abstraction, tạo/hủy process, quản lý image, stack, heap, ASID, context switch.

## Trạng thái hiện tại
- Đã hoàn thành: process_create_from_image, process_destroy, process_brk, ASID tagging, heap grow/shrink, multi-process isolation.
- Đã hoàn thành (Mới): `process_execve` (syscall 221) hỗ trợ thay thế image tiến trình.
- Đã hoàn thành (pthread/clone): `process_clone()` hỗ trợ `CLONE_THREAD` dùng chung `process/mm`, refcount process, `CLONE_SETTLS` cho `TPIDR_EL0`, và child exception frame sạch tại đỉnh kernel stack.
- Đã verify: user-a, user-b, shared_client chạy/exit sạch, heap grow/shrink, ASID recycle, `test_exec.elf` gọi thành công `execve`, `test_pthread.elf` hoàn tất lặp lại trên QEMU SMP.
- Known issues: chưa có process signal, chưa có process group.

## File chính
- src/kernel/process.c
- src/include/kernel/process.h
- src/kernel/sched.c
- src/include/kernel/sched.h

## Invariant/Assumption
- Mỗi process có mm_context, heap, stack riêng.
- Thread tạo bằng `CLONE_THREAD` chia sẻ cùng `struct process`/`mm_context`; `process_destroy()` phải tôn trọng `ref_count` và chỉ free mm khi reference cuối cùng biến mất.
- Clone child không copy toàn bộ live kernel stack của parent. Chỉ copy `EXCEPTION_CONTEXT_SIZE` bytes của saved exception frame vào child stack, set `context.sp` tới frame đó, rồi resume qua `fork_child_exit`.
- Khi gọi `execve`, con trỏ `task->process` và `task->mm` phải được cập nhật đồng thời để tránh crash trong fault handler.
- ASID được recycle an toàn khi destroy.

## Lệnh verify nhanh
- make clean all RUN_OS_DEMOS=1
- QEMU boot, load nhiều user app, kiểm tra isolation, heap grow/shrink.

## Pitfall/Debug note
- Khi process không exit sạch, kiểm tra heap/stack free và ASID recycle.
- Khi context switch lỗi, kiểm tra mm_context và sched.
- Nếu pthread child nhảy vào kernel VA hoặc `restore_context` fault gần cuối RAM, kiểm tra clone frame setup và đảm bảo không bật IRQ trước khi `fork_child_exit` restore xong frame.

## Next steps
- Thêm process signal, group, fork/exec.

---

*Handoff này chỉ tóm tắt trạng thái, invariant, file chính, và trap debug. Khi có thay đổi lớn, cập nhật delta vào đây.*
