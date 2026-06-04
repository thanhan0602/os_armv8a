#include <kernel/vfs.h>
#include <kernel/ramfs.h>
#include <kernel/heap.h>
#include <kernel/log.h>
#include <kernel/vm.h>

static struct fs_type *fs_types = (struct fs_type *)0;
static struct vfs_mount *mounts = (struct vfs_mount *)0;

void vfs_init(void)
{
    fs_types = (struct fs_type *)0;
    mounts = (struct vfs_mount *)0;
}

int vfs_register_fs(const char *name, struct vfs_ops *ops)
{
    struct fs_type *type = (struct fs_type *)kmalloc(sizeof(struct fs_type));
    if (!type) return 0;

    type->name = name;
    type->ops = ops;
    type->next = fs_types;
    fs_types = type;

    KER_LOGF("vfs", "registered filesystem: %s", name);
    return 1;
}

int vfs_mount(const char *path, const char *fs_name, const char *device)
{
    struct fs_type *type = fs_types;
    while (type) {
        const char *t1 = type->name;
        const char *t2 = fs_name;
        while (*t1 && *t1 == *t2) { t1++; t2++; }
        if (*t1 == *t2) break;
        type = type->next;
    }

    if (!type) return 0;

    struct vfs_mount *mnt = (struct vfs_mount *)kmalloc(sizeof(struct vfs_mount));
    if (!mnt) return 0;

    mnt->path = path;
    mnt->ops = type->ops;
    mnt->data = (void *)0;
    mnt->next = mounts;

    if (!mnt->ops->mount(mnt, device)) {
        kfree(mnt);
        return 0;
    }

    mounts = mnt;
    KER_LOGF("vfs", "mounted %s on %s", fs_name, path);
    return 1;
}

int vfs_lookup(const char *path, struct vnode **out)
{
    if (!path || path[0] != '/') return 0;

    struct vfs_mount *best_mnt = (struct vfs_mount *)0;
    unsigned long max_len = 0;
    struct vfs_mount *mnt = mounts;

    while (mnt) {
        unsigned long len = 0;
        const char *p1 = path;
        const char *p2 = mnt->path;
        while (*p2 && *p1 == *p2) { p1++; p2++; len++; }
        if (*p2 == '\0' && (len > max_len || best_mnt == (void *)0)) {
            max_len = len;
            best_mnt = mnt;
        }
        mnt = mnt->next;
    }

    if (!best_mnt) return 0;

    return best_mnt->vnode_root->ops->lookup(best_mnt->vnode_root, path, out);
}

int fs_open(const char *path, struct file *file)
{
    struct vnode *vn;
    if (!vfs_lookup(path, &vn)) return 0;
    file->name = path;
    if (vn->ops->open(vn, file)) return 1;
    return 0;
}

unsigned long fs_read(struct file *file, void *dst, unsigned long len)
{
    if (!file || !file->vn) return 0;
    return file->vn->ops->read(file->vn, file, dst, len);
}

void fs_close(struct file *file)
{
    if (!file || !file->vn) return;
    file->vn->ops->close(file->vn, file);
    kfree(file->vn);
    file->vn = (struct vnode *)0;
}

#ifdef CONFIG_KERNEL_VIRTUAL
int fs_register_file(const char *path, unsigned char *data, unsigned long size)
{
    return ramfs_register_file(path, data, size);
}

int fs_unregister_file(const char *path)
{
    return ramfs_unregister_file(path);
}

void fs_init(void)
{
    vfs_init();
    ramfs_init();
    vfs_mount("/", "ramfs", (void *)0);
}
#else
int fs_register_file(const char *path, unsigned char *data, unsigned long size) { return 0; }
int fs_unregister_file(const char *path) { return 0; }
void fs_init(void) {}
#endif
