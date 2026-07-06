#include <kernel/shell.h>

#include <kernel/console.h>
#include <kernel/fs.h>
#include <kernel/heap.h>
#include <kernel/loader.h>
#include <kernel/log.h>
#include <kernel/page_alloc.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>

#define SHELL_INPUT_MAX     128U
#define SHELL_TOKENS_MAX    4U
#define SHELL_READ_CHUNK    16UL
#define SHELL_DEFAULT_READ  64UL
#define SHELL_UPLOAD_PATH_MAX 64U

static struct spinlock shell_command_lock = SPINLOCK_INITIALIZER;
static char shell_input[SHELL_INPUT_MAX];
static unsigned int shell_input_len;
static unsigned char *shell_upload_buffer;
static unsigned long shell_upload_size;
static unsigned long shell_upload_received;
static unsigned int shell_upload_high_nibble_valid;
static unsigned int shell_upload_high_nibble;
static char shell_upload_path[SHELL_UPLOAD_PATH_MAX];

static int shell_is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static int shell_is_printable(char ch)
{
    return ch >= 32 && ch <= 126;
}

static int shell_streq(const char *lhs, const char *rhs)
{
    while (*lhs != '\0' && *rhs != '\0') {
        if (*lhs != *rhs) {
            return 0;
        }
        lhs++;
        rhs++;
    }

    return *lhs == *rhs;
}

static const char *shell_skip_spaces(const char *text)
{
    while (*text != '\0' && shell_is_space(*text)) {
        text++;
    }

    return text;
}

static int shell_parse_ulong(const char *text, unsigned long *value)
{
    unsigned long base;
    unsigned long result;
    int seen_digit;

    if (text == (const char *)0 || value == (unsigned long *)0) {
        return 0;
    }

    base = 10UL;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16UL;
        text += 2;
    }

    result = 0UL;
    seen_digit = 0;
    while (*text != '\0') {
        unsigned long digit;

        if (*text >= '0' && *text <= '9') {
            digit = (unsigned long)(*text - '0');
        } else if (base == 16UL && *text >= 'a' && *text <= 'f') {
            digit = 10UL + (unsigned long)(*text - 'a');
        } else if (base == 16UL && *text >= 'A' && *text <= 'F') {
            digit = 10UL + (unsigned long)(*text - 'A');
        } else {
            return 0;
        }

        result = (result * base) + digit;
        seen_digit = 1;
        text++;
    }

    if (!seen_digit) {
        return 0;
    }

    *value = result;
    return 1;
}

static int shell_hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }

    return -1;
}

static unsigned int shell_tokenize(char *line, char *tokens[SHELL_TOKENS_MAX])
{
    unsigned int count;

    count = 0U;
    while (*line != '\0' && count < SHELL_TOKENS_MAX) {
        while (*line != '\0' && shell_is_space(*line)) {
            *line = '\0';
            line++;
        }

        if (*line == '\0') {
            break;
        }

        tokens[count++] = line;
        while (*line != '\0' && !shell_is_space(*line)) {
            line++;
        }
    }

    return count;
}

static void shell_copy_line(char *dst, const char *src, unsigned int dst_size)
{
    unsigned int index;

    if (dst_size == 0U) {
        return;
    }

    index = 0U;
    while (src[index] != '\0' && index + 1U < dst_size) {
        dst[index] = src[index];
        index++;
    }
    dst[index] = '\0';
}

static const char *shell_default_task_name(const char *path)
{
    const char *name;

    name = path;
    while (*path != '\0') {
        if (*path == '/') {
            name = path + 1;
        }
        path++;
    }

    return name;
}

static void shell_print_prompt(void)
{
    log_write("os> ");
}

static void shell_write_hex_digit(unsigned int value)
{
    static const char digits[] = "0123456789abcdef";

    log_putc(digits[value & 0xfU]);
}

static void shell_write_hex_byte(unsigned int value)
{
    shell_write_hex_digit((value >> 4) & 0xfU);
    shell_write_hex_digit(value & 0xfU);
}

static void shell_write_hex_width(unsigned long value, unsigned int digits)
{
    unsigned int shift;

    shift = digits * 4U;
    while (shift != 0U) {
        shift -= 4U;
        shell_write_hex_digit((unsigned int)((value >> shift) & 0xfUL));
    }
}

static void shell_dump_read_chunk(unsigned long offset,
                                  const unsigned char *buffer,
                                  unsigned long count)
{
    unsigned long index;

    log_write("[shell] ");
    shell_write_hex_width(offset, 8U);
    log_write(" :");
    for (index = 0UL; index < SHELL_READ_CHUNK; index++) {
        if (index < count) {
            log_putc(' ');
            shell_write_hex_byte((unsigned int)buffer[index]);
        } else {
            log_write("   ");
        }
    }

    log_write("  |");
    for (index = 0UL; index < count; index++) {
        log_putc(shell_is_printable((char)buffer[index]) ? (char)buffer[index] : '.');
    }
    log_write("|\n");
}

