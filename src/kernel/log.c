#include <kernel/log.h>

#include <kernel/console.h>

#include <stdarg.h>

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

/*
 * Minimal printf-style logger.  Supported specifiers:
 *   %s   const char *
 *   %c   char (int-promoted)
 *   %d   int              %ld  long
 *   %u   unsigned int     %lu  unsigned long
 *   %x   unsigned int     %lx  unsigned long   (hex, 0x-prefixed, 16 digits)
 *   %p   void *           (hex, 0x-prefixed, 16 digits)
 *   %%   literal '%'
 */
void log_printf(const char *fmt, ...)
{
    va_list args;
    const char *p;
    int is_long;

    va_start(args, fmt);

    for (p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            log_putc(*p);
            continue;
        }

        p++;
        is_long = 0;
        if (*p == 'l') {
            is_long = 1;
            p++;
        }

        switch (*p) {
        case 's': {
            const char *s = va_arg(args, const char *);
            log_write((s != (const char *)0) ? s : "(null)");
            break;
        }
        case 'c':
            log_putc((char)va_arg(args, int));
            break;
        case 'u': {
            unsigned long v = is_long
                ? va_arg(args, unsigned long)
                : (unsigned long)va_arg(args, unsigned int);
            log_write_u64(v);
            break;
        }
        case 'd': {
            long v = is_long
                ? va_arg(args, long)
                : (long)va_arg(args, int);
            if (v < 0L) {
                log_putc('-');
                log_write_u64((unsigned long)(-v));
            } else {
                log_write_u64((unsigned long)v);
            }
            break;
        }
        case 'x': {
            unsigned long v = is_long
                ? va_arg(args, unsigned long)
                : (unsigned long)va_arg(args, unsigned int);
            log_write_hex(v);
            break;
        }
        case 'p':
            log_write_hex((unsigned long)va_arg(args, void *));
            break;
        case '%':
            log_putc('%');
            break;
        default:
            log_putc('%');
            if (is_long != 0) {
                log_putc('l');
            }
            log_putc(*p);
            break;
        }
    }

    va_end(args);
}
