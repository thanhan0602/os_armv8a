#include <stdio.h>
#include <unistd.h>

/**
 * test_lazy.c
 * 
 * Mục đích: Kiểm tra cơ chế Lazy Binding.
 * Khi chạy với LD_DEBUG=1, ta sẽ thấy:
 * 1. Lần gọi printf đầu tiên: Kích hoạt ld_plt_resolver để tìm symbol.
 * 2. Lần gọi printf thứ hai: Nhảy trực tiếp vào GOT đã được patch, không qua resolver nữa.
 */

int main(void)
{
    printf("--- Phase 1: First call to printf (triggers lazy resolution) ---\n");
    
    printf("--- Phase 2: Second call to printf (should be direct via GOT) ---\n");
    
    printf("--- Phase 3: Testing another function (sleep) ---\n");
    sleep(0);

    printf("Test lazy binding completed successfully!\n");
    
    return 0;
}
