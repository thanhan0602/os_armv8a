# Handoff

## Subsystem handoff

- [MMU](document/handoff_mmu.md)
- [SMP](document/handoff_smp.md)
- [Heap](document/handoff_heap.md)
- [IPC](document/handoff_ipc.md)
- [Loader](document/handoff_loader.md)
- [Shell](document/handoff_shell.md)
- [QEMU Inspector](document/handoff_inspector.md)
- [Scheduler](document/handoff_scheduler.md)
- [Exception](document/handoff_exception.md)
- [UART](document/handoff_uart.md)
- [Page Allocator](document/handoff_page_alloc.md)
- [Process](document/handoff_process.md)
- [Timer](document/handoff_timer.md)
- [Filesystem](document/handoff_fs.md)
- [Mutex](document/handoff_mutex.md)

## Mục tiêu của repo

Repo này là một kernel ARMv8-A bare-metal chạy trên QEMU `virt`, đang được phát triển theo từng stage từ boot, logging, exception, timer, memory management, đến MMU.

## Trạng thái hiện tại

- Stage 1 đến Stage 9 đã hoàn thành.
- **Stage 9 (EL0 + Syscall ABI)**: user task chạy ở EL0, SVC syscalls hoạt động (SYS_WRITE=1, SYS_YIELD=2, SYS_EXIT=3), user task in "hello from EL0" 3 lần rồi exit sạch sẽ.
- **Stage 10 (User Address Spaces)** — increments 1–4 đã hoàn thành và đã verify trên QEMU:
  - Inc 1: `mmu_install_empty_ttbr0_root()` — owned empty TTBR0 root, boot identity map released
  - Inc 2: EL0 fault handler — DABT/IABT từ EL0 được bắt ở EL1, log + `task_exit()` sạch
  - Inc 3: Multi-process isolation — `user-a` và `user-b` mỗi task có `mm_context` riêng, code+stack được copy vào pages riêng, `mmu_map_user_page()` map vào user L0→L3
  - Inc 4: Fault classification — phân loại FSC type (translation/permission/access-flag) và WnR bit (read/write), dùng `KER_LOGF` + `log_write()` pattern an toàn (không `const char *` jump-table). Kết quả QEMU verify:
    - `user-b` stores to 0x10000 (RO code page) → `write permission L3`: FAR=0x10000
    - `user-a` stores to 0xDEAD0000 (unmapped) → `write translation L1`: FAR=0xdead0000
  - Inc 5: ASID (Address Space Identifier) — mỗi `mm_context` có một 8-bit ASID riêng (1–254), ASID=0 reserved cho kernel/empty root. ASID được encode vào TTBR0_EL1[55:48] khi context switch. Không cần `tlbi vmalle1` (full flush) trên mỗi context switch nữa — ASID tags trong TLB tự partition TLB entries. User page L3 descriptors được force nG=1 (non-global bit) để TLB entries được ASID-tagged. `mmu_map_user_page` dùng `tlbi vae1is` (per-VA per-ASID) thay vì `tlbi vmalle1`.
  - Inc 6: Linux-like syscall/process base — syscall numbers đã chuyển sang gần Linux AArch64 (`write=64`, `exit=93`, `sched_yield=124`, `brk=214`); `sys_write` không còn dereference user VA trực tiếp mà đi qua `mmu_copy_from_user()` dựa trên software walk của `mm_context`.
  - Inc 7: Process image abstraction + user heap — thêm `process_create_from_image()`/`process_destroy()` để đóng gói code image + stack + heap metadata; `process_brk()` hỗ trợ grow và shrink user heap bằng unmap/free page thực sự; `mm_context` hiện track mọi page sở hữu (root/subtables/code/stack/heap) và free sạch khi process bị reap.
  - Inc 8: Hardening pass cho process runtime — user code page được map RO + EL0 execute nhưng `PXN` tại EL1; `sys_write` trả partial byte count nếu fault sau khi đã emit một phần buffer; ASID hiện được recycle an toàn khi `mm_context_destroy()` thay vì tăng đơn điệu suốt lifetime kernel.