static void shell_cmd_help(void)
{
    log_write("[shell] commands:\n");
    log_write("[shell]   help\n");
    log_write("[shell]   read <path> [count]\n");
    log_write("[shell]   write <text>\n");
    log_write("[shell]   show process | ps\n");
    log_write("[shell]   show memory | memory | mem\n");
    log_write("[shell]   load <path> [task-name]\n");
    log_write("[shell]   unload <task-id>\n");
    log_write("[shell]   receive <path> <size>\n");
}

static void shell_reset_upload_state(void)
{
    shell_upload_buffer = (unsigned char *)0;
    shell_upload_size = 0UL;
    shell_upload_received = 0UL;
    shell_upload_high_nibble_valid = 0U;
    shell_upload_high_nibble = 0U;
    shell_upload_path[0] = '\0';
}

static void shell_finish_upload(int success)
{
    unsigned char *buffer;
    char path[SHELL_UPLOAD_PATH_MAX];
    unsigned long size;

    buffer = shell_upload_buffer;
    size = shell_upload_size;
    shell_copy_line(path, shell_upload_path, SHELL_UPLOAD_PATH_MAX);

    shell_reset_upload_state();
    if (success) {
        KER_LOGF("[shell] received %s size=%lu\n", path, size);
        if (buffer != (unsigned char *)0) {
            kfree(buffer);
        }
    } else if (buffer != (unsigned char *)0) {
        kfree(buffer);
        log_write("[shell] receive aborted\n");
    }

    shell_print_prompt();
}

static void shell_cmd_read(unsigned int argc, char *argv[])
{
    struct file file;
    unsigned char buffer[SHELL_READ_CHUNK];
    unsigned long limit;
    unsigned long total;

    if (argc < 2U) {
        log_write("[shell] usage: read <path> [count]\n");
        return;
    }

    limit = SHELL_DEFAULT_READ;
    if (argc >= 3U && !shell_parse_ulong(argv[2], &limit)) {
        log_write("[shell] invalid count\n");
        return;
    }

    if (!fs_open(argv[1], &file)) {
        KER_LOGF("[shell] read failed: %s\n", argv[1]);
        return;
    }

    KER_LOGF("[shell] file=%s size=%lu\n", file.name, file.size);
    total = 0UL;
    while (total < limit && total < file.size) {
        unsigned long remaining;
        unsigned long want;
        unsigned long count;

        remaining = limit - total;
        want = remaining < SHELL_READ_CHUNK ? remaining : SHELL_READ_CHUNK;
        count = fs_read(&file, buffer, want);
        if (count == 0UL) {
            break;
        }

        shell_dump_read_chunk(total, buffer, count);
        total += count;
    }

    fs_close(&file);
}

static void shell_cmd_write(const char *line)
{
    const char *text;

    text = shell_skip_spaces(line + 5);
    if (*text == '\0') {
        log_write("[shell] usage: write <text>\n");
        return;
    }

    KER_LOGF("[shell] %s\n", text);
}

static void shell_cmd_memory(void)
{
    KER_LOGF("[shell] pages total=%lu free=%lu reserved_bytes=%lu invalid_free=%lu double_free=%lu\n",
             page_allocator_total_pages(),
             page_allocator_free_pages(),
             page_allocator_reserved_bytes(),
             page_allocator_invalid_free_count(),
             page_allocator_double_free_count());
    KER_LOGF("[shell] heap pages=%lu used=%lu free=%lu allocs=%lu failed=%lu\n",
             kernel_heap_total_pages(),
             kernel_heap_used_bytes(),
             kernel_heap_free_bytes(),
             kernel_heap_allocation_count(),
             kernel_heap_failed_allocations());
}

static void shell_cmd_load(unsigned int argc, char *argv[])
{
    struct process *process;
    struct task *task;
    const char *task_name;

    if (argc < 2U) {
        log_write("[shell] usage: load <path> [task-name]\n");
        return;
    }

    task_name = (argc >= 3U) ? argv[2] : shell_default_task_name(argv[1]);
    process = loader_load_process_image(argv[1]);
    if (process == (struct process *)0) {
        KER_LOGF("[shell] load failed: %s\n", argv[1]);
        return;
    }

    task = task_create_user(process, task_name);
    if (task == (struct task *)0) {
        process_destroy(process);
        KER_LOGF("[shell] spawn failed: %s\n", task_name);
        return;
    }
    sched_wake_task(task);

    KER_LOGF("[shell] loaded %s as task id=%lu entry=%lx brk=%lx\n",
             task_name,
             task->id,
             process->entry_va,
             process->brk);
}

static void shell_cmd_unload(unsigned int argc, char *argv[])
{
    unsigned long task_id;

    if (argc < 2U) {
        log_write("[shell] usage: unload <task-id>\n");
        return;
    }

    if (!shell_parse_ulong(argv[1], &task_id)) {
        log_write("[shell] invalid task id\n");
        return;
    }

    if (!sched_kill_task(task_id)) {
        KER_LOGF("[shell] unload failed: %lu\n", task_id);
        return;
    }

    KER_LOGF("[shell] task %lu marked dead\n", task_id);
}

