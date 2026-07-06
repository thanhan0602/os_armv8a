.section .text.start
.global _start
.hidden _start
.hidden ld_self_relocate
.hidden ld_main

_start:
    /* 
     * Khi Kernel nạp ld.so, SP đang trỏ tới Stack chứa AUX Vectors, Env, Args.
     */
    
    /* Tính toán base address thực tế */
    /* ADR x0, _start lấy địa chỉ PC-relative của _start */
    adr x0, _start
    
    /* Lấy địa chỉ của mảng _DYNAMIC */
    adrp x1, _DYNAMIC
    add x1, x1, :lo12:_DYNAMIC

    /* Gọi hàm self-relocation trực tiếp */
    bl ld_self_relocate

    /* Sau khi relocate xong, gọi main của linker */
    mov x0, sp
    bl ld_main

    /* Linker không bao giờ nên quay lại đây */
1:  wfe
    b 1b
