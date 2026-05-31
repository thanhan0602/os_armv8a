# MMU handoff

## Mục tiêu
- Quản lý phân trang, bảo vệ vùng nhớ, isolation process, hỗ trợ user/kernel address space.

## Trạng thái hiện tại
- Đã hoàn thành: TTBR0/TTBR1 split, ASID, user page isolation, permission fault, translation fault, heap grow/shrink qua brk, PA→VA migration.
- Đã verify: multi-process isolation, fault classification, ASID tagging, user heap, page free-list giữ PA.
- Known issues: chưa có page sharing, chưa có copy-on-write, chưa có object cache.

## File chính
- src/kernel/mmu.c
- src/include/kernel/mmu.h
- src/include/kernel/vm.h
- src/kernel/page_alloc.c
- src/include/kernel/page_alloc.h
- src/arch/arm/start.S (boot MMU setup)

## Invariant/Assumption
- TTBR1 luôn giữ kernel VA, TTBR0 dùng cho user.
- ASID=0 reserved cho kernel/empty root.
- Không dereference pointer lưu trong .rodata sau khi TTBR0 bị tắt.
- Free-list allocator chỉ lưu PA, không VA.

## Lệnh verify nhanh
- make clean all RUN_OS_DEMOS=1
- QEMU boot, chạy user-a, user-b, kiểm tra fault log đúng loại (permission/translation), heap grow/shrink ok.

## Pitfall/Debug note
- GCC jump-table bug: không dùng const char * table trong .rodata nếu sẽ chạy sau khi TTBR0 bị tắt.
- Khi gặp DABT/IABT recursive crash, kiểm tra pointer trong .rodata có phải PA không.
- Khi context switch, phải save/restore ELR_EL1/SPSR_EL1.
- Khi free page, chỉ lưu PA, không lưu VA.

## Next steps
- Thêm page sharing, object cache, copy-on-write.
- Refactor page table walk cho dễ debug.

---

*Handoff này chỉ tóm tắt trạng thái, invariant, file chính, và trap debug. Khi có thay đổi lớn, cập nhật delta vào đây.*
