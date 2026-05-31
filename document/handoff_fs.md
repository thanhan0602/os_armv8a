# Filesystem handoff

## Mục tiêu
- Quản lý ramfs, file node, loader, hỗ trợ read/write, dynamic node, external app receive.

## Trạng thái hiện tại
- Đã hoàn thành: kernel-only ramfs, dynamic node, fs_register_file, fs_unregister_file, shell command read/write/load/unload.
- Đã verify: read/write file, receive external ELF, load app từ file, unload task.
- Known issues: chưa có persistent fs, chưa có directory, chưa có file permission.

## File chính
- src/kernel/fs.c
- src/include/kernel/fs.h
- src/kernel/loader.c
- src/include/kernel/loader.h

## Invariant/Assumption
- ramfs chỉ tồn tại runtime, không persistent.
- Node động được quản lý qua register/unregister.

## Lệnh verify nhanh
- make clean all RUN_OS_DEMOS=1
- QEMU boot, receive ELF, read/write file, load/unload app.

## Pitfall/Debug note
- Khi file không read/write được, kiểm tra node register/unregister.
- Khi load fail, kiểm tra file node và loader.

## Next steps
- Thêm persistent fs, directory, file permission.

---

*Handoff này chỉ tóm tắt trạng thái, invariant, file chính, và trap debug. Khi có thay đổi lớn, cập nhật delta vào đây.*
