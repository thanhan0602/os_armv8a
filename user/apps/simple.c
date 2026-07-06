
static void sys_puts(const char *s) {
    unsigned long len = 0;
    while(s[len]) len++;
    asm volatile (
        "mov x0, #1\n"
        "mov x1, %0\n"
        "mov x2, %1\n"
        "mov x8, #64\n"
        "svc #0\n"
        : : "r"(s), "r"(len) : "x0", "x1", "x2", "x8"
    );
}

static void sys_exit(int code) {
    asm volatile (
        "mov x0, %0\n"
        "mov x8, #93\n"
        "svc #0\n"
        : : "r"((unsigned long)code) : "x0", "x8"
    );
}

const char *msg = "Hello from simple PIE!\n";

void _start() {
    sys_puts(msg);
    sys_exit(0);
}
