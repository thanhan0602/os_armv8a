#ifndef USER_SHARED_H
#define USER_SHARED_H

#include <stdarg.h>

void shared_write(const char *text);

int shared_vsnprintf(char *buf, unsigned long size, const char *fmt, va_list args);
int shared_printf(const char *fmt, ...);

#define printf shared_printf

#endif /* USER_SHARED_H */