- Kernel virtual layout hoàn thành: TTBR1 active, trampoline works, PA→VA migration done.
- Scheduler hoạt động: round-robin preemptive scheduling qua timer IRQ.
- Kernel chỉ chạy tại high VA qua `TTBR1_EL1`; identity map `TTBR0_EL1` chỉ dùng trong giai đoạn boot.
- Build hiện tại thành công qua `make`.
- Runtime hiện tại boot ổn định trên QEMU cho cả hai variant.
- Mặc định build hiện tắt `DEBUG_PRE_MMU`, `DEBUG_MMU_BOOT`, và `DEBUG_POST_MMU` (`0/0/0`), nên boot thành công chỉ còn log `[info] kernel init complete`; có thể override lại qua `make DEBUG_PRE_MMU=1 DEBUG_MMU_BOOT=1 DEBUG_POST_MMU=1` khi cần soi boot path.
- `kernel_main()` hiện không còn tự spawn các task demo (`task-a`, `task-b`, `user-a`, `user-b`) trên đường boot thành công, để console giữ trạng thái quiet boot; code demo vẫn còn trong tree để bật lại khi cần kiểm thử scheduler/EL0.
- Có thêm build flag `RUN_OS_DEMOS=1` để spawn hai user process demo bằng process abstraction mới; vì đây là compile-time flag, cần `make clean all RUN_OS_DEMOS=1` để tránh dùng lại image cũ.
- UART logging hoạt động.
- Driver system layer hiện gom UART, GIC, và timer qua `driver_system_init()`; `driver_system_dump()` báo trạng thái sau khi logger sẵn sàng.
- Exception vectors hoạt động.
- Sync fault path hiện dump đầy đủ `x0..x30`, `SP_EL1`, `FPCR`, và `FPSR`.
- GICv2 và generic timer IRQ hoạt động.
- Physical page allocator hoạt động.
- Kernel heap page-backed hiện hỗ trợ allocation nhỏ và allocation lớn hơn một page thông qua contiguous physical spans.
- Shared debug target framework hiện cũng bao phủ heap arena inspection qua `heap-arenas` và `heap-large-arenas`; heap self-test đã đi qua cùng framework này.
- MMU đã bật ổn định: boot path dùng TTBR0 identity cùng TTBR1 kernel VA.
- Sau trampoline kernel giữ TTBR1 cho runtime và thay TTBR0 identity root bằng một empty lower-half root được kernel sở hữu; low VA kernel aliases fault như mong đợi.
- Đây là increment Stage 10 đầu tiên: runtime đã cắt phụ thuộc vào boot identity map và giữ sẵn lower-half root để đi tiếp tới address spaces riêng.
- Cache `SCTLR_EL1.M/C/I` đã được bật và verify.
- Boot flow: `_start` (PA) → `kernel_main_early` (PA) → trampoline → `kernel_main` (VA qua TTBR1).
- Runtime verify mới nhất với `RUN_OS_DEMOS=1`: `user-a` và `user-b` in hello bằng syscall numbers kiểu Linux; `user-a` grow heap bằng `brk`, in `[user] brk ok`, rồi fault `0xDEAD0000`; `user-b` fault khi ghi vào code page `0x10000`. Cả hai đều bị kill và reap sạch.
- **Stage 11 (IPC And Synchronization)** — increments 1–2 đã chạy được trên QEMU:
  - increment 1: thêm `spinlock` IRQ-safe (`spin_lock_irqsave` / `spin_unlock_irqrestore`) và `wfe`/`sev` wait hints trong `src/kernel/spinlock.c`; logger hiện là consumer đầu tiên nên output kernel đã được serialize qua cùng primitive này
  - increment 2: thêm fixed IPC channels trong `src/kernel/ipc.c` với một-message mailbox (`IPC_CHANNEL_MAX=8`, `IPC_MESSAGE_MAX=64`), `ipc_send()`, `ipc_receive()`, và detach path cho waiter khi task chết
  - scheduler dùng `sched_park_task()` / `sched_unpark_task()` với pending-wake token để task sleep trong `SYS_IPC_RECV` mà không bị lost wakeup khi wake đến trước park
  - IPC và mutex luôn nhả subsystem lock trước khi gọi scheduler; reaper tháo task khỏi run queue dưới `sched_lock` rồi cleanup IPC, mutex, process và MM sau khi nhả khóa
  - remote kill task đang RUNNING chỉ đặt `kill_pending` và gửi IPI; CPU sở hữu task tự chuyển nó sang DEAD trước khi reaper giải phóng tài nguyên
  - mutex pool có `active_ops`/`destroying`, task detach khỏi wait queue khi reap, và `mm_context` có refcount, `dying`, active CPU mask cùng deferred release
  - syscall ABI hiện có thêm `SYS_IPC_SEND=451` và `SYS_IPC_RECV=452`; user runtime expose `user_ipc_send()` / `user_ipc_recv()`
  - built-in demo apps mới: `/bin/ipc_recv.elf` và `/bin/ipc_send.elf`
  - runtime verify mới nhất: `load /bin/ipc_recv.elf`, sau đó `load /bin/ipc_send.elf` in `[ipc-send] sent message on channel 1`, `[ipc-recv] got: hello from ipc_send`, và cả hai task exit sạch
  - limitation hiện tại: channel id là fixed global integer, mailbox chỉ giữ một message tại một thời điểm, `recv` là blocking còn `send` hiện fail nếu mailbox còn đầy; chưa có multi-waiter queue, close semantics, select/poll, hay stream/pipe abstraction
