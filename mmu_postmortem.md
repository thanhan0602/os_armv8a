# Hậu kiểm MMU

## Vấn đề

Nhánh MMU nhiều tầng trên ARMv8-A boot được đến trước lúc bật `SCTLR_EL1.M`, sau đó dừng lại ngay sau `isb`. Triệu chứng xuất hiện ở cả nhánh 3-level và 4-level, khiến lỗi ban đầu trông như một lỗi kiến trúc rất sâu, dễ bị nhầm với lỗi QEMU, lỗi `TCR_EL1`, hoặc lỗi quyền thực thi.

## Ảnh hưởng

- Kernel không thể hoàn tất Stage 6 theo nhánh nhiều tầng.
- Không có log `stage 6 mmu enabled`.
- Exception path rơi vào chuỗi `Prefetch Abort` đệ quy.
- Việc mở rộng sang `48-bit VA` bị chặn cho đến khi tìm được nguyên nhân gốc.

## Nguyên nhân gốc

Table descriptor bị encode sai:

```c
#define MMU_DESC_TABLE (1UL << 1)
```

Descriptor này thiếu bit `VALID`. Table descriptor đúng phải là:

```c
#define MMU_DESC_TABLE (MMU_DESC_VALID | (1UL << 1))
```

Hệ quả là translation walk của instruction fetch gặp `translation fault level 0` ngay khi MMU được bật.

## Cách phát hiện

Quy trình debug hiệu quả nhất gồm 4 lớp:

1. Log các mốc trong `mmu_init` để xác định kernel dừng sau `msr sctlr_el1`.
2. GDB single-step qua điểm bật MMU để thấy CPU rơi vào vector sync.
3. QEMU trace `-d int,mmu,guest_errors` để lấy `ESR/FAR/ELR`.
4. So sánh descriptor thật sự trong log, phát hiện giá trị `...002` thay vì `...003`.

Bằng chứng quyết định:

```text
...with ESR 0x21/0x86000004
...with FAR 0x40082460
```

và

```text
[info] l0 root entry=0x...002
```

## Cách sửa

- Sửa macro `MMU_DESC_TABLE` để bao gồm bit `VALID`.
- Verify lại nhánh 4-level tối giản `L0 + L1`.
- Đưa `L2/L3` trở lại cho vùng kernel đầu trên nền `48-bit VA`.

## Kết quả sau khi sửa

- Kernel boot thành công với `48-bit VA`.
- `TTBR0_EL1` trỏ vào `L0` root table.
- Runtime hiện tại dùng hybrid map:
  - `L0 -> L1 -> L2 -> L3` cho vùng kernel đầu.
  - `L2` block cho RAM còn lại.
- Timer IRQ vẫn chạy sau MMU enable.

## Bài học chính

1. Bug MMU cần debug bằng dữ liệu, không bằng suy đoán.
2. `AT S1E1R` hữu ích, nhưng không đủ để chứng minh instruction fetch sẽ thành công.
3. QEMU trace là công cụ quan trọng nhất khi GDB không lộ được syndrome.
4. Một bug 1-bit trong descriptor có thể giả dạng thành lỗi kiến trúc rất sâu.