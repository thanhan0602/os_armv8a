# Loader handoff

## Mục tiêu
- Nạp ELF app (built-in/external), parse ELF64, map segment, apply relocation, khởi tạo process image.

## Trạng thái hiện tại
- Đã hoàn thành: loader_load_process_image, process_create_from_elf, hỗ trợ ET_DYN/PIE, relocation R_AARCH64_RELATIVE/JUMP_SLOT/GLOB_DAT/ABS64.
- Đã verify: load /bin/hello.elf, /ext/shared_client.elf, relocation runtime, heap brk sau image.
- Known issues: chưa có page sharing cho shared lib, chưa có ABI versioning, chưa có object cache.

## File chính
- src/kernel/loader.c
- src/include/kernel/loader.h
- src/kernel/process.c
- src/include/kernel/process.h

## Invariant/Assumption
- ELF app phải là ET_DYN/PIE, không còn hardcoded VMA.
- Loader luôn cộng load_bias vào p_vaddr/e_entry.
- Relocation chỉ hỗ trợ một số type tối thiểu.

## Lệnh verify nhanh
- make clean all RUN_OS_DEMOS=1
- QEMU boot, load /bin/hello.elf, /ext/shared_client.elf, kiểm tra output đúng.

## Pitfall/Debug note
- Khi load fail, kiểm tra ELF header, type, và relocation table.
- Khi app chạy sai entry, kiểm tra load_bias và e_entry.

## Next steps
- Thêm page sharing cho shared lib.
- Thêm ABI versioning, object cache.

---

*Handoff này chỉ tóm tắt trạng thái, invariant, file chính, và trap debug. Khi có thay đổi lớn, cập nhật delta vào đây.*
