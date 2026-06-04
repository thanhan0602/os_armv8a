# QEMU Inspector Handoff

## Mục tiêu

`tools/qemu-inspector` là lớp quan sát runtime cho QEMU ARMv8-A trong repo này. Nó phục vụ hai nhu cầu chính:

- đọc live memory/page-table state qua HMP
- phối hợp với GDB RSP khi cần breakpoint, interrupt, hoặc register access

## Trạng thái hiện tại

- FastAPI server nằm ở `tools/qemu-inspector/server.py`, port mặc định `8888`.
- Start qua `bash tools/qemu-inspector/start.sh`.
- Kết nối HMP tại port `4445` và GDB RSP tại port `4446`.
- Có thêm endpoint HMP-only `/api/hmp/snapshot`: dùng khi debugger ngoài (ví dụ VS Code GDB) đã dừng VM sẵn. Endpoint này chỉ đọc `tasks + page_owners` qua HMP và **không** tự `cont`.
- `start.sh` dùng `-S`, nên QEMU khởi động paused. Điều này cần thiết để `break_and_snapshot` còn cơ hội đặt breakpoint trước khi user tasks thoát.

## API và invariant quan trọng

### `/api/gdb/break_and_snapshot`

Flow hiện tại:

1. đặt Z0 breakpoint bằng GDB RSP
2. resume VM
3. chờ stop-reply `T05`
4. đọc task state + page owners qua HMP
5. xóa breakpoint

Lưu ý quan trọng:

- Z0 phải dùng **kernel VA** (`PA + KERNEL_VA_OFFSET`), không phải PA.
- Không force-reconnect GDB vì disconnect có thể làm QEMU auto-`vm_start()`.
- Thay vào đó dùng `flush_and_sync()` để gửi Ctrl-C và drain buffer.

### RUN_STATE_DEBUG (`"running (debug)"`) — invariant sống còn

QEMU report `RUN_STATE_DEBUG` qua HMP `info status` thành chuỗi `"running (debug)"`.

Nếu chỉ check `"running" in status`, các endpoint HMP read có thể gọi `cont` nhầm và làm VM resume khỏi breakpoint GDB.

Pattern đúng hiện tại là:

```python
status_str = (await hmp.command("info status")).lower()
was_freely_running = ("running" in status_str and "debug" not in status_str)
```

Pattern này đã được áp dụng cho các endpoint đọc dữ liệu có stop/read/cont:

- `/api/tasks`
- `/api/snapshot`
- `/api/page_owners`
- `/api/walk`
- `/api/walk_task`
- `/api/debug/rawmem`

`/api/status` cũng đã map đúng `RUN_STATE_DEBUG` thành `"halted (gdb)"`.

## HMP-first, GDB-optional

Hướng kiến trúc hiện tại là:

- `HMP` là data plane để đọc snapshot
- `GDB` là control plane cho breakpoint, continue, interrupt, registers

Điều này cho phép hai workflow:

### Workflow A: inspector tự điều khiển breakpoint

- dùng `/api/gdb/break_and_snapshot`
- phù hợp cho demo nhanh

### Workflow B: debugger ngoài sở hữu breakpoint

- VS Code GDB dừng VM trước
- inspector chỉ dùng `/api/hmp/snapshot`
- user continue từ debugger ngoài

Workflow B là hướng phù hợp hơn nếu muốn mở rộng inspector thành công cụ quan sát hệ thống thay vì chỉ là MMU viewer.

## Verification đã làm

### VA walk end-to-end

Đã verify thành công khi bắt user tasks còn sống tại `schedule` breakpoint:

- `user-a` VA `0x10000` -> PA `0x400a0000`, `ro/x`
- `user-a` VA `0x1f000` -> PA `0x400a1000`, `rw/nx`
- `user-b` VA `0x10000` -> PA `0x400a8000`, `ro/x`
- `user-b` VA `0x1f000` -> PA `0x400a9000`, `rw/nx`

VM vẫn giữ trạng thái `halted (gdb)` trong toàn bộ flow walk.

### HMP-only snapshot

`/api/hmp/snapshot` đã verify trả đúng:

- `tasks[]`
- `ttbr0_root_pa` của user tasks
- `page_owners[]`

trong khi VM đã bị debugger dừng sẵn tại breakpoint.

## Browser và môi trường

- Web UI ở `http://127.0.0.1:8888/`.
- Trong VS Code Server, cần forward port `8888` để mở bằng browser local.
- VS Code Copilot Playwright browser tool bị `net::ERR_ABORTED` với localhost trong môi trường này, nên verification chính vẫn nên qua `curl`.
- Có thêm app desktop riêng ở `tools/inspector-desktop`: app local chạy trên Windows, SSH sang Linux để build/start/stop inspector và gọi remote API headless bằng `curl over SSH`. Đây là hướng thay web UI cho workflow local-remote.

## Tài liệu liên quan

- kiến trúc mở rộng: `document/qemu_inspector_hmp_architecture.md`
- implementation: `tools/qemu-inspector/server.py`
- launcher: `tools/qemu-inspector/start.sh`
- web UI: `tools/qemu-inspector/index.html`

## Gợi ý cho session sau

- nếu làm việc với inspector, đọc file này trước thay vì `handoff.md`
- nếu debug bằng VS Code GDB, ưu tiên workflow HMP-only
- nếu mở rộng sang process/system observability, thiết kế một debug ABI versioned từ kernel thay vì tiếp tục parse struct private ad hoc