- **Stage 12 (Filesystem And Program Loading)** — increment 1 đã hoàn thành và đã verify trên QEMU:
  - thêm kernel-only read-only `ramfs` trong `src/kernel/fs.c` với hai node `/bin/user-a` và `/bin/user-b`
  - thêm `loader_load_process_image()` trong `src/kernel/loader.c` để nối `file -> loader -> process`
  - thêm `process_create_from_buffer()` để process image không còn phụ thuộc trực tiếp vào symbol linker
  - `kernel_main()` khi build với `RUN_OS_DEMOS=1` hiện spawn demo qua đường file-backed loader thay vì nạp trực tiếp từ symbol range
  - runtime verify mới nhất: path `ramfs -> loader -> process` hoạt động end-to-end; `user-a` và `user-b` lại chạy, `brk` vẫn pass, permission fault tại `0x10000` và translation fault tại `0xDEAD0000` vẫn được phân loại và kill sạch như trước
  - external uploads qua shell `receive` hiện được copy vào buffer do ramfs sở hữu, nên program nạp từ bên ngoài QEMU được lưu độc lập với buffer tạm của shell
  - `kernel_main_early()` hiện nhận và lưu DTB pointer từ boot entry; DTB được validate sớm để mở đường cho driver/device-tree init sau này
- **Stage 13 (Console Shell)** — increment 1 đã hoàn thành và đã verify trên QEMU...
- **VFS (Virtual File System) Implementation** — hoàn thành và verify trên QEMU:
  - Tách core FS thành VFS layer ([src/kernel/fs.c](src/kernel/fs.c)) và RamFS provider ([src/kernel/ramfs.c](src/kernel/ramfs.c)).
  - Thêm `struct vfs_ops` và `struct vnode_ops` để trừu tượng hóa các thao tác trên filesystem.
  - Hỗ trợ `vfs_register_fs` và `vfs_mount` để gắn các filesystem khác nhau vào cây thư mục (mặc định mount `ramfs` vào `/`).
  - Path lookup hỗ trợ tìm kiếm mount point dài nhất (longest matching mount point).
  - Khắc phục lỗi con trỏ hàm tuyệt đối trong static structs khi chạy ở VA (runtime initialization cho ops structs).
  - Shell `read` và `load` đã được chuyển sang dùng VFS API xuyên suốt.
- **VMA = VA Linker Script Transition** — hoàn thành và verify:
  - Linker script ([src/linker.ld](src/linker.ld)) cập nhật link address tại `0xFFFF000040080000`.
  - Dùng `AT()` để giữ LMA tại `0x40080000` cho QEMU loading.
  - Page allocator ([src/kernel/page_alloc.c](src/kernel/page_alloc.c)) đã được cập nhật để dùng `va_to_pa(__kernel_end)` khi khởi tạo pool, tránh conflict với high virtual address symbols.
  - Hệ thống hiện boot sạch sẽ vào shell, hỗ trợ đầy đủ ELF loading và user fault handling.
- **User ELF apps + external loading** — đã hoàn thành và đã verify trên QEMU...
  - thêm cây `user/` với user runtime riêng: `user/common/start.S`, `user/include/user/syscall.h`, `user/linker.ld`, và các app `hello`, `fault`, `ticker`
  - built-in user apps hiện được build thành ELF thật (`/bin/hello.elf`, `/bin/fault.elf`) rồi embed vào kernel image dưới dạng blob, thay cho flat binary cũ
  - kernel loader hiện parse ELF64 AArch64 qua `process_create_from_elf()`; `loader_load_process_image()` dùng chung path này cho cả built-in và external images
  - `ramfs` đã có dynamic nodes qua `fs_register_file()` / `fs_unregister_file()`, và shell có thêm `receive <path> <size>` để nhận hex stream rồi đăng ký file runtime
  - runtime verify mới nhất: `receive /ext/ticker.elf 8496` nhận file ELF ngoài thành công, `read /ext/ticker.elf 32` trả ELF header hợp lệ, `load /ext/ticker.elf ticker` spawn task user chạy `[user-ticker] tick`, và `unload 2` reap task sạch (`ps` chỉ còn idle)
  - user ELF hiện đã chuyển sang `ET_DYN`/PIE thay vì `ET_EXEC`; loader chọn load bias theo từng process trong user image window, nên app không còn phụ thuộc vào VMA link cứng như `0x10000`
  - `process_create_from_elf()` hiện map PT_LOAD tại `load_bias + p_vaddr`, cộng `load_bias` vào `e_entry`, và nếu có `R_AARCH64_RELATIVE` trong `PT_DYNAMIC` thì áp relocation tối thiểu trong kernel trước khi task chạy
