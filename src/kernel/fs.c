#include <kernel/fs.h>

#include <kernel/heap.h>
#include <kernel/vm.h>

#ifdef CONFIG_KERNEL_VIRTUAL

#define FS_DYNAMIC_FILES_MAX  8UL
#define FS_DYNAMIC_PATH_MAX   64UL

extern unsigned char _binary_build_user_builtin_hello_elf_start[];
extern unsigned char _binary_build_user_builtin_hello_elf_end[];
extern unsigned char _binary_build_user_builtin_fault_elf_start[];
extern unsigned char _binary_build_user_builtin_fault_elf_end[];
extern unsigned char _binary_build_user_lib_libshared_so_start[];
extern unsigned char _binary_build_user_lib_libshared_so_end[];
extern unsigned char _binary_build_user_external_shared_client_elf_start[];
extern unsigned char _binary_build_user_external_shared_client_elf_end[];

struct dynamic_ramfs_node {
    int in_use;
    char path[FS_DYNAMIC_PATH_MAX];
    unsigned char *data;
    unsigned long size;
};

static struct dynamic_ramfs_node dynamic_ramfs_nodes[FS_DYNAMIC_FILES_MAX];

static int fs_path_equals(const char *lhs, const char *rhs)
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

static int fs_copy_path(char *dst, const char *src)
{
    unsigned long index;

    if (dst == (char *)0 || src == (const char *)0 || src[0] == '\0') {
        return 0;
    }

    for (index = 0UL; index + 1UL < FS_DYNAMIC_PATH_MAX && src[index] != '\0'; index++) {
        dst[index] = src[index];
    }

    if (src[index] != '\0') {
        return 0;
    }

    dst[index] = '\0';
    return 1;
}

static struct dynamic_ramfs_node *fs_find_dynamic_node(const char *path)
{
    unsigned long index;

    for (index = 0UL; index < FS_DYNAMIC_FILES_MAX; index++) {
        if (dynamic_ramfs_nodes[index].in_use == 0) {
            continue;
        }

        if (fs_path_equals(dynamic_ramfs_nodes[index].path, path)) {
            return &dynamic_ramfs_nodes[index];
        }
    }

    return (struct dynamic_ramfs_node *)0;
}

static struct dynamic_ramfs_node *fs_find_free_dynamic_node(void)
{
    unsigned long index;

    for (index = 0UL; index < FS_DYNAMIC_FILES_MAX; index++) {
        if (dynamic_ramfs_nodes[index].in_use == 0) {
            return &dynamic_ramfs_nodes[index];
        }
    }

    return (struct dynamic_ramfs_node *)0;
}

void fs_init(void)
{
    unsigned long index;

    for (index = 0UL; index < FS_DYNAMIC_FILES_MAX; index++) {
        dynamic_ramfs_nodes[index].in_use = 0;
        dynamic_ramfs_nodes[index].path[0] = '\0';
        dynamic_ramfs_nodes[index].data = (unsigned char *)0;
        dynamic_ramfs_nodes[index].size = 0UL;
    }
}

