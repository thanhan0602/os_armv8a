#include <kernel/ramfs.h>
#include <kernel/heap.h>
#include <kernel/log.h>
#include <kernel/vm.h>

#ifdef CONFIG_KERNEL_VIRTUAL

#define RAMFS_FILES_MAX  16UL
#define RAMFS_PATH_MAX   64UL

struct ramfs_node {
    int in_use;
    int owns_data;
    char path[RAMFS_PATH_MAX];
    unsigned char *data;
    unsigned long size;
};

static struct ramfs_node ramfs_nodes[RAMFS_FILES_MAX];

/* Forward declarations */
static int ramfs_open(struct vnode *vn, struct file *file);
static unsigned long ramfs_read(struct vnode *vn, struct file *file, void *dst, unsigned long len);
static void ramfs_close(struct vnode *vn, struct file *file);
static int ramfs_lookup(struct vnode *vn, const char *name, struct vnode **out);
static int ramfs_vfs_mount(struct vfs_mount *mnt, const char *device);

/* External symbols from linker script for the builtins section */
extern const struct ramfs_builtin_file __start_ramfs_builtins[];
extern const struct ramfs_builtin_file __stop_ramfs_builtins[];

static struct vnode_ops ramfs_vnode_ops = {
    .open = ramfs_open,
    .read = ramfs_read,
    .close = ramfs_close,
    .lookup = ramfs_lookup
};

static struct vfs_ops ramfs_vfs_ops = {
    .mount = ramfs_vfs_mount
};

static int ramfs_path_equals(const char *lhs, const char *rhs)
{
    while (*lhs != '\0' && *rhs != '\0') {
        if (*lhs != *rhs) {
            return 0;
        }
        lhs++;
        rhs++;
    }
    return *lhs == *rhs;
}

static struct ramfs_node *ramfs_find_node(const char *path)
{
    unsigned long index;
    for (index = 0UL; index < RAMFS_FILES_MAX; index++) {
        if (ramfs_nodes[index].in_use && ramfs_path_equals(ramfs_nodes[index].path, path)) {
            return &ramfs_nodes[index];
        }
    }
    return (struct ramfs_node *)0;
}

/* VFS Interface implementation for ramfs */

static int ramfs_open(struct vnode *vn, struct file *file)
{
    struct ramfs_node *node = (struct ramfs_node *)vn->data;
    file->vn = vn;
    file->offset = 0UL;
    file->size = node->size;
    return 1;
}

static unsigned long ramfs_read(struct vnode *vn, struct file *file, void *dst, unsigned long len)
{
    struct ramfs_node *node = (struct ramfs_node *)vn->data;
    unsigned char *out;
    unsigned long remaining;
    unsigned long count;
    unsigned long index;

    if (file->offset >= node->size) {
        return 0UL;
    }

    remaining = node->size - file->offset;
    count = (len < remaining) ? len : remaining;
    out = (unsigned char *)dst;
    for (index = 0UL; index < count; index++) {
        out[index] = node->data[file->offset + index];
    }

    file->offset += count;
    return count;
}

static void ramfs_close(struct vnode *vn, struct file *file)
{
    (void)vn;
    (void)file;
}

static int ramfs_vfs_mount(struct vfs_mount *mnt, const char *device)
{
    (void)device;
    struct vnode *root = (struct vnode *)kmalloc(sizeof(struct vnode));
    if (!root) return 0;
    root->mnt = mnt;
    root->ops = &ramfs_vnode_ops;
    root->data = (void *)0; /* Root doesn't have a node per se in this simple flat ramfs */
    root->is_dir = 1;
    mnt->vnode_root = root;
    return 1;
}

static int ramfs_lookup(struct vnode *vn, const char *name, struct vnode **out)
{
    /* For now, ramfs is flat and name matches full path or we just match the filename if we are at root */
    /* Since current fs.c matches full paths, we'll keep it simple: if name matches a node, return it. */
    struct ramfs_node *node = ramfs_find_node(name);
    if (node) {
        struct vnode *new_vn = (struct vnode *)kmalloc(sizeof(struct vnode));
        if (!new_vn) return 0;
        new_vn->mnt = vn->mnt;
        new_vn->ops = vn->ops;
        new_vn->data = node;
        new_vn->is_dir = 0;
        *out = new_vn;
        return 1;
    }
    KER_LOGF("[ramfs] lookup failed: node not found for %s\n", name);
    return 0;
}

void ramfs_add_builtin(const char *path, unsigned char *start, unsigned char *end)
{
    unsigned long index;
    for (index = 0UL; index < RAMFS_FILES_MAX; index++) {
        if (!ramfs_nodes[index].in_use) {
            ramfs_nodes[index].in_use = 1;
            ramfs_nodes[index].owns_data = 0;
            unsigned long i;
            for (i = 0; i < RAMFS_PATH_MAX - 1 && path[i]; i++) ramfs_nodes[index].path[i] = path[i];
            ramfs_nodes[index].path[i] = '\0';
            ramfs_nodes[index].data = start;
            ramfs_nodes[index].size = (unsigned long)(end - start);
            return;
        }
    }
}

void ramfs_init(void)
{
    unsigned long index;
    const struct ramfs_builtin_file *builtin;

    for (index = 0UL; index < RAMFS_FILES_MAX; index++) {
        ramfs_nodes[index].in_use = 0;
    }

    /* Iterate over the linker-defined section for builtin files */
    for (builtin = __start_ramfs_builtins; builtin < __stop_ramfs_builtins; builtin++) {
        ramfs_add_builtin(builtin->path, builtin->start, builtin->end);
    }

    vfs_register_fs("ramfs", &ramfs_vfs_ops);
}

int ramfs_register_file(const char *path, unsigned char *data, unsigned long size)
{
    struct ramfs_node *node = ramfs_find_node(path);
    if (!node) {
        unsigned long index;
        for (index = 0UL; index < RAMFS_FILES_MAX; index++) {
            if (!ramfs_nodes[index].in_use) {
                node = &ramfs_nodes[index];
                break;
            }
        }
    }
    if (!node) return 0;

    unsigned char *copy = (unsigned char *)kmalloc(size);
    if (!copy) return 0;
    unsigned long i;
    for (i = 0; i < size; i++) copy[i] = data[i];

    if (node->in_use && node->owns_data) kfree(node->data);

    node->in_use = 1;
    node->owns_data = 1;
    for (i = 0; i < RAMFS_PATH_MAX - 1 && path[i]; i++) node->path[i] = path[i];
    node->path[i] = '\0';
    node->data = copy;
    node->size = size;
    return 1;
}

int ramfs_unregister_file(const char *path)
{
    struct ramfs_node *node = ramfs_find_node(path);
    if (!node) return 0;
    if (node->owns_data) kfree(node->data);
    node->in_use = 0;
    return 1;
}

void *ramfs_get_data_ptr(struct file *file)
{
    if (!file || !file->vn || !file->vn->data) return (void *)0;
    /* Basic check: make sure the vnode ops are ramfs ops */
    extern struct vnode_ops ramfs_vnode_ops;
    if (file->vn->ops != &ramfs_vnode_ops) return (void *)0;
    
    struct ramfs_node *node = (struct ramfs_node *)file->vn->data;
    return (void *)node->data;
}

#else
void ramfs_init(void) {}
int ramfs_register_file(const char *path, unsigned char *data, unsigned long size) { (void)path; (void)data; (void)size; return 0; }
int ramfs_unregister_file(const char *path) { (void)path; return 0; }
#endif
