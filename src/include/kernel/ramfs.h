#ifndef KERNEL_RAMFS_H
#define KERNEL_RAMFS_H

#include <kernel/vfs.h>

void ramfs_init(void);

/* To support the existing shell commands for now */
int ramfs_register_file(const char *path, unsigned char *data, unsigned long size);
int ramfs_unregister_file(const char *path);

#endif
