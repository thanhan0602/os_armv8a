#include <user/shared.h>
#include <user/syscall.h>

void shared_write(const char *text)
{
    user_write_string(text);
}

static void shared_reverse(char *s, int len)
{
    int i = 0, j = len - 1;
    while (i < j) {
        char t = s[i];
        s[i] = s[j];
        s[j] = t;
        i++; j--;
    }
}

static int shared_itoa(long long n, char *s, int base, int is_unsigned)
{
    int i = 0;
    unsigned long long num = (unsigned long long)n;
    int negative = 0;

    if (!is_unsigned && n < 0) {
        negative = 1;
        num = (unsigned long long)-n;
    }

    if (num == 0) s[i++] = '0';
    else {
        while (num != 0) {
            int rem = num % base;
            s[i++] = (rem > 9) ? (rem - 10) + 'a' : rem + '0';
            num /= base;
        }
    }

    if (negative) s[i++] = '-';
    s[i] = '\0';
    shared_reverse(s, i);
    return i;
}

int shared_vsnprintf(char *buf, unsigned long size, const char *fmt, va_list args)
{
    unsigned long i = 0;
    const char *p = fmt;
    char *out = buf;

    while (*p && i < size - 1) {
        if (*p == '%') {
            p++;
            int long_mod = 0;
            if (*p == 'l') {
                long_mod = 1;
                p++;
            }
            if (*p == 's') {
                char *s = va_arg(args, char *);
                while (*s && i < size - 1) {
                    out[i++] = *s++;
                }
            } else if (*p == 'd' || *p == 'i') {
                char tmp[32];
                long long val = long_mod ? va_arg(args, long) : va_arg(args, int);
                int len = shared_itoa(val, tmp, 10, 0);
                for (int j = 0; j < len && i < size - 1; j++) {
                    out[i++] = tmp[j];
                }
            } else if (*p == 'u') {
                char tmp[32];
                unsigned long long val = long_mod ? va_arg(args, unsigned long) : va_arg(args, unsigned int);
                int len = shared_itoa((long long)val, tmp, 10, 1);
                for (int j = 0; j < len && i < size - 1; j++) {
                    out[i++] = tmp[j];
                }
            } else if (*p == 'x' || *p == 'p') {
                char tmp[32];
                unsigned long long val;
                if (*p == 'p') val = (unsigned long long)va_arg(args, void *);
                else val = long_mod ? va_arg(args, unsigned long) : va_arg(args, unsigned int);
                int len = shared_itoa((long long)val, tmp, 16, 1);
                for (int j = 0; j < len && i < size - 1; j++) {
                    out[i++] = tmp[j];
                }
            } else {
                out[i++] = '%';
                if (i < size - 1) out[i++] = *p;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return (int)i;
}

int shared_printf(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    int len;

    va_start(args, fmt);
    len = shared_vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    user_write(1UL, buf, (unsigned long)len);
    return len;
}