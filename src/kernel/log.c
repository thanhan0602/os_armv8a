#include <kernel/log.h>

#include <kernel/console.h>

void log_init(void)
{
    console_init();
}

void log_putc(char ch)
{
    console_putc(ch);
}

void log_write(const char *message)
{
    console_write(message);
}

void log_write_hex(unsigned long value)
{
    console_write_hex(value);
}

void log_write_u64(unsigned long value)
{
    char buffer[21];
    unsigned int index;

    if (value == 0UL) {
        log_putc('0');
        return;
    }

    index = 0;
    while (value != 0UL) {
        buffer[index++] = (char)('0' + (value % 10UL));
        value /= 10UL;
    }

    while (index > 0U) {
        log_putc(buffer[--index]);
    }
}

void log_info(const char *message)
{
    log_write("[info] ");
    log_write(message);
    log_putc('\n');
}