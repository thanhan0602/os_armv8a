#include <kernel/console.h>
#include <kernel/vfs.h>
#include <kernel/heap.h>

#include <drivers/uart/pl011.h>
#include <kernel/driver.h>

static int console_vnode_open(struct vnode *vn, struct file *file)
{
    (void)vn;
    file->offset = 0;
    file->size = 0;
    return 1;
}

static unsigned long console_vnode_write(struct vnode *vn, struct file *file, const void *src, unsigned long len)
{
    (void)vn;
    (void)file;
    const char *buf = (const char *)src;
    for (unsigned long i = 0; i < len; i++) {
        console_putc(buf[i]);
    }
    return len;
}

static struct vnode_ops console_ops = {
    .open = console_vnode_open,
    .write = console_vnode_write,
};

static struct vnode console_vnode = {
    .ops = &console_ops,
    .is_dir = 0,
};

struct file *console_open_file(void)
{
    struct file *f = (struct file *)kmalloc(sizeof(struct file));
    if (!f) return (struct file *)0;
    f->vn = &console_vnode;
    f->name = "console";
    f->offset = 0;
    f->size = 0;
    return f;
}

void console_init(void)
{
    if (!driver_uart_is_ready()) {
        pl011_init();
    }
}

void console_putc(char ch)
{
    if (ch == '\n') {
        pl011_write('\r');
    }

    pl011_write((unsigned int)(unsigned char)ch);
}

void console_write(const char *message)
{
    while (*message != '\0') {
        console_putc(*message);
        message++;
    }
}

void console_write_hex(unsigned long value)
{
    static const char digits[] = "0123456789abcdef";
    char buffer[2 + (sizeof(unsigned long) * 2) + 1];
    unsigned int shift;
    unsigned int index;

    buffer[0] = '0';
    buffer[1] = 'x';
    index = 2;

    for (shift = (unsigned int)((sizeof(unsigned long) * 8) - 4); ; shift -= 4) {
        buffer[index++] = digits[(value >> shift) & 0xfUL];
        if (shift == 0) {
            break;
        }
    }

    buffer[index] = '\0';
    console_write(buffer);
}

int console_try_getc(char *ch)
{
    if (ch == (char *)0 || !pl011_can_read()) {
        return 0;
    }

    *ch = (char)pl011_read();
    return 1;
}