#include <kernel/log.h>

#include <kernel/console.h>
#include <kernel/spinlock.h>
#include <kernel/sched.h>
#include <arch/arm/cpu.h>

#include <stdarg.h>

static void log_putc_unlocked(char ch)
{
    console_putc_unlocked(ch);
}

static void log_write_unlocked(const char *message)
{
    console_write_unlocked(message);
}

static void log_write_hex_unlocked(unsigned long value)
{
    console_write_hex_unlocked(value);
}

static void log_write_u64_unlocked(unsigned long value)
{
    char buffer[21];
    unsigned int index;

    if (value == 0UL) {
        log_putc_unlocked('0');
        return;
    }

    index = 0;
    while (value != 0UL) {
        buffer[index++] = (char)('0' + (value % 10UL));
        value /= 10UL;
    }

    while (index > 0U) {
        log_putc_unlocked(buffer[--index]);
    }
}

void log_init(void)
{
    console_init();
}

void log_putc(char ch)
{
    unsigned long flags;

    console_lock_acquire(&flags);
    log_putc_unlocked(ch);
    console_lock_release(flags);
}

void log_write(const char *message)
{
    unsigned long flags;

    console_lock_acquire(&flags);
    log_write_unlocked(message);
    console_lock_release(flags);
}

void log_write_hex(unsigned long value)
{
    unsigned long flags;

    console_lock_acquire(&flags);
    log_write_hex_unlocked(value);
    console_lock_release(flags);
}

void log_write_u64(unsigned long value)
{
    unsigned long flags;

    console_lock_acquire(&flags);
    log_write_u64_unlocked(value);
    console_lock_release(flags);
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
    unsigned long flags;

    va_start(args, fmt);
    console_lock_acquire(&flags);

    /* Dynamic prefix: [cpuX:task] */
    struct task *curr = arch_get_current_task();
    log_putc_unlocked('[');
    log_write_unlocked("cpu");
    log_write_u64_unlocked((unsigned long)arch_get_cpu_id());
    if (curr && curr->name) {
        log_putc_unlocked(':');
        log_write_unlocked(curr->name);
        log_putc_unlocked(':');
        log_write_u64_unlocked(curr->id);
    }
    log_write_unlocked("] ");

    for (p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            log_putc_unlocked(*p);
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
            log_write_unlocked((s != (const char *)0) ? s : "(null)");
            break;
        }
        case 'c':
            log_putc_unlocked((char)va_arg(args, int));
            break;
        case 'u': {
            unsigned long v = is_long
                ? va_arg(args, unsigned long)
                : (unsigned long)va_arg(args, unsigned int);
            log_write_u64_unlocked(v);
            break;
        }
        case 'd': {
            long v = is_long
                ? va_arg(args, long)
                : (long)va_arg(args, int);
            if (v < 0L) {
                log_putc_unlocked('-');
                log_write_u64_unlocked((unsigned long)(-v));
            } else {
                log_write_u64_unlocked((unsigned long)v);
            }
            break;
        }
        case 'x': {
            unsigned long v = is_long
                ? va_arg(args, unsigned long)
                : (unsigned long)va_arg(args, unsigned int);
            log_write_hex_unlocked(v);
            break;
        }
        case 'p':
            log_write_hex_unlocked((unsigned long)va_arg(args, void *));
            break;
        case '%':
            log_putc_unlocked('%');
            break;
        default:
            log_putc_unlocked('%');
            if (is_long != 0) {
                log_putc_unlocked('l');
            }
            log_putc_unlocked(*p);
            break;
        }
    }

    console_lock_release(flags);
    va_end(args);
}
