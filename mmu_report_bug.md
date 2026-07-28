# Báo cáo điều tra và debug MMU

## Mục tiêu

Tài liệu này tóm tắt quá trình điều tra bug MMU trong kernel ARMv8-A trên QEMU `virt`, tập trung vào:

- Triệu chứng ban đầu
- Các giả thuyết đã được kiểm tra
- Phương pháp debug đã sử dụng
- Dữ liệu quan sát được
- Nguyên nhân gốc
- Trạng thái hệ thống sau khi sửa

## Tóm tắt ngắn gọn

Bug chính không nằm ở QEMU, không nằm ở việc dùng 3-level hay 4-level theo bản thân mô hình dịch địa chỉ, mà nằm ở encoding của table descriptor trong page tables. Các table entry đã được tạo với giá trị `0b10` thay vì `0b11`, nghĩa là có bit `TABLE` nhưng thiếu bit `VALID`.

Hệ quả:

- `AT S1E1R` và `PAR_EL1` có thể gây hiểu nhầm rằng translation có vẻ hợp lệ trong một số giai đoạn thử nghiệm
- nhưng khi bật `SCTLR_EL1.M`, instruction-side fetch gặp `Prefetch Abort`
- emulator báo `translation fault level 0`

Sau khi sửa descriptor về đúng dạng `VALID | TABLE`, nhánh 4-level với `48-bit VA` bắt đầu boot được bình thường, và sau đó nhánh lai được đưa `L2/L3` trở lại cho vùng kernel.

## Triệu chứng ban đầu

Trong quá trình nâng cấp MMU từ nhánh ổn định sang nhánh nhiều tầng hơn:

- kernel boot được đến trước lúc bật MMU
- log in ra đầy đủ các mốc `mmu init start`, `mmu maps built`, `mmu control registers programmed`
- sau lệnh `msr sctlr_el1, ...` và `isb`, hệ thống dừng lại
- không thấy log `stage 6 mmu enabled`

Ở giai đoạn thử 3-level và sau đó là 4-level, hiện tượng lặp lại rất giống nhau:

- translation probe bằng `AT S1E1R` cho các địa chỉ như vector, code, bss, stack trả về kết quả có vẻ hợp lệ
- nhưng instruction stream sau khi bật MMU vẫn bị fault

## Đặt vấn đề và giả thuyết ban đầu

Lúc đầu có nhiều khả năng cần loại trừ:

1. Lỗi do QEMU hoặc do model CPU cụ thể.
2. Lỗi do cấu hình `TCR_EL1`, `TTBR0_EL1`, `MAIR_EL1`, `SCTLR_EL1`.
3. Lỗi do 3-level table sai, cần đổi sang 4-level `48-bit VA`.
4. Lỗi do page permissions, ví dụ `.text` không execute được.
5. Lỗi do exception path sau fault đầu tiên bị đệ quy tiếp.
6. Lỗi do page-table descriptor bị encode sai.

Phương pháp debug đã được xây dựng theo hướng loại trừ từng giả thuyết, thay vì sửa từng chỗ mà không có bằng chứng.

## Sơ đồ luồng debug

```mermaid
flowchart TD
   A[Triệu chứng ban đầu\nDừng ngay sau khi bật MMU] --> B[Thêm log trong mmu_init]
   B --> C[Xác nhận fault xảy ra sau msr sctlr_el1 và isb]
   C --> D[In page-table descriptors]
   D --> E[Thấy entry kết thúc bằng ...002]
   C --> F[Probe AT S1E1R và PAR_EL1]
   F --> G[Translation có vẻ hợp lệ nhưng chưa đủ kết luận]
   C --> H[Objdump và GDB single-step]
   H --> I[CPU rơi vào vector sync thay vì chạy tiếp]
   I --> J[QEMU trace int mmu guest_errors]
   J --> K[Prefetch Abort\ntranslation fault level 0]
   E --> L[Nghi ngờ table descriptor thiếu bit VALID]
   K --> L
   L --> M[Sửa MMU_DESC_TABLE thành VALID | TABLE]
   M --> N[Boot thành công với 4-level 48-bit VA]
   N --> O[Đưa lại L2 L3 cho vùng kernel đầu]
```

## Phương pháp debug đã sử dụng

### 1. In mốc log chi tiết trong `mmu_init`

Mục đích:

- xác định kernel dừng ở giai đoạn nào
- biết chính xác nó đã qua bước lập bảng, nạp thanh ghi điều khiển, hay đã qua bước `SCTLR_EL1.M`

Thực hiện:

