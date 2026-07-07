#ifndef KERNEL_PROCESS_H
#define KERNEL_PROCESS_H

#include <kernel/vm.h>

#define PROCESS_VM_REGIONS_MAX 16

enum vm_type {
    VM_TYPE_ANON,
    VM_TYPE_ELF,
    VM_TYPE_HEAP,
};

struct vm_region {
    unsigned long start;
    unsigned long end;
    enum vm_type type;
    unsigned long flags;
    const unsigned char *elf_image;
    unsigned long elf_offset;
    unsigned long file_size;
};

struct mm_context;
struct exception_context;

struct process {
    char name[64];
    struct mm_context *mm;
    unsigned long entry_va;
    unsigned long stack_top;
    unsigned long heap_start;
    unsigned long brk;
    unsigned long heap_limit;
    unsigned long heap_mapped_end;

    struct vm_region regions[PROCESS_VM_REGIONS_MAX];
    unsigned int region_count;

    unsigned char *owned_images[8];
    unsigned int owned_image_count;
};

struct process *process_create_from_buffer(const unsigned char *code, unsigned long code_size);
struct process *process_create_from_elf(const unsigned char *image, unsigned long image_size, const char *path);
struct process *process_create_from_image(char *code_start, char *code_end);
void process_destroy(struct process *process);
unsigned long process_brk(struct process *process, unsigned long new_break);
unsigned long process_fork(struct exception_context *ctx);
int process_add_region(struct process *process, 
                      unsigned long start, unsigned long end,
                      enum vm_type type, unsigned long flags,
                      const unsigned char *image, unsigned long offset,
                      unsigned long filesz);

#endif