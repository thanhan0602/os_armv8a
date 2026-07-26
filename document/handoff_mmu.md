# MMU handoff

## Mục tiêu
- Quản lý phân trang, bảo vệ vùng nhớ, isolation process, hỗ trợ user/kernel address space.

## Trạng thái hiện tại
- Đã hoàn thành: TTBR0/TTBR1 split, ASID, user page isolation, permission fault, translation fault, heap grow/shrink qua brk, PA→VA migration.
- Đã hoàn thành (Advanced): ASID tagging (8-bit), Copy-on-Write (CoW) với reference counting trên physical pages, Lazy Loading (cho stack/heap), SYS_FORK, SYS_WAIT4, SYS_MUNMAP.
- Đã hoàn thành (Refactor): Unified sub-table allocation helpers, fixed descriptor VA-to-PA pointers in `mmu_find_pte`, improved recursive table merging.
- Đã hoàn thành (SMP hardening): per-`mm_context` lock cho page-table mutation, lazy fault, CoW, map/unmap và software walk; ASID allocator lock; allocator/refcount release an toàn; process VM metadata được đồng bộ với page-fault handler.
- Đã hoàn thành (MM lifetime): `mm_context` có owner refcount, trạng thái `dying`, active CPU mask và lifecycle lock. Context chỉ được giải phóng sau khi owner cuối cùng biến mất và không còn CPU nào cài nó trong TTBR0.
- Đã verify (MM deferred release): regression xác định trên QEMU 4 core giữ context active trên một CPU, drop owner cuối từ task điều khiển, xác nhận context chuyển sang `dying` nhưng chưa release, rồi xác nhận release đúng một lần sau khi CPU giữ context chuyển TTBR0 về empty root.
- Đã verify: test_cow.elf (CoW success), fork/wait4 logic, lazy page fault handler, execve memory cleanup.
- Đã verify thêm trên QEMU 4 core: CPU 1-3 online, kernel init hoàn tất, pthread workload in ra `Complex Test Finished.`, không có fatal MMU/allocator marker trong log kiểm thử.

## File chính
- src/kernel/mmu_vmsa.c (Core VMSA logic)
- src/kernel/mmu.c (Generic MMU API)
- src/include/kernel/mmu.h
- src/include/kernel/vm.h
- src/kernel/page_alloc.c
- src/include/kernel/page_alloc.h
- src/arch/arm/start.S (boot MMU setup)

## Invariant/Assumption
- TTBR1 luôn giữ kernel VA, TTBR0 dùng cho user.
- Các descriptor trong Page Table PHẢI chứa Physical Address (PA).
- ASID=0 reserved cho kernel/empty root.
- Physical pages được ref-counted (`page_alloc.c`) để hỗ trợ CoW.
- L3 descriptors của shared CoW pages được đặt `AP=RO` và `nG=1`.
- Thứ tự khóa cho page fault và VM metadata là `process->lock -> mm->lock -> page allocator lock`.
- TCR hiện dùng ASID 8-bit (`TCR_EL1.AS=0`), vì vậy allocator chỉ dùng ASID 1-255; ASID 0 dành cho kernel/empty TTBR0 root.
- `mmu_context_get()` chỉ nhận thêm owner khi context còn sống; `mmu_context_put()` đặt `dying` khi refcount về zero và trì hoãn giải phóng nếu `active_cpu_mask` chưa rỗng.
- `mmu_context_switch()` cập nhật context đang active theo CPU dưới lifecycle lock. CPU cuối cùng tháo một context `dying` khỏi TTBR0 sẽ thực hiện deferred release sau khi đổi TTBR0 và invalidate TLB cục bộ.
- `mmu_context_destroy()` hiện là tên tương thích cho thao tác drop một owner reference, không còn đồng nghĩa với free ngay lập tức.

## Known issues / future SMP work

- Lifetime cơ bản đã an toàn với refcount, `dying` và active CPU mask, nhưng chưa có synchronous cross-CPU detach/shootdown API cho caller cần chờ mọi CPU xác nhận ngay lập tức.
- `mmu_context_switch()` vẫn thực hiện global TLB invalidation sau khi ghi TTBR0. Cách này an toàn nhưng làm mất phần lớn lợi ích ASID và tạo broadcast cost trên mỗi context switch.
- Chưa có ASID generation rollover. Nếu có hơn 255 address space sống đồng thời, cấp ASID sẽ thất bại.
- `CLONE_VM` không kèm `CLONE_THREAD` chưa có semantics chia sẻ MM đúng nghĩa. Nên từ chối tổ hợp chưa hỗ trợ cho đến khi MM lifetime độc lập được hoàn thiện.
- Chưa có object cache (slab/slub).

## Lệnh verify nhanh
- make clean all RUN_OS_DEMOS=1
- QEMU boot, chạy user-a, user-b, kiểm tra fault log đúng loại (permission/translation), heap grow/shrink ok.

## Pitfall/Debug note
- GCC jump-table bug: không dùng const char * table trong .rodata nếu sẽ chạy sau khi TTBR0 bị tắt.
- Khi gặp DABT/IABT recursive crash, kiểm tra pointer trong .rodata có phải PA không.
- Descriptor Bug: Cần cẩn thận khi dùng `new_page` làm con trỏ table nạp vào descriptor; phải dùng `va_to_pa(new_page)`.
- Khi context switch, phải lưu/phục hồi ELR_EL1/SPSR_EL1 và SP_EL0.
- Khi free page, chỉ lưu PA, không lưu VA.

## Next steps
- Thêm stress lặp và cross-CPU shootdown cho trường hợp nhiều CPU đồng thời active cùng một context.
- Bỏ global TLB flush khỏi context-switch fast path và thêm ASID generation rollover sau khi có targeted shootdown đầy đủ.
- Thêm object cache (slab/slub).
- Hỗ trợ Shared Memory (SYS_SHM).
- Refactor page table walk cho dễ debug.

---

*Handoff này chỉ tóm tắt trạng thái, invariant, file chính, và trap debug. Khi có thay đổi lớn, cập nhật delta vào đây.*
