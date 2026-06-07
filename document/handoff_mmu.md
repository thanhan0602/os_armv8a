# MMU handoff

## Mục tiêu
- Quản lý phân trang, bảo vệ vùng nhớ, isolation process, hỗ trợ user/kernel address space.

## Trạng thái hiện tại
- Đã hoàn thành: TTBR0/TTBR1 split, ASID, user page isolation, permission fault, translation fault, heap grow/shrink qua brk, PA→VA migration.
- Đã hoàn thành (Advanced): ASID tagging (8-bit), Copy-on-Write (CoW) với reference counting trên physical pages, Lazy Loading (cho stack/heap), SYS_FORK, SYS_WAIT4, SYS_MUNMAP.
- Đã verify: test_cow.elf (CoW success), fork/wait4 logic, lazy page fault handler.
- Known issues: chưa có object cache (slab/slub).

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
- Physical pages được ref-counted (`page_alloc.c`) để hỗ trợ CoW.
- L3 descriptors của shared CoW pages được đặt `AP=RO` và `nG=1`.

## Lệnh verify nhanh
- make clean all RUN_OS_DEMOS=1
- QEMU boot, chạy user-a, user-b, kiểm tra fault log đúng loại (permission/translation), heap grow/shrink ok.

## Pitfall/Debug note
- GCC jump-table bug: không dùng const char * table trong .rodata nếu sẽ chạy sau khi TTBR0 bị tắt.
- Khi gặp DABT/IABT recursive crash, kiểm tra pointer trong .rodata có phải PA không.
- Khi context switch, phải save/restore ELR_EL1/SPSR_EL1.
- Khi free page, chỉ lưu PA, không lưu VA.

## Next steps
- Thêm object cache (slab/slub).
- Hỗ trợ Shared Memory (SYS_SHM).
- Refactor page table walk cho dễ debug.

---

*Handoff này chỉ tóm tắt trạng thái, invariant, file chính, và trap debug. Khi có thay đổi lớn, cập nhật delta vào đây.*
