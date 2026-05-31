# QEMU Inspector handoff

## Mục tiêu
- Visualize page table, MMU state, physical memory, debug kernel/user mapping qua QEMU HMP monitor.

## Trạng thái hiện tại
- Đã hoàn thành: symbol resolve từ kernel8.elf, đọc physical memory qua HMP, command xp/stop/cont/info status, webview visualizer.
- Đã verify: inspector đọc được page table, dump memory, resolve symbol, visualize mapping.
- Known issues: chưa có QMP/GDB integration, chưa có live update, chưa có multi-core support.

## File chính
- tools/qemu-inspector/server.py
- tools/qemu-inspector/index.html
- tools/qemu-inspector/start.sh
- document/qemu_inspector_hmp_architecture.md

## Invariant/Assumption
- Inspector chỉ dùng HMP, không dùng QMP/GDB.
- Symbol resolve dựa vào aarch64-linux-gnu-nm và kernel8.elf.

## Lệnh verify nhanh
- cd tools/qemu-inspector && ./start.sh
- Truy cập webview, kiểm tra page table/memory mapping.

## Pitfall/Debug note
- Khi không đọc được memory, kiểm tra HMP socket và QEMU args.
- Khi symbol resolve fail, kiểm tra kernel8.elf và nm path.

## Next steps
- Thêm QMP/GDB integration.
- Thêm live update, multi-core support.

---

*Handoff này chỉ tóm tắt trạng thái, invariant, file chính, và trap debug. Khi có thay đổi lớn, cập nhật delta vào đây.*
