#ifndef KERNEL_FS_H
#define KERNEL_FS_H

struct file {
    const char *name;
    const unsigned char *data;
    unsigned long size;
    unsigned long offset;
};

void fs_init(void);
int fs_open(const char *path, struct file *file);
unsigned long fs_read(struct file *file, void *dst, unsigned long len);
void fs_close(struct file *file);
int fs_register_file(const char *path, unsigned char *data, unsigned long size);
int fs_unregister_file(const char *path);

#endif