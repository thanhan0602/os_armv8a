# Thiết kế kernel heap

## Mục tiêu

Tài liệu này mô tả baseline Stage 7 hiện tại: một kernel heap nhỏ, đặt trên page allocator vật lý hiện có, đủ để cấp phát các cấu trúc động nhỏ trong kernel.

## Tổng quan

Kernel heap hiện được khởi tạo sau `mmu_init()`.

Heap dùng page allocator làm backend. Với allocation nhỏ, heap vẫn dùng arena một page như trước. Với allocation lớn hơn sức chứa của một page, heap xin một span page vật lý liên tiếp rồi dùng toàn bộ span đó làm một arena lớn hơn.

Thiết kế hiện tại ưu tiên:

- đơn giản khi bring-up
- dễ debug
- không phụ thuộc vào virtual heap layout riêng

## Cấu trúc nội bộ

Mỗi heap arena có:

- `struct kernel_heap_page` ở đầu page
- một `struct kernel_heap_block` đầu tiên mô tả phần free còn lại trong page
- các block tiếp theo được split dần khi có allocation

Arena có thể rộng `1` page hoặc nhiều page liên tiếp, nhưng logic block bên trong vẫn giống nhau.

Mỗi `kernel_heap_block` có:

- `size`
- `is_free`
- `next`
- `prev`

Các allocation được căn chỉnh theo `16` byte.

## Luồng cấp phát

`kmalloc(size)` hoạt động như sau:

1. căn chỉnh `size`
2. duyệt các heap page hiện có để tìm free block đủ lớn
3. nếu block lớn hơn cần thiết, split block
4. đánh dấu block là allocated và trả về địa chỉ ngay sau header block
5. nếu không arena nào đủ chỗ, tính số page tối thiểu cần thiết cho request đó
6. xin một span page liên tiếp từ `page_alloc_contiguous()` rồi seed thành một arena mới

## Luồng giải phóng

`kfree(ptr)`:

1. xác định page chứa con trỏ bằng cách lấy page base
2. kiểm tra `magic` của page để bắt invalid free rõ ràng hơn
3. đánh dấu block là free
4. coalesce với block trước hoặc sau nếu chúng cũng đang free

## Giới hạn hiện tại

Baseline này vẫn giữ phạm vi hẹp:

- allocation lớn cần span page vật lý liên tiếp
- heap chưa có virtual range riêng
- arena rỗng chưa được trả về lại page allocator
- chưa có guard pages hoặc red-zone giữa các allocation

Điều này vẫn đủ cho các object kernel nhỏ như node, queue, descriptor, metadata cấu hình, hoặc buffer ngắn.

## Helper và observability

Heap hiện có các helper thống kê:

- `kernel_heap_total_pages()`
- `kernel_heap_free_bytes()`
- `kernel_heap_used_bytes()`
- `kernel_heap_allocation_count()`
- `kernel_heap_failed_allocations()`
- `kernel_heap_log_stats()`

Boot-time self-test trong `kernel_main()` hiện:

- khởi tạo heap
- cấp phát hai object nhỏ
- cấp phát một object lớn hơn một page
- ghi dữ liệu vào object đã cấp phát
- log thống kê trước và sau `kfree`

## Hướng mở rộng hợp lý

Các bước tiếp theo có giá trị thực tế:

1. trả các heap arena hoàn toàn rỗng về lại page allocator
2. thêm heap-specific debug targets để soi allocation lớn và span liên tiếp
3. cân nhắc virtual heap range riêng nếu không còn muốn phụ thuộc vào contiguous physical spans
4. thêm guard pattern hoặc sanity check để bắt ghi tràn block sớm hơn