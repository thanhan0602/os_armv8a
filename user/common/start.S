.section .text.start

.global _start
.type _start, %function

_start:
    bl      main

    mov     x8, #93
    svc     #0

1:  b       1b