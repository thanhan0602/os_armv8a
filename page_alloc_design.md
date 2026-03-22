# Thiết kế và debug page allocator

## Mục tiêu

Tài liệu này mô tả page allocator vật lý hiện tại, cùng các helper debug đã được thêm để quan sát trạng thái từng page và kiểm tra tính nhất quán của allocator trong runtime.

## Tổng quan

Page allocator hiện tại là một free-list allocator đơn giản với page kích thước `4 KiB`.

Nguồn page đến từ vùng RAM QEMU `virt` sau `__kernel_end`.

Trạng thái nội bộ được theo dõi bằng:

- `page_free_list`
- `free_pages`
- `total_pages`
- `page_state[]`

Ba trạng thái page hiện tại là:

- `unused`: nằm trong RAM tổng thể nhưng không thuộc vùng managed
- `free`: thuộc vùng managed và đang nằm trong free list
- `allocated`: đã được cấp phát ra ngoài

## Các helper debug hiện tại

Page allocator hiện đã có các helper sau:

- `page_allocator_log_page_state(address)`
- `page_allocator_log_page_range(start, count)`
- `page_allocator_log_managed_head(count)`
- `page_allocator_check_consistency()`
- `page_allocator_log_consistency()`

## Ý nghĩa từng helper

### `page_allocator_log_page_state`

In trạng thái của đúng một địa chỉ page.

Ví dụ log:

```text
[info] page state addr=0x0000000047fff000 index=32767 state=allocated managed=yes
```

Thông tin gồm:

- địa chỉ page
- page index trong RAM QEMU
- state hiện tại
- page đó có nằm trong vùng managed hay không

### `page_allocator_log_page_range`

In liên tiếp nhiều page bắt đầu từ một địa chỉ cho trước.

Helper này hữu ích khi muốn soi một cửa sổ nhỏ quanh các page vừa `alloc` hoặc quanh một vùng nghi ngờ bị hỏng metadata.

### `page_allocator_log_managed_head`

In một đoạn đầu của vùng managed, bắt đầu từ `managed_start`.

Ví dụ log:

```text
[info] page managed head count=4 start=0x0000000040092000
```

Sau đó từng page đầu vùng managed sẽ được in ra để dễ đối chiếu với `__kernel_end` và layout boot-time.

### `page_allocator_check_consistency`

Trả về số lượng mismatch giữa các nguồn sự thật nội bộ của allocator.

Hiện tại helper này kiểm tra:

- số page `free` trong `page_state[]`
- số page `allocated` trong `page_state[]`
- `free_pages`
- `total_pages`
- số node thật sự trong `page_free_list`

Nếu một trong các quan hệ này lệch nhau, helper sẽ trả về giá trị lớn hơn `0`.

### `page_allocator_log_consistency`

In toàn bộ snapshot consistency ra log.

Ví dụ:

```text
[info] page allocator consistency mismatches=0 free_list_nodes=32622 state_free=32622 state_allocated=0 tracked_free_pages=32622 total_pages=32622
```

Đây là dòng nên nhìn đầu tiên khi nghi ngờ allocator bị hỏng trạng thái nội bộ.

## Tích hợp với debug target framework

Debug page allocator hiện không còn được gọi rải rác trực tiếp từ `kernel_main`.

Thay vào đó, module `debug_targets` cung cấp một framework target thống nhất cho:

- `managed-head`
- `page-a`
- `page-b`
- `alloc-window`
- `mmu-tables`
- `mmu-walk`
- `mmu-probe`

Điều này có nghĩa là page allocator và MMU hiện chia sẻ cùng một cơ chế điều phối debug boot-time.

## Cách đọc log boot-time

Trước MMU:

- `managed-head` cho biết đầu vùng managed đang ra sao
- `page-a`, `page-b`, `alloc-window` cho biết các page vừa `alloc` có chuyển sang `allocated` đúng không
- `page allocator consistency` cho biết allocator có tự mâu thuẫn hay không

Sau MMU:

- `mmu-tables` cho biết các page allocator nào đã bị giữ lại cho page tables
- `mmu-walk` cho biết software walk của một địa chỉ quan trọng
- `mmu-probe` cho biết `PAR_EL1` của cùng địa chỉ đó

## Kết quả runtime hiện tại

Runtime gần nhất đã xác minh:

- `managed-head` ra đúng các page đầu vùng managed
- `page-a` và `page-b` đổi trạng thái `free -> allocated -> free`
- sau `mmu_init()`, các page table như `l0-root`, `l1-root`, `l2-ram`, `l3-chunk-0`, `l3-chunk-1` được allocator track là `allocated`
- `page_allocator consistency mismatches=0` ở các mốc đã test

## Hướng mở rộng hợp lý

Các bước tiếp theo có giá trị thực tế:

1. thêm target kind cho dump nhiều cửa sổ page allocator khác nhau theo cấu hình
2. thêm kiểm tra chéo để phát hiện page được đánh dấu `free` nhưng không có trong free list
3. thêm guard metadata hoặc poison pattern cho page vừa free để dễ bắt use-after-free sớm hơn