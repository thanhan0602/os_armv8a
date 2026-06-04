#ifndef KERNEL_FS_H
#define KERNEL_FS_H

struct vnode;

struct file {
    struct vnode *vn;
    const char *name;
    unsigned long offset;
    unsigned long size;
};

void fs_init(void);
int fs_open(const char *path, struct file *file);
unsigned long fs_read(struct file *file, void *dst, unsigned long len);
void fs_close(struct file *file);
int fs_register_file(const char *path, unsigned char *data, unsigned long size);
int fs_unregister_file(const char *path);

#endif