#ifndef KERNEL_VFS_H
#define KERNEL_VFS_H

#include <kernel/fs.h>

#define VFS_NAME_MAX 64
#define VFS_PATH_MAX 256

struct vnode;
struct vfs_mount;

struct vnode_ops {
    int (*open)(struct vnode *vn, struct file *file);
    unsigned long (*read)(struct vnode *vn, struct file *file, void *dst, unsigned long len);
    unsigned long (*write)(struct vnode *vn, struct file *file, const void *src, unsigned long len);
    void (*close)(struct vnode *vn, struct file *file);
    int (*lookup)(struct vnode *vn, const char *name, struct vnode **out);
    int (*create)(struct vnode *vn, const char *name, struct vnode **out);
};

struct vfs_ops {
    int (*mount)(struct vfs_mount *mnt, const char *device);
    int (*unmount)(struct vfs_mount *mnt);
    int (*root)(struct vfs_mount *mnt, struct vnode **out);
};

struct vnode {
    struct vfs_mount *mnt;
    struct vnode_ops *ops;
    void *data; /* FS-specific data for this node */
    int is_dir;
};

struct vfs_mount {
    const char *path;
    struct vfs_ops *ops;
    struct vnode *vnode_root;
    void *data; /* FS-specific data for this mount */
    struct vfs_mount *next;
};

struct fs_type {
    const char *name;
    struct vfs_ops *ops;
    struct fs_type *next;
};

/* VFS Core API */
void vfs_init(void);
int vfs_register_fs(const char *name, struct vfs_ops *ops);
int vfs_mount(const char *path, const char *fs_name, const char *device);

/* Generic File operations (will be used by fs_open etc) */
int vfs_open(const char *path, struct file *file);
unsigned long vfs_read(struct file *file, void *dst, unsigned long len);
unsigned long vfs_write(struct file *file, const void *src, unsigned long len);
void vfs_close(struct file *file);

/* Internal path resolution */
int vfs_lookup(const char *path, struct vnode **out);

#endif