- **Stage 14 (Synchronization & Mutex)** — hoàn thành và verify trên QEMU:
  - Thêm `struct mutex` trong [src/kernel/mutex.c](src/kernel/mutex.c) hỗ trợ sleep-based blocking.
  - Hỗ trợ SMP-safe mutex bằng cách dùng spinlock bảo vệ wait-queue nội bộ.
  - Thêm syscall `SYS_MUTEX_LOCK` (500) và `SYS_MUTEX_UNLOCK` (501).
  - Cung cấp `pthread.h` trong [user/include/pthread.h](user/include/pthread.h) với API `pthread_mutex_lock` và `pthread_mutex_unlock`.
  - Verification suite trong [src/kernel/mutex_test.c](src/kernel/mutex_test.c) pass trên cả 4 cores.
  - heap của ELF process không còn cố định ở `0x00040000`; `brk(0)` giờ bắt đầu ngay sau image đã map, có một page guard giữa image và heap
- **Stage 19 (pthread/clone SMP stability)** — hoàn thành và verify trên QEMU:
  - `SYS_CLONE` (224) hiện hỗ trợ `CLONE_VM`, `CLONE_THREAD`, và `CLONE_SETTLS`; user `pthread_create()` dùng trampoline assembly kiểu glibc để child bắt đầu trên stack riêng.
  - `TPIDR_EL0` được lưu/khôi phục trong `switch_context`, cho phép TLS/errno riêng từng thread.
  - GICv2 SGI/IPI fix: IRQ handler mask `IAR[9:0]` để lấy intid nhưng ghi full IAR vào EOIR, tránh SGI từ CPU khác bị coi là spurious và chặn timer IRQ.
  - `process_clone()` chỉ copy saved `exception_context` vào frame sạch trên child kernel stack, không copy toàn bộ live kernel stack của parent.
  - `sched_reap_dead()` chỉ free stack task DEAD khi task đã rời CPU (`TASK_NO_CPU`), tránh free stack đang còn chạy trong `task_exit()`/`schedule()`.
  - Runtime verify mới nhất: `test_pthread.elf` pass 20/20 lần trên QEMU 4-core với idle `wfe` bật lại, không fault/panic/hang.
- **Stage 14 (Lazy Loading & Copy-on-Write)** — hoàn thành và verify trên QEMU:
  - Chuyển đổi từ mô hình nạp code "eager" (copy toàn bộ vào RAM ngay khi nạp) sang "Lazy Loading" (Demand Paging).
  - Thêm `struct vm_region` để quản lý các vùng nhớ ảo (ELF segments, Stack, Heap).
  - Cập nhật Fault Handler để xử lý Translation Fault từ EL0 bằng cách nạp trang từ ELF image hoặc zero-fill cho vùng anonymous khi truy cập lần đầu.
  - Hỗ trợ Copy-on-Write (CoW): Physical pages hiện có reference counting. Khi `fork()` (đã sẵn sàng kiến trúc) hoặc shared pages được truy cập ghi, kernel sẽ nhân bản trang nếu ref_count > 1.
  - Cập nhật software-walk (`mmu_copy_from_user`/`mmu_copy_to_user`) để thủ công kích hoạt fault handler, đảm bảo syscalls hoạt động đúng trên các vùng nhớ chưa được nạp (e.g. string truyền vào syscall).
  - Đã verify thành công với `test_cow.elf` (tính cô lập dữ liệu giữa cha và con qua CoW) và `hello.elf` (lazy load mã nguồn và dữ liệu).
  - runtime verify mới nhất cho PIE path: `hello` được load tại `entry=0x10000 brk=0x14000`, `fault` tại `entry=0x20000 brk=0x24000`, và một instance `hello` khác tại `entry=0x30000 brk=0x34000`; `fault` vẫn tạo permission fault bằng cách ghi vào chính image read-only của nó thay vì hardcode `0x10000`
