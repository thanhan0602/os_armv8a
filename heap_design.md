# Thiết kế kernel heap

## Mục tiêu

Tài liệu này mô tả baseline Stage 7 hiện tại: một kernel heap nhỏ, đặt trên page allocator vật lý hiện có, đủ để cấp phát các cấu trúc động nhỏ trong kernel.

## Tổng quan

Kernel heap hiện được khởi tạo sau `mmu_init()`.

Heap dùng page allocator làm backend, nhưng không yêu cầu các page phải liên tiếp vật lý. Mỗi heap page là một arena độc lập, bên trong có danh sách block riêng để phục vụ `kmalloc` và `kfree`.

Thiết kế hiện tại ưu tiên:

- đơn giản khi bring-up
- dễ debug
- không phụ thuộc vào virtual heap layout riêng

## Cấu trúc nội bộ

Mỗi page heap có:

- `struct kernel_heap_page` ở đầu page
- một `struct kernel_heap_block` đầu tiên mô tả phần free còn lại trong page
- các block tiếp theo được split dần khi có allocation

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
5. nếu không page nào đủ chỗ, xin thêm một page mới từ `page_alloc()` rồi thử lại

## Luồng giải phóng

`kfree(ptr)`:

1. xác định page chứa con trỏ bằng cách lấy page base
2. kiểm tra `magic` của page để bắt invalid free rõ ràng hơn
3. đánh dấu block là free
4. coalesce với block trước hoặc sau nếu chúng cũng đang free

## Giới hạn hiện tại

Baseline này cố ý giữ phạm vi hẹp:

- mỗi allocation phải vừa trong một heap page đơn lẻ
- chưa có allocation nhiều page liên tiếp
- chưa có heap virtual range riêng
- chưa trả page rỗng hoàn toàn về lại page allocator

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
- ghi dữ liệu vào object đã cấp phát
- log thống kê trước và sau `kfree`

## Hướng mở rộng hợp lý

Các bước tiếp theo có giá trị thực tế:

1. thêm chiến lược allocation nhiều page cho object lớn
2. tách heap sang một virtual range riêng thay vì chỉ dựa vào identity map hiện tại
3. trả các heap page hoàn toàn rỗng về lại page allocator
4. thêm guard pattern hoặc sanity check để bắt ghi tràn block sớm hơn