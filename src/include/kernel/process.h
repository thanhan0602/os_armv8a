#ifndef KERNEL_PROCESS_H
#define KERNEL_PROCESS_H

#include <kernel/vm.h>
#include <kernel/spinlock.h>

#define PROCESS_VM_REGIONS_MAX 16

enum vm_type {
    VM_TYPE_ANON,
    VM_TYPE_ELF,
    VM_TYPE_HEAP,
};

/* Clone flags (subset of Linux) */
#define CLONE_VM        0x00000100
#define CLONE_FS        0x00000200
#define CLONE_FILES     0x00000400
#define CLONE_SIGHAND   0x00000800
#define CLONE_THREAD    0x00010000
#define CLONE_SETTLS    0x00080000

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

    unsigned long user_sp;
    struct spinlock lock;
    unsigned int ref_count;   /* number of tasks (threads) sharing this process */
    unsigned int exec_in_progress; /* blocks new sibling threads during execve */
};

struct process *process_create_from_image(char *code_start, char *code_end);
struct process *process_create_from_elf(const unsigned char *image, unsigned long image_size, const char *path);
void process_destroy(struct process *process);
unsigned long process_brk(struct process *process, unsigned long new_break);
unsigned long process_clone(unsigned long flags, unsigned long stack, unsigned long tls, struct exception_context *ctx);
unsigned long process_fork(struct exception_context *ctx);
unsigned long process_thread_create(struct exception_context *ctx);
unsigned long process_execve(const char *filename, char *const argv[], char *const envp[], struct exception_context *ctx);
int process_add_region(struct process *process,
                      unsigned long start, unsigned long end,
                      enum vm_type type, unsigned long flags,
                      const unsigned char *image, unsigned long offset,
                      unsigned long filesz);

#endif