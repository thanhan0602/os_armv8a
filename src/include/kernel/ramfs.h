#ifndef KERNEL_RAMFS_H
#define KERNEL_RAMFS_H

#include <kernel/vfs.h>

struct ramfs_builtin_file {
    const char *path;
    unsigned char *start;
    unsigned char *end;
};

#define REGISTER_RAMFS_BUILTIN(name, path, start, end) \
    static const struct ramfs_builtin_file __builtin_##name \
    __attribute__((used, section(".ramfs_builtins"))) = { path, start, end }

void ramfs_init(void);
void ramfs_add_builtin(const char *path, unsigned char *start, unsigned char *end);
int ramfs_register_file(const char *path, unsigned char *data, unsigned long size);
int ramfs_unregister_file(const char *path);
void *ramfs_get_data_ptr(struct file *file);

#endif
