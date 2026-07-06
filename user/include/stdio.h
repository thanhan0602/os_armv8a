#ifndef _STDIO_H
#define _STDIO_H

#include <stdarg.h>

int printf(const char *format, ...);
int vsnprintf(char *str, unsigned long size, const char *format, va_list ap);
int puts(const char *s);
int putchar(int c);

#endif