- thêm các log như `mmu init start`
- `mmu tables allocated`
- `mmu maps built`
- `mmu control registers programmed`
- log các descriptor quan trọng
- log `sctlr_el1 before` và `sctlr_el1 after`

Kết quả:

- xác nhận được lỗi xảy ra sau khi đã lập xong page tables và đã nạp các control registers
- khoanh vùng fault vào khoảng sau `msr sctlr_el1` và `isb`

### 2. Log trực tiếp các page-table entries

Mục đích:

- kiểm tra descriptor thực tế được ghi vào table
- tránh việc suy đoán sai do code C trong khi giá trị trong RAM lại khác

Thực hiện:

- log `l0 root entry`
- `l1 device entry`
- `l1 ram entry`
- ở các giai đoạn thử nghiệm trước đã log thêm `l2 block0`, `l3 vector`, `l3 sync`, `l3 mmu`, `l3 bss`, `l3 stack`

Kết quả:

- giúp phát hiện sau này rằng giá trị table descriptor thực tế đang kết thúc bằng `...002` thay vì `...003`
- đây là dấu hiệu quan trọng cho thấy bit `VALID` có thể đang bị thiếu

### 3. Dùng `AT S1E1R` và đọc `PAR_EL1`

Mục đích:

- kiểm tra translation bằng phần cứng trước khi bật MMU
- xem các VA quan trọng có được dịch sang PA hợp lệ hay không

Thực hiện:

- thêm helper `mmu_probe_translate()`
- probe các địa chỉ đại diện:
  - vector
  - sync handler
  - code trong `mmu.c`
  - bss
  - stack

Kết quả:

- các probe trả về giá trị có vẻ đúng, ví dụ `0x40080b00`, `0x40081b00`, `0x40082b00`
- điều này chứng minh một phần cấu hình translation có vẻ hợp lệ
- nhưng nó không đủ để kết luận instruction fetch sau khi bật MMU sẽ thành công

Bài học:

- translation probe là công cụ hữu ích, nhưng không thể thay thế cho việc kiểm tra instruction-side fault thật sự

### 4. Dùng `objdump` để đối chiếu địa chỉ fault với lệnh máy

Mục đích:

- xác định chính xác instruction nào đang gây fault
- phân biệt fault tại `isb`, tại load MMIO, hay tại một lệnh gọi hàm tiếp theo

Thực hiện:

- disassemble `mmu_init` bằng `aarch64-linux-gnu-objdump -d build/kernel8.elf`
- đối chiếu các địa chỉ fault trong log/GDB với assembly

Kết quả:

- xác định được một giai đoạn fault gốc xảy ra ngay tại `isb` sau `msr sctlr_el1`
- giúp loại trừ khả năng hệ thống đã chạy tiếp đến vòng chờ UART MMIO sau khi bật MMU

### 5. Dùng GDB attach vào QEMU gdbstub

Mục đích:

- single-step qua điểm bật MMU
- xem PC chạy đi đâu ngay sau `msr sctlr_el1`
- đọc register và đối chiếu với vector table

Thực hiện:

- chạy QEMU với gdbstub trên cổng `1234`
- attach bằng `gdb-multiarch`
- đặt breakpoint tại instruction bật `SCTLR_EL1`
- single-step qua `msr sctlr_el1` và `isb`

Kết quả:

- sau `msr sctlr_el1`, PC nhảy vào vector sync thay vì tiếp tục bình thường
- GDB cho thấy bộ xử lý rơi vào `exception_vector_table`

Bài học:

- single-step qua điểm bật MMU là cách nhanh nhất để biết kernel dừng vì instruction fault, data fault, hay đi vào exception path

### 6. Instrumentation tạm trong exception vector

Mục đích:

- phân biệt fault đầu tiên với fault đệ quy trong exception handler
- xem vector sync có được fetch và chạy đến đâu

Thực hiện:

- chèn breadcrumb raw UART trước và sau `save_context` trong vector sync tương ứng

Kết quả:

- giúp đặt ra giả thuyết về recursive fault trong vector path
- về sau không cần giữ lại instrumentation này sau khi đã tìm được bug descriptor

Bài học:

- khi exception handler phức tạp, nên có một đường debug tối giản để tránh chính handler lại trở thành nguồn fault thứ hai

### 7. Dùng QEMU trace với `-d int,mmu,guest_errors`

Mục đích:

- lấy syndrome mà GDB không hiện rõ ràng
- có dữ liệu độc lập từ emulator về loại exception và địa chỉ fault

Thực hiện:

- chạy QEMU với:

```text
-d int,mmu,guest_errors -D /tmp/qemu-mmu.log
```

- đọc `head` và `tail` của file log để tìm record exception đầu tiên

Kết quả rất quan trọng:

- QEMU báo:

```text
Taking exception 3 [Prefetch Abort] on CPU 0
...with ESR 0x21/0x86000004
...with FAR 0x40082460
...with ELR 0x40082460
...to EL1 PC 0x40080a00
```

Ý nghĩa:

- đây là `Instruction Abort`, cùng mức EL
- fault gốc xảy ra tại địa chỉ instruction `0x40082460`
- vector `0x40080a00` chỉ là điểm nhảy vào exception handler sau đó
- mã `IFSC = 0x4` cho thấy `translation fault level 0`

Đây là bằng chứng mạnh nhất trong cả quá trình debug.

## Các thử nghiệm và kết quả

### Thử nghiệm 1: Đổi từ `-cpu cortex-a53` sang `-cpu max`

Lý do:

- kiểm tra xem bug có phụ thuộc model CPU của QEMU hay không

Kết quả:

- nhánh ổn định vẫn boot
- tần số timer đổi từ `62.5 MHz` sang `1 GHz`
- bug MMU nhiều tầng vẫn lặp lại

Kết luận:

- bug không phải vấn đề đơn giản do model CPU cụ thể

### Thử nghiệm 2: Đổi từ 3-level sang 4-level `48-bit VA`

Lý do:

- nếu `T0SZ=25` tương ứng 39-bit VA vẫn gây lỗi, có thể cần L0 root và 48-bit VA đầy đủ

Kết quả:

- vẫn fault sau khi bật MMU

Kết luận:

- việc có thêm L0 root không tự nó sửa được bug

### Thử nghiệm 3: Nới lỏng permission của page mappings thành `RWX`

Lý do:

- loại trừ khả năng lỗi execute permission hoặc PXN/UXN trên `.text`

Kết quả:

- bug vẫn giữ nguyên

Kết luận:

- lỗi không phải do phân quyền `.text`/`.rodata`/`.data`

### Thử nghiệm 4: Lập trình `SCTLR_EL1` bằng một giá trị explicit sạch

Lý do:

- loại trừ khả năng có các bit reset-state đang tác động đến instruction fetch

Kết quả:

- log cho thấy giá trị `SCTLR_EL1` trước và sau rõ ràng
- bug vẫn tồn tại cho đến khi sửa descriptor

Kết luận:

- control-register state không phải nguyên nhân gốc

### Thử nghiệm 5: Nâng `IPS` trong `TCR_EL1` lên `48-bit`

Lý do:

- kiểm tra tương tác giữa `48-bit VA` và physical address size

Kết quả:

- bug vẫn lặp lại

Kết luận:

- không phải do cấu hình `IPS`

### Thử nghiệm 6: Hạ nhánh về `L0 + L1 block map`

Lý do:

- dùng một cấu hình 4-level tối giản để tách vấn đề `L0 root` ra khỏi vấn đề `L2/L3`

Kết quả:

- vẫn fault khi descriptor của table vẫn sai

Kết luận:

- bug không nằm riêng ở `L2/L3`
- bug nằm ở tầng encoding descriptor / translation walk cơ bản

## Bằng chứng quyết định

Hai dấu hiệu kết hợp lại đã chỉ ra nguyên nhân gốc:

1. QEMU trace báo `translation fault level 0`.
2. Log descriptor cho thấy table entry có dạng `...002` thay vì `...003`.

Với AArch64, table descriptor hợp lệ cần có:

- bit 0 = `VALID`
- bit 1 = `TABLE`

Nghĩa là table descriptor phải có giá trị logic `0b11`.

Trong code lỗi trước đây:

```c
#define MMU_DESC_TABLE (1UL << 1)
```

Sau khi sửa:

```c
#define MMU_DESC_TABLE (MMU_DESC_VALID | (1UL << 1))
```

Đây là sửa đổi gốc rễ.

## Nguyên nhân gốc

Nguyên nhân gốc là page-table table descriptor bị encode sai.

Cụ thể:

- `MMU_DESC_TABLE` thiếu bit `VALID`
- vì vậy các entry trỏ đến bảng cấp tiếp theo không hợp lệ theo phần cứng
- khi bật MMU, instruction fetch gặp `translation fault`
- sau fault đầu tiên, exception vector lại tiếp tục fault, tạo vòng lặp abort

## Cách sửa cuối cùng

### Sửa descriptor

