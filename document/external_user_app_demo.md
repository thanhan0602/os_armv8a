# Demo external user app load

Tài liệu này mô tả cách demo app ELF được nạp từ bên ngoài vào shell runtime, dùng `shared_client.elf` làm ví dụ. App này phụ thuộc vào built-in shared library `/lib/libshared.so`.

## Chuẩn bị

Build toàn bộ kernel, user app, và shared library:

```bash
make -C /home/a/Learn/os_armv8a all
```

Artifact dùng cho demo:

- external app: `build/user/external/shared_client.elf`
- builtin dependency: `/lib/libshared.so` đã được embed trong kernel image

Kích thước hiện tại của `shared_client.elf` là `5904` bytes, nên shell receive command dùng size này.

## Cách 1: demo thủ công qua stdio QEMU

Khởi động QEMU bằng script mặc định:

```bash
cd /home/a/Learn/os_armv8a
QEMU_BIN=qemu-system-aarch64 bash scripts/run_qemu.sh
```

Ở shell `os>` của kernel, nhập:

```text
receive /ext/shared_client.elf 5904
```

Ở terminal host khác, in hex stream của ELF:

```bash
cd /home/a/Learn/os_armv8a
xxd -p -c 512 build/user/external/shared_client.elf
```

Copy toàn bộ hex stream đó và paste vào cửa sổ serial QEMU. Sau khi kernel báo đã nhận file, chạy:

```text
load /ext/shared_client.elf
```

Output mong đợi:

```text
[shell] received /ext/shared_client.elf size=5904
[shell] loaded shared_client.elf as task id=... entry=... brk=...
[shared-client] hello via shared lib
[syscall] user task exited code=0
```

## Cách 2: demo script hóa qua serial TCP

Nếu muốn demo lặp lại nhanh mà không copy-paste tay, chạy QEMU với serial TCP trên một cổng rảnh, ví dụ `5560`:

```bash
cd /home/a/Learn/os_armv8a
SERIAL_PORT=5560 make run-serial-tcp
```

Lệnh này đi qua [scripts/run_qemu_serial_tcp.sh](../scripts/run_qemu_serial_tcp.sh), có preflight báo lỗi rõ nếu cổng đã bị chiếm.

Ở terminal host khác, đẩy app vào shell rồi load ngay:

```bash
cd /home/a/Learn/os_armv8a
exec 3<>/dev/tcp/127.0.0.1/5560
{
  printf 'receive /ext/shared_client.elf 5904\n'
  xxd -p -c 512 build/user/external/shared_client.elf
  printf '\nload /ext/shared_client.elf\n'
} >&3
timeout 8 cat <&3
```

Output mong đợi sẽ chứa các dòng chính sau:

```text
[shell] receiving /ext/shared_client.elf size=5904 as hex stream
[shell] received /ext/shared_client.elf size=5904
[shell] loaded shared_client.elf as task id=... entry=... brk=...
[shared-client] hello via shared lib
```

## Shortcut script

Repo hiện có helper script [scripts/send_external_user_app.sh](../scripts/send_external_user_app.sh) để tránh dùng pattern one-shot kiểu `... | nc ...`, vì cách đó thường đóng socket quá sớm và làm mất output sau lệnh `load`.

Ví dụ dùng với cổng `5560`:

```bash
cd /home/a/Learn/os_armv8a
HOST=127.0.0.1 PORT=5560 scripts/send_external_user_app.sh \
  /ext/shared_client.elf \
  build/user/external/shared_client.elf
```

Script sẽ tự:

- tính kích thước ELF
- gửi `receive <path> <size>`
- stream ELF dưới dạng hex
- gửi `load <path>`
- giữ socket mở thêm một khoảng ngắn để đọc output từ shell

Có thể tăng thời gian đọc output bằng `READ_TIMEOUT`, ví dụ:

```bash
cd /home/a/Learn/os_armv8a
HOST=127.0.0.1 PORT=5560 READ_TIMEOUT=12 scripts/send_external_user_app.sh \
  /ext/shared_client.elf \
  build/user/external/shared_client.elf
```

## Ghi chú hiện tại

- `shared_client.elf` không còn phụ thuộc vào fixed load address; app được build kiểu `ET_DYN`/PIE.
- Shared library support hiện mới ở mức load dependency + apply relocation trong từng process.
- `/ext/shared_client.elf` vẫn phụ thuộc vào việc kernel image hiện tại đã embed `/lib/libshared.so`.