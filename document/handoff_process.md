# Process handoff

## Mục tiêu
- Quản lý process abstraction, tạo/hủy process, quản lý image, stack, heap, ASID, context switch.

## Trạng thái hiện tại
- Đã hoàn thành: process_create_from_image, process_destroy, process_brk, ASID tagging, heap grow/shrink, multi-process isolation.
- Đã hoàn thành (Mới): `process_execve` (syscall 221) hỗ trợ thay thế image tiến trình.
- Đã hoàn thành (pthread/clone): `process_clone()` hỗ trợ `CLONE_THREAD` dùng chung `process/mm`, refcount process, `CLONE_SETTLS` cho `TPIDR_EL0`, và child exception frame sạch tại đỉnh kernel stack.
- Đã thêm user-space `/bin/init.elf`. Kernel tạo và đăng ký init trước các boot service; các service được gán làm child của init để được thu gom bằng `wait4`.
- Khi parent thoát, scheduler reparent các child còn sống sang init. Task không có parent hợp lệ và init khi thoát được chuyển thẳng sang `DEAD` để không tạo zombie không thể thu gom.
- Shell `unload` dùng `sched_kill_task_sync()`, chỉ trả thành công sau khi task không còn chạy trên CPU đích hoặc đã được reap.
- Đã verify: user-a, user-b, shared_client chạy/exit sạch, heap grow/shrink, ASID recycle, `test_exec.elf` gọi thành công `execve`, `test_pthread.elf` hoàn tất lặp lại trên QEMU SMP.
- Runtime init đã verify trên QEMU 4 CPU: init khởi động với PID 4, pthread service hoàn tất `Complex Test Finished.`, và init in `[init] reaped pid=5 status=0`.
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
- `init_task_id` được bảo vệ bởi `sched_lock`. Mọi thay đổi `parent_id`, chuyển `ZOMBIE`/`DEAD`, wake parent và child reparenting đều diễn ra dưới cùng scheduler lock.
- Chỉ task có parent còn sống mới giữ trạng thái `ZOMBIE`. Task parentless được chuyển thẳng sang `DEAD`; reaper chỉ giải phóng stack/process/MM sau khi task đã rời CPU.
- Build SMP regression phải dùng `RUN_OS_DEMOS=0` để dành đủ `MAX_TASKS` slots cho stress suite. Init và boot services chỉ được spawn khi `CONFIG_RUN_OS_DEMOS` được bật.

## Lệnh verify nhanh
- make clean all RUN_OS_DEMOS=1
- QEMU boot, load nhiều user app, kiểm tra isolation, heap grow/shrink.
- Init lifecycle: kiểm tra `[boot] spawned init task`, `[init] started`, service exit và `[init] reaped ...`.
- SMP suite độc lập: `make clean all SMP_REGRESSION_TESTS=1 RUN_OS_DEMOS=0`; kết quả phải có `[stress] remote-stop-ack PASS` và `[stress] ALL PASS` với QEMU status 0.

## Pitfall/Debug note
- Khi process không exit sạch, kiểm tra heap/stack free và ASID recycle.
- Khi context switch lỗi, kiểm tra mm_context và sched.
- Nếu pthread child nhảy vào kernel VA hoặc `restore_context` fault gần cuối RAM, kiểm tra clone frame setup và đảm bảo không bật IRQ trước khi `fork_child_exit` restore xong frame.

## Next steps
- [x] Thêm init task, boot-service adoption và orphan reparenting.
- [x] Không giữ zombie cho task không có parent hợp lệ.
- [x] Thêm synchronous remote-stop acknowledgement cho shell unload.
- [x] Thêm regression chuyên biệt cho parent thoát trước child và init thu gom orphan zombie. Marker: `[stress] init-reap PASS`.
- [ ] Thêm process signal và process group.

---

*Handoff này chỉ tóm tắt trạng thái, invariant, file chính, và trap debug. Khi có thay đổi lớn, cập nhật delta vào đây.*