- **Shared-library groundwork** — đã chạy được end-to-end trên QEMU:
  - build hiện tạo thêm shared object mẫu `build/user/lib/libshared.so` và app phụ thuộc `build/user/external/shared_client.elf`
  - `ramfs` built-in hiện expose `/lib/libshared.so` và `/bin/shared_client.elf`; app external `/ext/shared_client.elf` có thể được upload bằng shell `receive`
  - loader trong `src/kernel/process.c` hiện parse `DT_NEEDED`, `DT_STRTAB`, `DT_SYMTAB`, `DT_STRSZ`, `DT_SYMENT`, `DT_SONAME`, `DT_RELA`, `DT_JMPREL`, `DT_PLTRELSZ`, `DT_PLTREL`
  - relocation runtime hiện hỗ trợ `R_AARCH64_RELATIVE`, `R_AARCH64_JUMP_SLOT`, `R_AARCH64_GLOB_DAT`, và `R_AARCH64_ABS64`
  - runtime verify mới nhất: `load /bin/shared_client.elf` in `[shared-client] hello via shared lib`; external path `receive /ext/shared_client.elf 5904` rồi `load /ext/shared_client.elf` cũng pass với cùng output
  - limitation hiện tại: library vẫn được map private theo từng process; chưa có page sharing giữa process, inode/object cache dùng chung, ABI versioning, hay copy-on-write cho text/data

## Trạng thái MMU hiện tại

- AArch64 EL1
- `48-bit VA`
- `4 KiB` granule
- `4-level translation` rooted at `L0`
- Một kiến trúc high-VA duy nhất:
  - `TTBR0_EL1`: identity map trong boot, sau đó empty lower-half root hoặc page table của user process.
  - `TTBR1_EL1`: kernel VA map (VA == PA + `0xFFFF000000000000`) active sau trampoline.
  - `mmu table pages=6` ở runtime sau handoff (1 empty TTBR0 root + 5 TTBR1 pages).
- Hybrid mapping (cùng cấu trúc cho cả hai bộ bảng trang):
  - `L0 -> L1 -> L2 -> L3` cho tối thiểu `2` chunk đầu, mỗi chunk `2 MiB`
  - tổng vùng fine-grained hiện tại là ít nhất `4 MiB`
  - phần RAM còn lại dùng `L2` block mappings
- Permission model hiện tại:
  - `.text`: `RO + X`
  - `.rodata`: `RO + NX`
  - `.data/.bss/.boot_stack`: `RW + NX`
- PA→VA migration hoàn tất: page_alloc, heap, mmu walks, MMIO drivers (pl011, gicv2) đều conditional convert qua `mmu_is_enabled()`

## Các bài học kỹ thuật quan trọng

- **GCC -O2 PA-pointer jump-table bug (critical for post-TTBR0 code)**: Khi dùng `const char *const arr[]` (pointer array) hoặc gán `const char *p = ...` qua nhiều nhánh if-else, GCC `-O2 -fno-pie` có thể sinh ra một **switch jump table** trong `.rodata` chứa các **link-time PA absolute address** của string literals. Sau khi `mmu_install_empty_ttbr0_root()` thay TTBR0 thành empty root, các PA address này không còn accessible → EL1 DABT → recursive crash. Dù Makefile đã dùng `-fno-jump-tables`, vẫn cần tránh pattern nguy hiểm:
  - **KHÔNG dùng**: `static const char *const names[16]` — GCC tạo PA pointer table trong `.rodata`
  - **KHÔNG dùng**: `const char *p; if (...) p = "a"; else p = "b"; log(p)` — GCC jump-table-ize if-else chain
  - **DÙNG thay thế**:
    - `static const char names[16][32]` — char data inline trong `.rodata`, địa chỉ computed tại runtime qua `adrp` (PC-relative → kernel VA)
    - Direct `log_write("literal")` call tại từng nhánh — string literal address computed qua `adrp` tại call site
    - Ternary `? "a" : "b"` cho 2-way choice — GCC sinh `csel`, không tạo table
  - Root cause: với VMA=LMA=PA (không có PIE relocation), link-time absolute address = PA. Khi kernel chạy qua TTBR1 tại high VA, `adrp` (PC-relative) tự động cho ra kernel VA → works. Nhưng stored absolute values trong `.rodata` vẫn là PA → breaks sau khi TTBR0 empty.
  - Fix đã áp dụng trong `src/kernel/exception.c`: `exception_vector_name()` và fault-type logging đều đã được rewrite theo pattern an toàn trên.

