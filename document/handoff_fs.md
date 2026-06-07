# Filesystem handoff

## Mục tiêu
- Quản lý Virtual File System (VFS) layer để hỗ trợ nhiều filesystem khác nhau (ramfs, devfs, etc.).
- Cung cấp API thống nhất (`fs_open`, `fs_read`, etc.) cho kernel và syscalls.

## Trạng thái hiện tại
- **VFS Layer**: Đã tách biệt thành công layer trung gian tại [src/kernel/fs.c](src/kernel/fs.c). Hỗ trợ đăng ký loại FS (`vfs_register_fs`) và gắn (`vfs_mount`).
- **RamFS Provider**: Logic filesystem RAM gốc được chuyển sang [src/kernel/ramfs.c](src/kernel/ramfs.c) và implement các interface `vfs_ops` / `vnode_ops`.
- **Decoupling (Stage 13)**: `ramfs.c` không còn phụ thuộc cứng vào danh sách user apps. Dùng linker section `.ramfs_builtins` và autogeneration qua `Makefile`.
- **Path Resolution**: Hỗ trợ lookup path dựa trên mount point (longest prefix match).
- **Runtime Ops Fix**: Khắc phục vấn đề absolute pointers trong static structs bằng cách gán địa chỉ hàm tại runtime trong `ramfs_init()` (với `pa_to_va` nếu cần, nhưng gán trực tiếp trong runtime C works vì PC-relative addressing).
- Đã verify: `read`, `load` hoạt động bình thường qua VFS -> RamFS path.

## File chính
- [src/kernel/fs.c](src/kernel/fs.c): VFS core.
- [src/kernel/ramfs.c](src/kernel/ramfs.c): RamFS implementation.
- [src/include/kernel/vfs.h](src/include/kernel/vfs.h): VFS definitions and API.
- [src/include/kernel/fs.h](src/include/kernel/fs.h): Generic file operations.

## Invariant/Assumption
- Mọi filesystem ops struct (`vnode_ops`) phải được gán địa chỉ VA hợp lệ.
- File descriptors (`struct file`) hiện lưu pointer tới `vnode`.
- VNode đơn giản sẽ bị leak nếu không `kfree` trong `fs_close` (hiện tại `fs.c` đã có `kfree(file->vn)`).

## Lệnh verify nhanh
- `make clean all`
- QEMU boot: log hiển thị `vfs: registered filesystem: ramfs` và `vfs: mounted ramfs on /`.
- `read /bin/hello.elf 32`: verify lookup và read xuyên qua VFS.
- `load /bin/hello.elf`: verify loader phối hợp với VFS.

## Pitfall/Debug note
- **Pointer Absolute**: Nếu thêm filesystem mới, hãy đảm bảo các ops struct được khởi tạo tại runtime hoặc dùng địa chỉ VA tuyệt đối nếu đã fix linker script (hiện tại linker script vẫn dùng PA as VMA).
- **VNode Lifetime**: Hiện tại vnode được `kmalloc` lúc lookup và `kfree` lúc close. Chưa có vnode cache.

## Next steps
- Implement DevFS để quản lý thiết bị qua file (e.g., `/dev/tty`, `/dev/fb`).
- Hỗ trợ Directory listing (`readdir`).
- Hỗ trợ `write` thực sự qua VFS ops.
- Fix linker script để VMA là VA thực (tránh pointer hacks).
