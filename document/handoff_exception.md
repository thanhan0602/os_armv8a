# Exception handoff

## Mục tiêu
- Xử lý exception, fault, interrupt, đảm bảo log/debug đầy đủ, phân loại lỗi rõ ràng.

## Trạng thái hiện tại
- Đã hoàn thành: exception vectors, sync fault path dump x0..x30/SP/FPCR/FPSR, phân loại FSC, WnR, log an toàn, save/restore ELR/SPSR.
- Đã verify: permission/translation fault, user/kernel exception, log đầy đủ, không crash recursive.
- Known issues: chưa có nested exception handler, chưa có user-level signal.

## File chính
- src/kernel/exception.c
- src/include/kernel/exception.h
- src/arch/arm/exception_vectors.S

## Invariant/Assumption
- Exception handler luôn save/restore ELR_EL1/SPSR_EL1.
- Log chỉ dùng pattern an toàn, không jump-table PA pointer.

## Lệnh verify nhanh
- make clean all RUN_OS_DEMOS=1
- QEMU boot, gây fault user-a, user-b, kiểm tra log dump đủ register.

## Pitfall/Debug note
- Khi log recursive crash, kiểm tra pointer trong .rodata.
- Khi exception không quay lại đúng PC, kiểm tra save/restore ELR/SPSR.

## Next steps
- Thêm nested exception handler.
- Thêm user-level signal.

---

*Handoff này chỉ tóm tắt trạng thái, invariant, file chính, và trap debug. Khi có thay đổi lớn, cập nhật delta vào đây.*