static void shell_cmd_receive(unsigned int argc, char *argv[])
{
    unsigned long size;

    if (argc < 3U) {
        log_write("[shell] usage: receive <path> <size>\n");
        return;
    }

    if (shell_upload_buffer != (unsigned char *)0) {
        log_write("[shell] receive already active\n");
        return;
    }

    if (!shell_parse_ulong(argv[2], &size) || size == 0UL) {
        log_write("[shell] invalid receive size\n");
        return;
    }

    shell_copy_line(shell_upload_path, argv[1], SHELL_UPLOAD_PATH_MAX);
    if (shell_upload_path[0] == '\0') {
        log_write("[shell] invalid receive path\n");
        return;
    }

    shell_upload_buffer = (unsigned char *)kmalloc(size);
    if (shell_upload_buffer == (unsigned char *)0) {
        log_write("[shell] receive alloc failed\n");
        shell_reset_upload_state();
        return;
    }

    shell_upload_size = size;
    shell_upload_received = 0UL;
    shell_upload_high_nibble_valid = 0U;
    shell_upload_high_nibble = 0U;
    KER_LOGF("[shell] receiving %s size=%lu as hex stream\n", shell_upload_path, size);
}

static void shell_execute(char *line)
{
    char *argv[SHELL_TOKENS_MAX];
    char raw_line[SHELL_INPUT_MAX];
    unsigned int argc;
    unsigned long flags;

    shell_copy_line(raw_line, line, SHELL_INPUT_MAX);
    argc = shell_tokenize(line, argv);
    if (argc == 0U) {
        return;
    }

    flags = spin_lock_irqsave(&shell_command_lock);

    if (shell_streq(argv[0], "help")) {
        shell_cmd_help();
    } else if (shell_streq(argv[0], "read")) {
        shell_cmd_read(argc, argv);
    } else if (shell_streq(argv[0], "write")) {
        shell_cmd_write(raw_line);
    } else if (shell_streq(argv[0], "ps")
            || (argc >= 2U && shell_streq(argv[0], "show") && shell_streq(argv[1], "process"))) {
        sched_dump_tasks();
    } else if (shell_streq(argv[0], "mem")
            || shell_streq(argv[0], "memory")
            || (argc >= 2U && shell_streq(argv[0], "show") && shell_streq(argv[1], "memory"))) {
        shell_cmd_memory();
    } else if (shell_streq(argv[0], "load")) {
        shell_cmd_load(argc, argv);
    } else if (shell_streq(argv[0], "unload")) {
        shell_cmd_unload(argc, argv);
    } else if (shell_streq(argv[0], "receive")) {
        shell_cmd_receive(argc, argv);
    } else {
        KER_LOGF("[shell] unknown command: %s\n", argv[0]);
    }

    spin_unlock_irqrestore(&shell_command_lock, flags);
}

void shell_init(void)
{
    shell_input_len = 0U;
    spinlock_init(&shell_command_lock);
    shell_reset_upload_state();
    log_write("[shell] ready\n");
    shell_print_prompt();
}

int shell_poll(void)
{
    char ch;

    if (shell_upload_buffer != (unsigned char *)0) {
        if (!console_try_getc(&ch)) {
            return 0;
        }

        if (shell_is_space(ch)) {
            return 1;
        }

        if (shell_upload_received >= shell_upload_size) {
            if (!fs_register_file(shell_upload_path, shell_upload_buffer, shell_upload_size)) {
                shell_finish_upload(0);
                return 1;
            }

            shell_finish_upload(1);
            return 1;
        }

        {
            int value;

            value = shell_hex_value(ch);
            if (value < 0) {
                shell_finish_upload(0);
                return 1;
            }

            if (shell_upload_high_nibble_valid == 0U) {
                shell_upload_high_nibble = (unsigned int)value;
                shell_upload_high_nibble_valid = 1U;
                return 1;
            }

            shell_upload_buffer[shell_upload_received++] =
                (unsigned char)((shell_upload_high_nibble << 4) | (unsigned int)value);
            shell_upload_high_nibble_valid = 0U;
            if (shell_upload_received == shell_upload_size) {
                if (!fs_register_file(shell_upload_path, shell_upload_buffer, shell_upload_size)) {
                    shell_finish_upload(0);
                    return 1;
                }

                shell_finish_upload(1);
            }
            return 1;
        }
    }

    if (!console_try_getc(&ch)) {
        return 0;
    }

    if (ch == '\r' || ch == '\n') {
        log_write("\n");
        shell_input[shell_input_len] = '\0';
        shell_execute(shell_input);
        shell_input_len = 0U;
        shell_print_prompt();
        return 1;
    }

    if (ch == '\b' || ch == 127) {
        if (shell_input_len != 0U) {
            shell_input_len--;
            log_write("\b \b");
        }
        return 1;
    }

    if (!shell_is_printable(ch)) {
        return 1;
    }

    if (shell_input_len >= (SHELL_INPUT_MAX - 1U)) {
        log_write("\n[shell] input too long\n");
        shell_input_len = 0U;
        shell_print_prompt();
        return 1;
    }

    shell_input[shell_input_len++] = ch;
    log_putc(ch);
    return 1;
}