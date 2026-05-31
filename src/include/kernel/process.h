#ifndef KERNEL_PROCESS_H
#define KERNEL_PROCESS_H

#include <kernel/vm.h>

struct mm_context;

struct process {
    struct mm_context *mm;
    unsigned long entry_va;
    unsigned long stack_top;
    unsigned long heap_start;
    unsigned long brk;
    unsigned long heap_limit;
    unsigned long heap_mapped_end;
};

struct process *process_create_from_buffer(const unsigned char *code, unsigned long code_size);
struct process *process_create_from_elf(const unsigned char *image, unsigned long image_size);
struct process *process_create_from_image(char *code_start, char *code_end);
void process_destroy(struct process *process);
unsigned long process_brk(struct process *process, unsigned long new_break);

#endif