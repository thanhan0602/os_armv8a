#ifndef KERNEL_LOG_H
#define KERNEL_LOG_H

void log_init(void);
void log_putc(char ch);
void log_write(const char *message);
void log_write_hex(unsigned long value);
void log_write_u64(unsigned long value);
void log_info(const char *message);

#endif