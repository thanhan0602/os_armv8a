# Page Allocator handoff

## Mục tiêu
- Quản lý cấp phát/trả lại page vật lý, hỗ trợ heap, process, mmu, user.

## Trạng thái hiện tại
- Đã hoàn thành: free-list allocator, alloc/free page, alloc span nhiều page, debug target page-a/page-b/alloc-window.
- Đã verify: alloc/free page, heap, mmu, process đều dùng chung allocator.
- Known issues: chưa có buddy system, chưa có reclaim, chưa có page coloring.

## File chính
- src/kernel/page_alloc.c
- src/include/kernel/page_alloc.h

## Invariant/Assumption
- Free-list chỉ lưu PA, không VA.
- Không alloc page chưa free, không double free.

## Lệnh verify nhanh
- make clean all
- QEMU boot, chạy debug target page-a/page-b/alloc-window.

## Pitfall/Debug note
- Khi alloc fail, kiểm tra free-list và PA/VA mapping.
- Khi double free, kiểm tra logic free-list.

## Next steps
- Thêm buddy system, reclaim, page coloring.

---

*Handoff này chỉ tóm tắt trạng thái, invariant, file chính, và trap debug. Khi có thay đổi lớn, cập nhật delta vào đây.*
