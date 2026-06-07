#ifndef KERNEL_CONSOLE_H
#define KERNEL_CONSOLE_H

struct file;

void console_init(void);
void console_putc(char ch);
void console_write(const char *message);
void console_write_hex(unsigned long value);
int console_try_getc(char *ch);

struct file *console_open_file(void);

#endif