Sửa macro descriptor về đúng dạng:

```c
#define MMU_DESC_VALID (1UL << 0)
#define MMU_DESC_TABLE (MMU_DESC_VALID | (1UL << 1))
#define MMU_DESC_PAGE  MMU_DESC_TABLE
```

### Kích hoạt lại 4-level `48-bit VA`

Sau khi sửa descriptor:

- nhánh 4-level `48-bit VA` boot được
- `TTBR0_EL1` trỏ vào `L0` root table
- timer vẫn phát IRQ bình thường sau MMU enable

### Reintroduce `L2/L3`

Sau khi có nhánh 4-level ổn định, hệ thống được nâng lại theo hướng hybrid:

- `L0 -> L1 -> L2 -> L3` cho vùng kernel đầu
- `L2` block mapping cho phần RAM còn lại

Hiện tại nhánh này vẫn boot ổn định và đã được verify bằng runtime logs.

## Kết quả runtime sau khi sửa

Sau bản sửa cuối cùng, log cho thấy:

- `stage 6 mmu enabled`
- `ttbr0_el1=...`
- `mmu table pages=4`
- `mmu enabled=1`
- `timer tick=1`, `2`, `3`, `4`

Nghĩa là:

- MMU đã được bật thành công
- exception, UART, GIC, timer vẫn tiếp tục hoạt động
- 4-level translation regime đang vận hành trên QEMU `virt`

## Bài học và nguyên tắc debug rút ra

1. Không đoán bug MMU chỉ bằng linh cảm.
   Phải dùng dữ liệu từ log, disassembly, GDB, và emulator trace.

2. Tách bug theo lớp.
   Tách translation probe, instruction fetch, exception path, và MMIO thành từng lớp để debug.

3. Luôn đối chiếu địa chỉ fault với assembly thật.
   Chỉ một địa chỉ như `0x40082460` đã giúp xác định fault xảy ra ngay tại `isb`.

4. QEMU trace là công cụ rất mạnh cho bug MMU.
   Khi GDB không dễ đọc `ESR_EL1/FAR_EL1`, trace của QEMU có thể cho syndrome rõ ràng.

5. Dùng thử nghiệm loại trừ theo bước nhỏ.
   Ví dụ:
   - đổi CPU model
   - đổi 3-level sang 4-level
   - nới lỏng page permissions
   - explicit `SCTLR_EL1`
   - đổi `IPS`

6. Instrumentation tạm là hợp lý nếu nó giúp khoanh vùng bug nhanh hơn.
   Nhưng sau khi tìm được nguyên nhân gốc thì phải bỏ instrumentation debug để trả code về trạng thái sạch.

7. Trong bug MMU, một bug 1-bit trong descriptor có thể giống bug kiến trúc rất sâu.
   Ở đây, một macro sai đã tạo ra cả chuỗi triệu chứng giống như lỗi QEMU, lỗi permission, hoặc lỗi TCR.
    O day, mot macro sai da tao ra ca chuoi trieu chung giong nhu loi QEMU, loi permission, hoac loi TCR.

## Phu luc A: Lenh debug tieu bieu

Day la cac lenh dai dien da duoc dung trong qua trinh dieu tra. Muc dich cua phu luc nay la ghi ro phuong phap debug, khong phai de sao chep nguyen van moi lan.

### 1. Disassemble `mmu_init`

```text
aarch64-linux-gnu-objdump -d build/kernel8.elf | sed -n '/<mmu_init>/,/^$/p'
```

Tac dung:

- doi chieu dia chi fault voi instruction that
- xac dinh fault nam o `isb`, MMIO load, hay call tiep theo

### 2. Liet ke symbol quan trong

```text
aarch64-linux-gnu-nm -n build/kernel8.elf | grep -E 'mmu_init|kernel_main|exception_sync_entry|exception_irq_entry|_start'
```

Tac dung:

- map dia chi assembly sang symbol trong kernel
- xac dinh vector va diem bat MMU

### 3. Attach GDB vao QEMU gdbstub

```text
gdb-multiarch -q build/kernel8.elf \
   -ex 'set pagination off' \
   -ex 'target remote :1234' \
   -ex 'b *0x400824c0' \
   -ex 'c' \
   -ex 'x/8i $pc' \
   -ex 'si' \
   -ex 'x/8i $pc'
```

Tac dung:

- dat breakpoint ngay truoc `msr sctlr_el1`
- single-step qua diem bat MMU
- xem PC co di vao vector sync hay khong

### 4. Chay QEMU voi MMU va exception trace