- **ELR_EL1/SPSR_EL1 must be saved in the exception frame**: these are hardware registers that get overwritten by any new exception (e.g. timer IRQ on another task). If not saved in `save_context` and restored in `restore_context`, `eret` after `sys_yield` jumps to the wrong address (another task's interrupted EL1 PC instead of back to EL0 user code). Fix: extend frame CTX_SIZE 784→800, save ELR at offset 248 and SPSR at offset 256; `restore_context` does `msr elr_el1`/`msr spsr_el1` + `isb` before `eret`. This also fixes a latent bug affecting EL1 tasks.
- **SP_EL0 must also survive blocking syscalls**: once `SYS_IPC_RECV` started scheduling user tasks out from inside the syscall path, another latent exception-frame bug surfaced. Preserving only `ELR_EL1`/`SPSR_EL1` was not enough; `SP_EL0` also had to be saved and restored in `exception_vectors.S`. Without that, a task that slept inside a syscall resumed at the right EL0 PC but with the wrong user stack pointer and faulted on local stack accesses after wakeup.
- Một bug MMU lớn trước đây đến từ việc table descriptor thiếu bit `VALID`.
- Table descriptor đúng phải là `VALID | TABLE`.
- `AT S1E1R` và `PAR_EL1` hữu ích nhưng không đủ để thay thế việc kiểm tra instruction fetch thực tế.
- QEMU trace với `-d int,mmu,guest_errors` là công cụ quan trọng khi debug MMU.
- FP/SIMD đã được enable sớm trong boot, nên không còn phụ thuộc vào `-mgeneral-regs-only`.
- Deliberate `brk` đã được verify lại sau khi thêm register dump; fault log hiện có cả `ESR/ELR/SPSR/FAR` lẫn `x0..x30`.
- **VMA=VA pointer-in-data bug**: khi đặt VMA=0xFFFF... trong linker.ld, các `const char *` bên trong `static const struct` arrays (.rodata) chứa VA tuyệt đối. Pre-MMU C code dereference chúng trước khi TTBR1 active → infinite fault loop. Giải pháp: giữ VMA=PA, trampoline dùng `adrp+add+offset` để nhảy sang VA tại runtime.
- **Post-TTBR0 static-table pointer lesson**: vì linker VMA vẫn là PA, các con trỏ string lưu trong `static const` tables tiếp tục là PA. Code chạy sau khi TTBR1 active và TTBR0 bị tắt phải translate các con trỏ đã lưu này (ví dụ `pa_to_va`) trước khi dereference.
- **Post-empty-TTBR0 allocator lesson**: free-list metadata hoặc stored pointers không được ngầm assume lower-half identity còn sống. Page allocator free-list hiện giữ physical addresses để tiếp tục hoạt động sau khi TTBR0 runtime root trở thành empty map.
- **Stage 12 loader lifetime lesson**: nếu metadata file bị reset trong `fs_close()`, loader phải snapshot các field cần dùng trước khi đóng file. Bug vừa gặp đã xuất hiện ở cả `loader_load_process_image()` lẫn helper shared-lib `process_load_file_image()`: nếu so `read_count` với `file.size` sau `fs_close()`, mọi load sẽ fail giả vì `fs_close()` zero lại `size`.
- **Stage 13 shell formatting lesson**: logger hiện chỉ hỗ trợ `%s %c %d %ld %u %lu %x %lx %p %%`; các width specifier kiểu `%08lx` hay `%02x` sẽ bị in literal, nên shell hexdump phải tự format nibble/byte thay vì dựa vào `log_printf()`.
- **User ELF linker lesson**: AArch64 `ld` có thể mặc định dùng `max-page-size = 0x10000`, làm PT_LOAD đầu tiên của user ELF bị đẩy tới offset `0x10000` và phình file lên khoảng `70 KiB` dù payload chỉ vài byte. Build user apps hiện phải force `-Wl,-z,max-page-size=0x1000`; sau fix, `ticker.elf` giảm còn khoảng `8.5 KiB` và external upload qua shell thực tế mới khả thi.
- **User PIE lesson**: chỉ bật `-pie` là chưa đủ nếu linker script vẫn đặt `.` tại `0x10000`; image vẫn có thể bị link như `ET_EXEC`. Để user app thật sự không phụ thuộc load address, linker script phải zero-base (`. = 0`) và loader phải cộng `load_bias` vào `p_vaddr`/`e_entry` khi map.

## Tài liệu nên đọc trước khi tiếp tục

- `roadmap.md`
- `mmu_design.md`
- `heap_design.md`
- `mmu_report_bug.md`
- `mmu_postmortem.md`

## Các file code quan trọng

- `src/kernel/main.c`
- `src/kernel/driver.c`
- `src/kernel/device_tree.c`
- `src/kernel/syscall.c`
- `src/kernel/debug_targets.c`
- `src/kernel/heap.c`
- `src/kernel/mmu.c`
- `src/kernel/page_alloc.c`
- `src/kernel/timer.c`
- `src/kernel/exception.c`
- `src/kernel/sched.c`
- `src/arch/arm/switch.S`
- `src/arch/arm/user_task.S`
- `src/arch/arm/start.S`
- `src/arch/arm/exception_vectors.S`
- `src/linker.ld`

## Trạng thái AI tooling hiện tại

- `.github/copilot-instructions.md` đã define workflow đọc context theo lớp: bắt đầu từ `handoff.md`, rồi chỉ đọc đúng doc/subsystem cần thiết.
- Workspace hiện có custom agent set trong `.github/agents` theo flow `Orchestrator -> Code -> Review`: `Orchestrator` điều phối, `Code` implement và validate, `Review` kiểm tra correctness/regression mà không sửa code.
- Local RAG API hiện nằm ở `tools/rag`, dùng shared SQLite + FAISS/Numpy retrieval tại `.rag-store/index.sqlite3`.
- MCP prototype hiện nằm ở `tools/mcp` với topology `1 + 3`:
  - `gateway`: route query và merge kết quả
  - `design`: doc-first design retrieval
  - `coding`: code-only retrieval, giữ symbol-aware search
  - `document`: document retrieval cho handoff/README/note
- Gateway và specialist MCP đều reuse cùng RAG store, không tạo index riêng.
- Gateway hiện giữ warm in-process specialist adapters để tránh respawn routing logic qua từng request; các specialist MCP server riêng vẫn còn để chạy standalone.
- Tài liệu thiết kế MCP hiện ở `document/mcp_architecture.md`.
- `tools/vscode-qemu-log` hiện có thêm command `qemuInspector.start`: một webview visualizer cho page/MMU state. Inspector resolve symbol từ `build/kernel8.elf` bằng `aarch64-linux-gnu-nm`, rồi đọc physical memory live qua QEMU **HMP monitor socket** (`xp`, `stop`, `cont`, `info status`) thay vì dựa vào UART log hoặc external GDB.
- Hướng QMP/GDB đã được loại trong workspace hiện tại vì `aarch64-linux-gnu-gdb` không có sẵn và QMP greeting không ổn định khi validate; HMP monitor đã được verify runtime thành công.

## QEMU Inspector

Inspector handoff đã được tách riêng khỏi handoff của OS.

- Handoff riêng: `tools/qemu-inspector/handoff.md`
- Tài liệu kiến trúc: `document/qemu_inspector_hmp_architecture.md`

Khi làm việc với inspector, ưu tiên đọc handoff riêng của inspector thay vì dùng phần này.

## Trạng thái Scheduler hiện tại

- Round-robin preemptive scheduling, driven bằng timer IRQ (~500ms/tick).
- `schedule()` gọi từ `exception_handle_irq()` sau khi EOI.
- Context switch lưu callee-saved registers (`x19-x30`, `SP`) — full GPR/SIMD frame đã được save/restore ở exception vectors.
- Task mới nhận 2 contiguous pages: 1 guard page (bottom) + 1 usable stack page (top). Guard page allocated nhưng không unmap được ở hardware level (vì nằm trong L2 block mapping) — chỉ isolation qua allocation.
- Dead task được reap tại đầu `schedule()`: unlink khỏi circular list, free guard+stack pages.
- Idle task (id=0) chạy trên boot stack, không cần alloc riêng.
- Demo tasks (`task-a`, `task-b`) verify round-robin cycling.

## Framework debug hiện tại

Repo hiện đã có một framework `debug target` thống nhất cho log boot-time, nằm chủ yếu ở:

- `src/kernel/debug_targets.c`
- `src/kernel/heap.c`
- `src/kernel/mmu.c`
- `src/kernel/page_alloc.c`

Các target hiện có gồm:

- `managed-head`
- `page-a`
- `page-b`
- `alloc-window`
- `mmu-tables`
- `mmu-walk`
- `mmu-probe`
- `heap-arenas`
- `heap-large-arenas`

Các phase này hiện có thể bật hoặc tắt qua build flags:

- `DEBUG_PRE_MMU`
- `DEBUG_MMU_BOOT`
- `DEBUG_POST_MMU`

Ý nghĩa nhanh:

- `managed-head`: dump vài page đầu của vùng allocator managed
- `page-a`, `page-b`, `alloc-window`: theo dõi vòng đời page vừa `alloc/free`
- `mmu-tables`: liệt kê các page allocator đang bị MMU giữ làm bảng trang, với tên semantic như `l0-root`, `l1-root`, `l2-ram`, `l3-chunk-0`
- `mmu-walk`: software walk qua `L0/L1/L2/L3`
- `mmu-probe`: hardware probe bằng `AT S1E1R` + `PAR_EL1`
- `heap-arenas`: dump mọi heap arena qua shared target framework
- `heap-large-arenas`: chỉ dump các arena có ít nhất `2` page, hữu ích để soi allocation lớn

Heap self-test trong `kernel_main()` hiện gọi `kernel_debug_log_heap_targets()` thay cho ad hoc page-range dump, nên log heap arena và log page allocator dùng cùng naming và flow debug target.

Page allocator hiện cũng có `page_alloc_contiguous/page_free_contiguous` để heap có thể tạo arena lớn hơn một page mà chưa cần heap VA riêng.

Các log consistency quan trọng của page allocator vẫn được in riêng dưới dạng:

```text
[info] page allocator consistency mismatches=0 ...
```

Nếu số `mismatches` khác `0`, nên coi đó là dấu hiệu đầu tiên rằng metadata allocator đã lệch khỏi free list hoặc bookkeeping tổng.

Nếu log MMU cần đọc nhanh, nên nhìn theo thứ tự:

1. `debug target=mmu-tables`
2. `debug target=mmu-walk`
3. `debug target=mmu-probe`
4. `stage 6 mmu enabled`

Nếu log page allocator cần đọc nhanh, nên nhìn theo thứ tự:

1. `debug target=managed-head`
2. `debug target=page-a/page-b/alloc-window`
3. `page allocator consistency mismatches=...`

Nếu log heap cần đọc nhanh, nên nhìn theo thứ tự:

1. `debug target=heap-arenas`
2. `debug target=heap-large-arenas`
3. `heap pages=... used_bytes=... free_bytes=...`

## Lệnh thường dùng

Build:

```text
make
```

Run:

```text
make run
```

Debug QEMU:

```text
bash scripts/run_qemu_debug.sh
```

## Việc đáng cân nhắc tiếp theo

1. **Hoàn thành**: PA→VA migration — tất cả module đã được chuyển.
2. **Hoàn thành**: high-VA kernel qua TTBR1; chế độ identity-only đã được loại bỏ.
3. Dùng empty TTBR0 runtime root hiện có làm base cho lower-half mappings riêng theo process/user.
4. Thêm guard pages cho stack hoặc các vùng nhạy cảm.
5. Tiếp tục các increment Stage 10 tiếp theo cho address spaces riêng.

## Ghi chú quyết định kiến trúc

- TTBR1 kernel VA map luôn active ở runtime. TTBR0 identity map chỉ giữ cho boot path; runtime đổi sang empty lower-half root riêng.
- VMA vẫn là PA trong linker.ld để tránh pointer-in-data bug (xem mục bài học kỹ thuật).
- Trampoline dùng `adrp+add+offset` thay vì `ldr =symbol` vì VMA=PA khiến literal pool chứa PA, cần cộng offset runtime.
- `kernel_main` gồm hai phần: `kernel_main_early` chạy tại PA và `kernel_main` chạy tại high VA.
- `KERNEL_VA_OFFSET` cố định là `0xFFFF000000000000`.

## Mục tiêu khi mở lại phiên sau

Khi quay lại repo này, nên làm theo thứ tự:

1. Đọc `handoff.md` này.
2. Đọc `roadmap.md` để biết stage hiện tại.
3. Đọc `mmu_design.md` nếu làm tiếp phần MMU.
4. Đọc `page_alloc_design.md` nếu đụng đến allocator hoặc debug target framework.
5. Đọc `heap_design.md` nếu làm tiếp Stage 7.
6. Nếu đụng đến bug MMU cũ, đọc `mmu_report_bug.md` trước khi sửa code.
7. Chạy `make run` để xác nhận baseline vẫn ổn trước khi thay đổi gì thêm.

## MCP Session Updates

- Gateway now keeps warm MCP specialist sessions and document specialist exposes append-only write tools.
- Focused MCP verification confirmed warm specialist reuse and append-only document write tools.
- Gateway now keeps warm MCP specialist sessions and document specialist exposes append-only write tools.
- MCP now includes an `ops` specialist plus gateway proxy tools for `build_kernel`, `start_qemu`, `qemu_status`, `read_qemu_log`, and `stop_qemu`; QEMU state/logs live under `.mcp-runtime/`.
- MCP ops now also supports `build_and_run`, `restart_qemu`, and `gdb_attach_info`; an ops demo client is available via `tools/mcp/run_ops_demo.sh`.
- MCP ops now understands the existing VS Code debug setup via `.vscode/launch.json` and `.vscode/tasks.json`, and exposes `vscode_debug_info` plus `prepare_vscode_debug` for the QEMU attach flow.
- MCP ops now exposes `git_push` through both the ops specialist and the gateway; it defaults to `origin`, uses the current branch when possible, rejects missing-upstream and no-op pushes, and never enables force push behavior.
