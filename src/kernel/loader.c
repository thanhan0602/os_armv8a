#include <kernel/loader.h>

#include <kernel/fs.h>
#include <kernel/heap.h>
#include <kernel/process.h>

struct process *loader_load_process_image(const char *path)
{
    struct file file;
    unsigned char *buffer;
    struct process *process;
    unsigned long image_size;
    unsigned long read_count;

    if (!fs_open(path, &file)) {
        return (struct process *)0;
    }

    image_size = file.size;
    buffer = (unsigned char *)kmalloc(image_size);
    if (buffer == (unsigned char *)0) {
        fs_close(&file);
        return (struct process *)0;
    }

    read_count = fs_read(&file, buffer, image_size);
    fs_close(&file);
    if (read_count != image_size) {
        kfree(buffer);
        return (struct process *)0;
    }

    process = process_create_from_elf(buffer, image_size);
    /* Note: We do NOT kfree(buffer) here because the process regions 
     * use it for lazy loading from the ELF image. 
     * The process takes ownership of this buffer. */
    return process;
}