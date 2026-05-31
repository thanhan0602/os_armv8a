# Heap handoff

## Mục tiêu
- Quản lý cấp phát bộ nhớ động cho kernel và user process, hỗ trợ cả alloc nhỏ và alloc lớn (multi-page).

## Trạng thái hiện tại
- Đã hoàn thành: heap page-backed, hỗ trợ alloc nhỏ/lớn, arena self-test, heap arena debug target.
- Đã verify: alloc nhỏ/lớn, free, heap-arenas, heap-large-arenas qua debug target.
- Known issues: chưa có heap fragmentation mitigation, chưa có GC, chưa có heap profiling.

## File chính
- src/kernel/heap.c
- src/include/kernel/heap.h

## Invariant/Assumption
- Heap chỉ alloc trên page đã được map hợp lệ.
- Không free lại page chưa từng alloc.
- Arena metadata luôn nằm trong vùng kernel VA.

## Lệnh verify nhanh
- make clean all
- QEMU boot, chạy heap-arenas, heap-large-arenas qua debug target.

## Pitfall/Debug note
- Khi gặp alloc fail, kiểm tra page table mapping và arena metadata.
- Khi heap self-test fail, kiểm tra lại free-list và arena init.

## Next steps
- Thêm heap fragmentation mitigation.
- Thêm heap profiling/debug tool.

---

*Handoff này chỉ tóm tắt trạng thái, invariant, file chính, và trap debug. Khi có thay đổi lớn, cập nhật delta vào đây.*