```text
qemu-system-aarch64 \
   -machine virt,gic-version=2 \
   -cpu max \
   -nographic \
   -serial mon:stdio \
   -kernel build/kernel8.img \
   -d int,mmu,guest_errors \
   -D /tmp/qemu-mmu.log
```

Tac dung:

- lay syndrome khi GDB khong hien ro `ESR_EL1/FAR_EL1`
- tim exception record dau tien truoc khi he thong roi vao de quy fault

### 5. Doc record fault dau tien tu log QEMU

```text
head -n 14 /tmp/qemu-mmu.log
tail -n 200 /tmp/qemu-mmu.log
```

Tac dung:

- tach fault goc khoi chuoi abort de quy
- lay `ESR`, `ELR`, `FAR`, va vector dich den

## Phu luc B: Trich doan log quyet dinh

### 1. Log runtime truoc khi sua descriptor

```text
[info] mmu init start
[info] mmu tables allocated
[info] mmu maps built
[info] l0 root entry=0x0000000047fff002
[info] l1 ram entry=0x0000000047ffd002
[info] mmu control registers programmed
[info] probe vector par=0x0000000040080b00
[info] probe sync par=0x0000000040081b00
[info] probe mmu par=0x0000000040082b00
[info] sctlr_el1 before=0x0000000000c50838
[info] sctlr_el1 after=0x0000000030d00801
```

Dau hieu dang chu y:

- root va RAM table entries ket thuc bang `...002`
- he thong dung ngay sau khi bat MMU

### 2. GDB single-step qua diem bat MMU

```text
Breakpoint 1, 0x00000000400824c0 in mmu_init ()
=> 0x400824c0 <mmu_init+1088>:  msr     sctlr_el1, x0
    0x400824c4 <mmu_init+1092>:  isb
    0x400824c8 <mmu_init+1096>:  mov     x9, #0x9000000

0x00000000400824c4 in mmu_init ()
=> 0x400824c4 <mmu_init+1092>:  Cannot access memory at address 0x400824c4

exception_vector_table () at src/arch/arm/exception_vectors.S
=> 0x40080a00 <exception_vector_table+512>: Cannot access memory at address 0x40080a00
```

Dau hieu dang chu y:

- CPU khong di tiep vao breadcrumb UART sau `isb`
- CPU da roi vao vector sync

### 3. QEMU trace cua fault goc

```text
Taking exception 3 [Prefetch Abort] on CPU 0
...from EL1 to EL1
...with ESR 0x21/0x86000004
...with FAR 0x40082460
...with SPSR 0x600003c5
...with ELR 0x40082460
...to EL1 PC 0x40080a00 PSTATE 0x23c5
```

Dau hieu dang chu y:

- day la instruction abort cung muc EL
- fault goc nam tai instruction sau `msr sctlr_el1`
- `translation fault level 0` huong truc tiep den table descriptor cua root walk

### 4. Log runtime sau khi sua descriptor

```text
[info] l0 root entry=0x0000000047fff003
[info] l1 device entry=0x0060000000000401
[info] l1 ram entry=0x0000000047ffd003
[info] l2 ram[0] entry=0x0000000047ffc003
[info] stage 6 mmu enabled
[info] ttbr0_el1=0x0000000047ffe000
[info] mmu table pages=4
[info] mmu enabled=1
[info] timer tick=1
[info] timer tick=2
```

Dau hieu dang chu y:

- table entries da ket thuc bang `...003`
- MMU da bat thanh cong
- timer IRQ van tiep tuc hoat dong sau MMU enable
   O day, mot macro sai da tao ra ca chuoi trieu chung giong nhu loi QEMU, loi permission, hoac loi TCR.

## Trang thai hien tai

Trang thai hien tai cua he thong:

- dang su dung `48-bit VA`
- co `L0` root table
- co `L2/L3` fine mapping cho vung kernel ban dau
- descriptor da duoc sua dung
- kernel boot on dinh sau MMU enable

## Goi y cho cac bug MMU sau nay

Neu can debug MMU tiep trong tuong lai, nen di theo checklist nay:

1. In log moc trong `mmu_init`
2. In gia tri descriptor that su
3. Probe `AT S1E1R` + `PAR_EL1`
4. Doi chieu dia chi fault bang `objdump`
5. Single-step qua `msr sctlr_el1` bang GDB
6. Bat QEMU trace `-d int,mmu,guest_errors`
7. Nhin vao ma syndrome truoc khi sua code

Day la quy trinh da chung minh hieu qua trong lan dieu tra nay.