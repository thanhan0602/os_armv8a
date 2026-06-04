# OS ARMv8-A Inspector Desktop

Desktop app native cho workflow inspector, chạy local trên Windows và điều khiển máy Linux từ xa qua SSH.

App này không dùng browser UI. Thay vào đó, nó:

- SSH sang máy Linux để build/start/stop inspector
- gọi các API inspector trên remote qua `curl` chạy trong SSH session
- hiển thị kết quả trong Tkinter desktop app local

## Mục tiêu

Giữ phần engine introspection hiện có trên Linux (`tools/qemu-inspector/server.py`), nhưng thay web UI bằng một app riêng biệt cho máy local.

## Tính năng hiện có

- kết nối SSH tới máy Linux chạy QEMU
- build kernel demo bằng `RUN_OS_DEMOS=1`
- start/stop inspector remote
- xem status QEMU / GDB / inspector
- xem tasks
- xem page owners
- break and snapshot theo symbol hoặc VA
- walk VA theo task cụ thể hoặc theo kernel root
- pause / continue VM
- xem register snapshot và raw memory dump

## Cài đặt trên Windows

Yêu cầu:

- Python 3.10+
- Tkinter đi kèm Python
- SSH access tới máy Linux

Trong thư mục này:

```powershell
py -m pip install -e .
py -m os_armv8a_inspector_desktop.app
```

Hoặc dùng entrypoint:

```powershell
os-armv8a-inspector-desktop
```

## Cấu hình kết nối

Trong app, nhập:

- SSH host
- SSH port
- username
- password hoặc private key
- đường dẫn repo remote, ví dụ `/home/a/Learn/os_armv8a`
- đường dẫn `qemu-system-aarch64` nếu muốn override

App lưu lại cấu hình không nhạy cảm trong file JSON local ở home directory của user. Password không được lưu.

## Remote assumptions

App giả định remote Linux đang có repo này với các script hiện có:

- `tools/qemu-inspector/start.sh`
- `build/kernel8.elf`
- `build/kernel8.img`
- `.venv` nếu cần cho inspector backend

## Kiến trúc

Desktop app local không parse trực tiếp ELF hoặc HMP. Nó dùng SSH để điều khiển remote backend hiện có. Điều này giúp giữ logic decode page table và task snapshot ở đúng nơi đang gần target runtime nhất.

Các phần chính:

- `remote.py`: SSH transport + remote command helpers
- `app.py`: Tkinter UI và orchestration

## Hướng mở rộng

- đóng gói thành `.exe` bằng PyInstaller
- chuyển từ `curl over SSH` sang SSH tunnel + direct HTTP client nếu cần tăng tốc
- thay backend ad hoc bằng debug ABI versioned khi kernel export được snapshot ổn định hơn