int fs_open(const char *path, struct file *file)
{
    struct dynamic_ramfs_node *dynamic_node;

    if (path == (const char *)0 || file == (struct file *)0) {
        return 0;
    }

    dynamic_node = fs_find_dynamic_node(path);
    if (dynamic_node != (struct dynamic_ramfs_node *)0) {
        file->name = dynamic_node->path;
        file->data = dynamic_node->data;
        file->size = dynamic_node->size;
        file->offset = 0UL;
        return 1;
    }

    if (fs_path_equals(path, "/bin/hello.elf")) {
        file->name = "/bin/hello.elf";
        file->data = _binary_build_user_builtin_hello_elf_start;
        file->size = (unsigned long)(_binary_build_user_builtin_hello_elf_end -
                                     _binary_build_user_builtin_hello_elf_start);
        file->offset = 0UL;
        return 1;
    }

    if (fs_path_equals(path, "/bin/fault.elf")) {
        file->name = "/bin/fault.elf";
        file->data = _binary_build_user_builtin_fault_elf_start;
        file->size = (unsigned long)(_binary_build_user_builtin_fault_elf_end -
                                     _binary_build_user_builtin_fault_elf_start);
        file->offset = 0UL;
        return 1;
    }

    if (fs_path_equals(path, "/bin/shared_client.elf")) {
        file->name = "/bin/shared_client.elf";
        file->data = _binary_build_user_external_shared_client_elf_start;
        file->size = (unsigned long)(_binary_build_user_external_shared_client_elf_end -
                                     _binary_build_user_external_shared_client_elf_start);
        file->offset = 0UL;
        return 1;
    }

    if (fs_path_equals(path, "/lib/libshared.so")) {
        file->name = "/lib/libshared.so";
        file->data = _binary_build_user_lib_libshared_so_start;
        file->size = (unsigned long)(_binary_build_user_lib_libshared_so_end -
                                     _binary_build_user_lib_libshared_so_start);
        file->offset = 0UL;
        return 1;
    }

    return 0;
}

unsigned long fs_read(struct file *file, void *dst, unsigned long len)
{
    unsigned char *out;
    unsigned long remaining;
    unsigned long count;
    unsigned long index;

    if (file == (struct file *)0 || dst == (void *)0 || len == 0UL) {
        return 0UL;
    }

    if (file->offset >= file->size) {
        return 0UL;
    }

    remaining = file->size - file->offset;
    count = (len < remaining) ? len : remaining;
    out = (unsigned char *)dst;
    for (index = 0UL; index < count; index++) {
        out[index] = file->data[file->offset + index];
    }

    file->offset += count;
    return count;
}

void fs_close(struct file *file)
{
    if (file == (struct file *)0) {
        return;
    }

    file->name = (const char *)0;
    file->data = (const unsigned char *)0;
    file->size = 0UL;
    file->offset = 0UL;
}

int fs_register_file(const char *path, unsigned char *data, unsigned long size)
{
    struct dynamic_ramfs_node *node;

    if (path == (const char *)0 || data == (unsigned char *)0 || size == 0UL) {
        return 0;
    }

    node = fs_find_dynamic_node(path);
    if (node == (struct dynamic_ramfs_node *)0) {
        node = fs_find_free_dynamic_node();
        if (node == (struct dynamic_ramfs_node *)0) {
            return 0;
        }

        if (!fs_copy_path(node->path, path)) {
            return 0;
        }
    } else if (node->data != (unsigned char *)0) {
        kfree(node->data);
    }

    node->in_use = 1;
    node->data = data;
    node->size = size;
    return 1;
}

int fs_unregister_file(const char *path)
{
    struct dynamic_ramfs_node *node;

    if (path == (const char *)0) {
        return 0;
    }

    node = fs_find_dynamic_node(path);
    if (node == (struct dynamic_ramfs_node *)0) {
        return 0;
    }

    if (node->data != (unsigned char *)0) {
        kfree(node->data);
    }

    node->in_use = 0;
    node->path[0] = '\0';
    node->data = (unsigned char *)0;
    node->size = 0UL;
    return 1;
}
#else
void fs_init(void)
{
}

int fs_open(const char *path, struct file *file)
{
    (void)path;
    (void)file;
    return 0;
}

unsigned long fs_read(struct file *file, void *dst, unsigned long len)
{
    (void)file;
    (void)dst;
    (void)len;
    return 0UL;
}

void fs_close(struct file *file)
{
    (void)file;
}

int fs_register_file(const char *path, unsigned char *data, unsigned long size)
{
    (void)path;
    (void)data;
    (void)size;
    return 0;
}

int fs_unregister_file(const char *path)
{
    (void)path;
    return 0;
}
#endif