# Dynamic Linker Roadmap & Architecture (ARMv8-A)

Tài liệu này trình bày kiến trúc và lộ trình triển khai Dynamic Linker cho hệ điều hành trên kiến trúc ARMv8-A. Hiện tại, hệ thống đã có một bộ nạp ELF (ELF Loader) cơ bản trong kernel hỗ trợ nạp các file thực thi và thư viện tĩnh. Bước tiếp theo là hiện thực hóa khả năng liên kết động (Dynamic Linking).

## 1. Kiến trúc Dynamic Linker

Dynamic Linker chịu trách nhiệm chuẩn bị một chương trình để thực thi bằng cách nạp các thư viện dùng chung (Shared Libraries) mà nó yêu cầu và giải quyết (resolve) các tham chiếu ký hiệu (symbols) giữa chúng.

### Thành phần chính (ARMv8-A ELF)

- **GOT (Global Offset Table)**: Một bảng chứa các địa chỉ của các biến toàn cục và hàm. Thay vì truy cập trực tiếp bằng địa chỉ tuyệt đối, mã nguồn (PIC - Position Independent Code) sẽ truy cập thông qua các mục trong GOT.
- **PLT (Procedure Linkage Table)**: Một tập hợp các đoạn mã nhỏ (stubs) dùng để gọi các hàm từ thư viện bên ngoài. PLT kết hợp với GOT để thực hiện giải quyết địa chỉ hàm một cách lười biếng (Lazy Binding).
- **Phần DYNAMIC (.dynamic)**: Chứa thông tin cho linker như tên các thư viện cần nạp (`DT_NEEDED`), địa chỉ bảng symbol (`DT_SYMTAB`), địa chỉ bảng chuỗi (`DT_STRTAB`), và các mục relocation (`DT_RELA`).

### Quy trình nạp (Loading Flow)

1. **Kernel Loader**: Đọc header của tiến trình, xác định xem có phần `PT_INTERP` không. Nếu có, kernel nạp cả thực thi chính và Dynamic Linker (`ld.so`).
2. **Linker Startup**: Dynamic Linker tự relocation chính nó (vì nó cũng là một file ELF).
3. **Dependency Mapping**: Linker đọc danh sách `DT_NEEDED`, tìm các file `.so` trong filesystem (thường là `/lib/`) và nạp chúng vào vùng nhớ ảo của tiến trình.
4. **Symbol Resolution**: Linker duyệt qua các mục relocation trong bảng `.rela.dyn` và `.rela.plt` để điền địa chỉ thực tế vào GOT.
5. **Execution**: Linker nhảy đến entry point của chương trình chính.

---

## 2. Roadmap thực hiện từng bước

Chúng ta sẽ thực hiện theo lộ trình từ đơn giản đến phức tạp:

### Bước 1: Hỗ trợ PIC (Position Independent Code) và Shared Objects (.so)
- **Mục tiêu**: Build được file `.so` và file thực thi có thể chạy ở bất kỳ địa chỉ nào.
- **Công việc**:
    - Cập nhật Makefile để dùng flag `-fPIC` và `-shared` khi build thư viện.
    - Đảm bảo Kernel Loader hỗ trợ nạp ELF loại `ET_DYN` (hiện tại đã có hỗ trợ cơ bản trong `process.c`).
    - Thực hiện Relocation loại `R_AARCH64_RELATIVE` đơn giản nhất (chỉ cộng dồn base address).

### Bước 2: Load Dependencies (Recursive Loading)
- **Mục tiêu**: Tự động nạp các thư viện phụ thuộc.
- **Công việc**:
    - Duyệt phần `.dynamic` để tìm các tag `DT_NEEDED`.
    - Tìm kiếm file trong hệ thống tập tin (ví dụ: `/lib/libc.so`).
    - Nạp đệ quy tất cả các thư viện cần thiết vào không gian địa chỉ tiến trình.

### Bước 3: Symbol Resolution (Global Data)
- **Mục tiêu**: Truy cập được biến toàn cục từ thư viện khác.
- **Công việc**:
    - Thực hiện giải quyết relocation loại `R_AARCH64_GLOB_DAT` và `R_AARCH64_ABS64`.
    - Xây dựng bảng tra cứu symbol (Global Symbol Table) từ tất cả các object đã nạp.
    - Đối chiếu tên symbol và ghi địa chỉ vào GOT.

### Bước 4: Chuyển Dynamic Linker ra User-space (ld.so)
- **Mục tiêu**: Giảm tải cho Kernel, tuân thủ thiết kế chuẩn của hệ điều hành hiện đại.
- **Công việc**:
    - Tách logic xử lý ELF từ `src/kernel/process.c` ra một ứng dụng user-space mới mang tên `ld.so`.
    - Kernel chỉ cần nạp `ld.so` và truyền thông tin về app chính qua stack (AUX Vectors).
    - `ld.so` thực hiện nạp thư viện và relocation ở quyền người dùng.

### Bước 5: Lazy Binding (PLT Resolution)
- **Mục tiêu**: Tối ưu hóa tốc độ khởi động bằng cách chỉ resolve hàm khi được gọi lần đầu.
- **Công việc**:
    - Hiện thực `_dl_runtime_resolve` bằng Assembly.
    - Cài đặt địa chỉ của hàm resolver này vào GOT[2].
    - Khi một hàm PLT được gọi lần đầu, nó sẽ nhảy vào resolver để tìm địa chỉ thật và cập nhật GOT.

### Bước 6: API cho lập trình viên (dlopen, dlsym)
- **Mục tiêu**: Cho phép ứng dụng nạp thư viện khi đang chạy.
- **Công việc**:
    - Hiện thực hàm `dlopen()` để nạp thêm `.so`.
    - Hiện thực hàm `dlsym()` để lấy địa chỉ hàm theo tên.

---

## 3. Các loại Relocation AArch64 cần chú ý

Khi implement, bạn cần xử lý các mã relocation sau (định nghĩa trong `elf.h`):

| Mã số | Tên | Công thức xử lý | Chi chú |
| :--- | :--- | :--- | :--- |
| 257 | `R_AARCH64_ABS64` | `S + A` | Ghi địa chỉ tuyệt đối của symbol |
| 1025 | `R_AARCH64_GLOB_DAT` | `S + A` | Dành cho GOT (dữ liệu) |
| 1026 | `R_AARCH64_JUMP_SLOT` | `S + A` | Dành cho PLT (cửa ngõ gọi hàm) |
| 1027 | `R_AARCH64_RELATIVE` | `Delta(B) + A` | Điều chỉnh địa chỉ theo vị trí nạp |

*(S: Symbol value, A: Addend, B: Base address)*

---
**Ghi chú**: Hiện tại `src/kernel/process.c` đã có một phần logic của Bước 1, 2 và 3. Việc di chuyển chúng ra file riêng trong user-space là ưu tiên cao để làm sạch kernel code.
