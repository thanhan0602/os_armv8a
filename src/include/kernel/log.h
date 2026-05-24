#ifndef KERNEL_LOG_H
#define KERNEL_LOG_H

void log_init(void);
void log_putc(char ch);
void log_write(const char *message);
void log_write_hex(unsigned long value);
void log_write_u64(unsigned long value);
void log_printf(const char *fmt, ...);

/*
 * Unified log macros — callers no longer need to choose between
 * log_write / log_write_u64 / log_write_hex by hand.
 *
 * KER_INFO(msg)         — emit "[info] msg\n"
 * KER_WARN(msg)         — emit "[warn] msg\n"
 * KER_LOGF(fmt, ...)    — printf-style: %s %c %d %ld %u %lu %x %lx %p %%
 */
#define KER_LOGF(fmt, ...)  log_printf(fmt, ##__VA_ARGS__)
#define KER_INFO(msg)       KER_LOGF("[info] %s\n", (msg))
#define KER_WARN(msg)       KER_LOGF("[warn] %s\n", (msg))

#endif