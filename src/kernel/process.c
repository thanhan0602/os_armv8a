#include <kernel/process.h>

#include <kernel/exception.h>
#include <kernel/fs.h>
#include <kernel/heap.h>
#include <kernel/log.h>
#include <kernel/mmu.h>
#include <kernel/page_alloc.h>
#include <kernel/sched.h>
#include <arch/arm/cpu.h>

extern void fork_child_exit(void);
extern int mmu_copy_to_user(const struct mm_context *mm, unsigned long dst_va, const void *src, unsigned long len);
extern int mmu_handle_process_page_fault(struct process *p, unsigned long far_el1, unsigned long esr_el1);

#ifdef CONFIG_KERNEL_VIRTUAL
#define ELF_MAGIC 0x464c457fUL
#define ELF_CLASS_64 2U
#define ELF_DATA_LE 1U
#define ELF_ET_EXEC 2U
#define ELF_ET_DYN 3U
#define ELF_EM_AARCH64 183U
#define ELF_PT_LOAD 1U
#define ELF_PT_DYNAMIC 2U
#define ELF_PT_INTERP 3U
#define ELF_PF_X 0x1U
#define ELF_PF_W 0x2U

#define PROCESS_BRK_ROLLBACK_MAX_PAGES  ((USER_HEAP_LIMIT - USER_HEAP_BASE) / PAGE_SIZE)
#define PROCESS_ELF_HEAP_GUARD_SIZE PAGE_SIZE
#define PROCESS_ELF_LOAD_STRIDE 0x10000UL
#define PROCESS_ELF_OBJECTS_MAX 8UL
#define PROCESS_ELF_NEEDED_MAX 8UL
#define PROCESS_ELF_NAME_MAX 64UL

struct elf64_ehdr {
    unsigned char e_ident[16];
    unsigned short e_type;
    unsigned short e_machine;
    unsigned int e_version;
    unsigned long e_entry;
    unsigned long e_phoff;
    unsigned long e_shoff;
    unsigned int e_flags;
    unsigned short e_ehsize;
    unsigned short e_phentsize;
    unsigned short e_phnum;
    unsigned short e_shentsize;
    unsigned short e_shnum;
    unsigned short e_shstrndx;
};

struct elf64_phdr {
    unsigned int p_type;
    unsigned int p_flags;
    unsigned long p_offset;
    unsigned long p_vaddr;
    unsigned long p_paddr;
    unsigned long p_filesz;
    unsigned long p_memsz;
    unsigned long p_align;
};

struct process_elf_object {
    char name[PROCESS_ELF_NAME_MAX];
    unsigned char *owned_image;
    const unsigned char *image;
    unsigned long image_size;
    const struct elf64_ehdr *ehdr;
    const struct elf64_phdr *phdrs;
    unsigned long phnum;
    unsigned long image_start;
    unsigned long image_end;
    unsigned long load_bias;
    unsigned long mapped_end;
};

struct process_elf_load_state {
    struct process *process;
    struct process_elf_object objects[PROCESS_ELF_OBJECTS_MAX];
    unsigned long object_count;
    unsigned long max_mapped_end;
    
    /* For Auxiliary Vector */
    unsigned long main_phdr_va;
    unsigned long main_phent;
    unsigned long main_phnum;
    unsigned long main_entry;
    unsigned long interp_load_bias;
    unsigned long interp_entry;
};

/* Auxiliary Vector Types */
#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_BASE   7
#define AT_FLAGS  8
#define AT_ENTRY  9
#define AT_EXECFN 31

static unsigned long process_next_elf_load_base = USER_CODE_BASE;

static int process_map_elf_segment(struct process *process,
                                   const unsigned char *image,
                                   unsigned long image_size,
                                   const struct elf64_phdr *phdr,
                                   unsigned long load_bias);

int process_add_region(struct process *process, 
                      unsigned long start, unsigned long end,
                      enum vm_type type, unsigned long flags,
                      const unsigned char *image, unsigned long offset,
                      unsigned long filesz);

static unsigned long process_align_up(unsigned long value, unsigned long alignment)
{
    return (value + alignment - 1UL) & ~(alignment - 1UL);
}

static unsigned long process_align_down(unsigned long value, unsigned long alignment)
{
    return value & ~(alignment - 1UL);
}

static void process_zero_bytes(void *ptr, unsigned long size)
{
    unsigned char *bytes;
    unsigned long index;

    bytes = (unsigned char *)ptr;
    for (index = 0UL; index < size; index++) {
        bytes[index] = 0U;
    }
}

static int process_streq(const char *lhs, const char *rhs)
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

static int process_copy_string(char *dst, const char *src, unsigned long max_len)
{
    unsigned long index;

    if (dst == (char *)0 || src == (const char *)0 || max_len == 0UL) {
        return 0;
    }

    for (index = 0UL; index + 1UL < max_len && src[index] != '\0'; index++) {
        dst[index] = src[index];
    }

    if (src[index] != '\0') {
        return 0;
    }

    dst[index] = '\0';
    return 1;
}

static unsigned long process_elf_page_flags(unsigned int elf_flags)
{
    unsigned long page_flags;

    page_flags = MMU_USER_PAGE_NORMAL | MMU_USER_PAGE_AF | MMU_USER_PAGE_INNER_SH;
    if ((elf_flags & ELF_PF_W) != 0U) {
        page_flags |= MMU_USER_PAGE_AP_RW | MMU_USER_PAGE_UXN | MMU_USER_PAGE_PXN;
    } else if ((elf_flags & ELF_PF_X) != 0U) {
        page_flags |= MMU_USER_PAGE_AP_RO | MMU_USER_PAGE_PXN;
    } else {
        page_flags |= MMU_USER_PAGE_AP_RO | MMU_USER_PAGE_UXN | MMU_USER_PAGE_PXN;
    }

    return page_flags;
}

static int process_is_elf_image(const unsigned char *image, unsigned long image_size)
{
    const struct elf64_ehdr *ehdr;

    if (image == (const unsigned char *)0 || image_size < sizeof(struct elf64_ehdr)) {
        return 0;
    }

    ehdr = (const struct elf64_ehdr *)image;
    return ehdr->e_ident[0] == 0x7fU &&
           ehdr->e_ident[1] == 'E' &&
           ehdr->e_ident[2] == 'L' &&
           ehdr->e_ident[3] == 'F';
}

static int process_validate_elf_header(const struct elf64_ehdr *ehdr,
                                       unsigned long image_size)
{
    if (ehdr == (const struct elf64_ehdr *)0 || image_size < sizeof(struct elf64_ehdr)) {
        return 0;
    }

    if (ehdr->e_ident[4] != ELF_CLASS_64 || ehdr->e_ident[5] != ELF_DATA_LE) {
        return 0;
    }

    if ((ehdr->e_type != ELF_ET_EXEC && ehdr->e_type != ELF_ET_DYN) ||
        ehdr->e_machine != ELF_EM_AARCH64) {
        return 0;
    }

    if (ehdr->e_phoff == 0UL || ehdr->e_phentsize != sizeof(struct elf64_phdr)) {
        return 0;
    }

    if (ehdr->e_phnum == 0U) {
        return 0;
    }

    if (ehdr->e_phoff + ((unsigned long)ehdr->e_phnum * sizeof(struct elf64_phdr)) > image_size) {
        return 0;
    }
    return 1;
}

static int process_elf_load_bounds(const struct elf64_phdr *phdrs,
                                   unsigned long phnum,
                                   unsigned long *image_start,
                                   unsigned long *image_end)
{
    unsigned long min_vaddr;
    unsigned long max_vaddr;
    unsigned long index;
    int found;

    min_vaddr = ~0UL;
    max_vaddr = 0UL;
    found = 0;
    for (index = 0UL; index < phnum; index++) {
        const struct elf64_phdr *phdr;
        unsigned long segment_end;

        phdr = &phdrs[index];
        if (phdr->p_type != ELF_PT_LOAD || phdr->p_memsz == 0UL) {
            continue;
        }

        if (phdr->p_filesz > phdr->p_memsz) {
            return 0;
        }

        segment_end = phdr->p_vaddr + phdr->p_memsz;
        if (segment_end <= phdr->p_vaddr) {
            return 0;
        }

        if (!found || phdr->p_vaddr < min_vaddr) {
            min_vaddr = phdr->p_vaddr;
        }

        if (!found || segment_end > max_vaddr) {
            max_vaddr = segment_end;
        }

        found = 1;
    }

    if (!found) {
        return 0;
    }

    *image_start = min_vaddr;
    *image_end = max_vaddr;
    return 1;
}

static unsigned long process_choose_elf_load_bias(unsigned long image_start,
                                                  unsigned long image_end)
{
    unsigned long image_span;
    unsigned long max_base;
    unsigned long base;

    image_span = process_align_up(image_end - image_start, PAGE_SIZE);
    if (image_span == 0UL || image_span >= (USER_HEAP_LIMIT - USER_CODE_BASE)) {
        return 0UL;
    }

    max_base = USER_HEAP_LIMIT - PROCESS_ELF_HEAP_GUARD_SIZE - image_span;
    if (max_base < USER_CODE_BASE) {
        return 0UL;
    }

    base = process_next_elf_load_base;
    if (base < USER_CODE_BASE || base > max_base) {
        base = USER_CODE_BASE;
    }

    process_next_elf_load_base = base + PROCESS_ELF_LOAD_STRIDE;
    if (process_next_elf_load_base > max_base) {
        process_next_elf_load_base = USER_CODE_BASE;
    }

    return base - image_start;
}

static void process_release_loaded_images(struct process_elf_load_state *state)
{
    unsigned long index;

    for (index = 0UL; index < state->object_count; index++) {
        if (state->objects[index].owned_image != (unsigned char *)0) {
            kfree(state->objects[index].owned_image);
            state->objects[index].owned_image = (unsigned char *)0;
        }
    }
}

static void process_capture_loaded_images(struct process *process, struct process_elf_load_state *state)
{
    unsigned long index;

    for (index = 0UL; index < state->object_count; index++) {
        if (state->objects[index].owned_image != (unsigned char *)0) {
            if (process->owned_image_count < 8) {
                process->owned_images[process->owned_image_count++] = state->objects[index].owned_image;
                /* Clear it from state so release_loaded_images doesn't free it */
                state->objects[index].owned_image = (unsigned char *)0;
            } else {
                /* Too many images, just free it (should not happen in this OS) */
                kfree(state->objects[index].owned_image);
                state->objects[index].owned_image = (unsigned char *)0;
            }
        }
    }
}

static int process_load_file_image(const char *path,
                                   unsigned char **image,
                                   unsigned long *image_size)
{
    struct file file;
    unsigned char *buffer;
    unsigned long file_size;
    unsigned long read_count;

    if (!fs_open(path, &file)) {
        return 0;
    }

    file_size = file.size;

    buffer = (unsigned char *)kmalloc(file_size);
    if (buffer == (unsigned char *)0) {
        fs_close(&file);
        return 0;
    }

    read_count = fs_read(&file, buffer, file_size);
    fs_close(&file);
    if (read_count != file_size) {
        kfree(buffer);
        return 0;
    }

    *image = buffer;
    *image_size = file_size;
    return 1;
}

static int process_register_loaded_object(struct process_elf_load_state *state,
                                          const char *object_name,
                                          unsigned char *owned_image,
                                          const unsigned char *image,
                                          unsigned long image_size,
                                          int set_entry)
{
    const struct elf64_ehdr *ehdr;
    struct process_elf_object *object;
    unsigned long heap_guard_base;
    unsigned long index;

    if (state->object_count >= PROCESS_ELF_OBJECTS_MAX) {
        return 0;
    }
    ehdr = (const struct elf64_ehdr *)image;
    if (!process_validate_elf_header(ehdr, image_size)) {
        return 0;
    }

    object = &state->objects[state->object_count];
    process_zero_bytes(object, sizeof(*object));
    if (!process_copy_string(object->name, object_name, PROCESS_ELF_NAME_MAX)) {
        return 0;
    }

    object->owned_image = owned_image;
    object->image = image;
    object->image_size = image_size;
    object->ehdr = ehdr;
    object->phdrs = (const struct elf64_phdr *)(image + ehdr->e_phoff);
    object->phnum = (unsigned long)ehdr->e_phnum;
    if (!process_elf_load_bounds(object->phdrs,
                                 object->phnum,
                                 &object->image_start,
                                 &object->image_end)) {
        return 0;
    }

    if (ehdr->e_type == ELF_ET_DYN) {
        object->load_bias = process_choose_elf_load_bias(object->image_start, object->image_end);
        /* If load_bias is 0 it's fine for the first object at 0x0, but we need to check if it's the 1st or not */
        if (object->load_bias == 0UL && object->image_start != 0UL && state->object_count > 0) {
            return 0;
        }
    } else {
        object->load_bias = 0UL;
        /* For static/ET_EXEC binaries, we must still move the next_base beyond this image */
        unsigned long image_span = process_align_up(object->image_end - object->image_start, PAGE_SIZE);
        if (process_next_elf_load_base < object->image_start + image_span) {
            process_next_elf_load_base = process_align_up(object->image_start + image_span, PROCESS_ELF_LOAD_STRIDE);
        }
    }

    if (state->object_count == 0UL) {
        state->main_phdr_va = object->load_bias + ehdr->e_phoff;
        state->main_phent = (unsigned long)ehdr->e_phentsize;
        state->main_phnum = (unsigned long)ehdr->e_phnum;
        state->main_entry = object->load_bias + ehdr->e_entry;
    }

    if (set_entry != 0) {
        if (ehdr->e_entry < object->image_start || ehdr->e_entry >= object->image_end) {
            return 0;
        }

        state->process->entry_va = object->load_bias + ehdr->e_entry;
        
        /* If this is NOT the first object, it's the interpreter */
        if (state->object_count > 0UL) {
            state->interp_load_bias = object->load_bias;
            state->interp_entry = state->process->entry_va;
        }
    }

    for (index = 0UL; index < object->phnum; index++) {
        if (!process_map_elf_segment(state->process,
                                     object->image,
                                     object->image_size,
                                     &object->phdrs[index],
                                     object->load_bias)) {
            return 0;
        }
    }

    object->mapped_end = process_align_up(object->load_bias + object->image_end, PAGE_SIZE);
    heap_guard_base = process_align_up(object->mapped_end + PROCESS_ELF_HEAP_GUARD_SIZE, PAGE_SIZE);
    if (heap_guard_base > USER_HEAP_LIMIT) {
        return 0;
    }

    if (object->mapped_end > state->max_mapped_end) {
        state->max_mapped_end = object->mapped_end;
    }

    state->object_count++;
    return 1;
}

int process_add_region(struct process *process, 
                      unsigned long start, unsigned long end,
                      enum vm_type type, unsigned long flags,
                      const unsigned char *image, unsigned long offset,
                      unsigned long filesz)
{
    if (process->region_count >= PROCESS_VM_REGIONS_MAX) {
        return 0;
    }

    struct vm_region *region = &process->regions[process->region_count++];
    region->start = start;
    region->end = end;
    region->type = type;
    region->flags = flags;
    region->elf_image = image;
    region->elf_offset = offset;
    region->file_size = filesz;
    return 1;
}

static int process_map_elf_segment(struct process *process,
                                   const unsigned char *image,
                                   unsigned long image_size,
                                   const struct elf64_phdr *phdr,
                                   unsigned long load_bias)
{
    unsigned long vaddr_start;
    unsigned long vaddr_end;
    unsigned long aligned_vaddr;
    unsigned long aligned_vaddr_end;
    unsigned long offset_diff;
    unsigned long segment_start;
    unsigned long segment_end;
    unsigned long segment_offset;
    unsigned long segment_filesz;
    unsigned long flags;

    if (phdr->p_type != ELF_PT_LOAD || phdr->p_memsz == 0UL) {
        return 1;
    }

    if ((phdr->p_flags & ELF_PF_W) != 0U && (phdr->p_flags & ELF_PF_X) != 0U) {
        return 0;
    }

    if (phdr->p_filesz > phdr->p_memsz || phdr->p_offset + phdr->p_filesz > image_size) {
        return 0;
    }

    vaddr_start = phdr->p_vaddr;
    vaddr_end = vaddr_start + phdr->p_memsz;
    aligned_vaddr = process_align_down(vaddr_start, PAGE_SIZE);
    aligned_vaddr_end = process_align_up(vaddr_end, PAGE_SIZE);
    
    offset_diff = vaddr_start - aligned_vaddr;
    
    segment_start = load_bias + aligned_vaddr;
    segment_end = load_bias + aligned_vaddr_end;
    segment_offset = phdr->p_offset - offset_diff;
    segment_filesz = phdr->p_filesz + offset_diff;

    if (segment_start < USER_CODE_BASE || segment_end > USER_HEAP_LIMIT || segment_end <= segment_start) {
        return 0;
    }

    flags = process_elf_page_flags(phdr->p_flags);

    return process_add_region(process, segment_start, segment_end,
                             VM_TYPE_ELF, flags,
                             image, segment_offset, segment_filesz);
}

static struct process *process_create_flat_binary(const unsigned char *code, unsigned long code_size)
{
    struct process *process;
    unsigned long code_pages;

    if (code == (const unsigned char *)0 || code_size == 0UL) {
        return (struct process *)0;
    }

    code_pages = process_align_up(code_size, PAGE_SIZE) / PAGE_SIZE;
    if ((USER_CODE_BASE + code_pages * PAGE_SIZE) > USER_HEAP_BASE) {
        KER_INFO("process image too large for user layout");
        return (struct process *)0;
    }

    process = (struct process *)kmalloc(sizeof(struct process));
    if (process == (struct process *)0) {
        return (struct process *)0;
    }

    process->mm = mmu_context_create();
    if (process->mm == (struct mm_context *)0) {
        kfree(process);
        return (struct process *)0;
    }

    process->entry_va = USER_CODE_BASE;
    process->stack_top = USER_STACK_TOP;
    process->heap_start = USER_HEAP_BASE;
    process->brk = USER_HEAP_BASE;
    process->heap_limit = USER_HEAP_LIMIT;
    process->heap_mapped_end = USER_HEAP_BASE;
    process->region_count = 0U;
    process->owned_image_count = 0U;

    /* Register Code region */
    process_add_region(process, USER_CODE_BASE, USER_CODE_BASE + (code_pages * PAGE_SIZE),
                      VM_TYPE_ELF,
                      MMU_USER_PAGE_NORMAL | MMU_USER_PAGE_AF |
                      MMU_USER_PAGE_INNER_SH | MMU_USER_PAGE_AP_RO |
                      MMU_USER_PAGE_PXN,
                      code, 0, code_size);

    /* Register Stack region */
    process_add_region(process, USER_STACK_TOP - 0x100000, USER_STACK_TOP,
                      VM_TYPE_ANON,
                      MMU_USER_PAGE_NORMAL | MMU_USER_PAGE_AF |
                      MMU_USER_PAGE_INNER_SH | MMU_USER_PAGE_AP_RW |
                      MMU_USER_PAGE_UXN | MMU_USER_PAGE_PXN,
                      (unsigned char *)0, 0, 0);

    /* Register Heap region */
    process_add_region(process, USER_HEAP_BASE, USER_HEAP_LIMIT,
                      VM_TYPE_ANON,
                      MMU_USER_PAGE_NORMAL | MMU_USER_PAGE_AF |
                      MMU_USER_PAGE_INNER_SH | MMU_USER_PAGE_AP_RW |
                      MMU_USER_PAGE_UXN | MMU_USER_PAGE_PXN,
                      (unsigned char *)0, 0, 0);

    cpu_invalidate_icache_all();

    return process;
}

struct process *process_create_from_buffer(const unsigned char *code, unsigned long code_size)
{
    if (process_is_elf_image(code, code_size)) {
        return process_create_from_elf(code, code_size, (const char *)0);
    }

    return process_create_flat_binary(code, code_size);
}

static void process_populate_arg_stack(struct process *process, const char *path, const struct process_elf_load_state *state)
{
    unsigned long stack_page_va;
    unsigned char kstack[1024];
    unsigned long ksp = 1024;
    unsigned long path_len = 0;
    unsigned long path_va;
    unsigned long user_sp;

    /* Write to the top-most page of the stack. Ensure it's mapped. */
    stack_page_va = (process->stack_top - 1) & ~0xFFFUL;
    if (!mmu_handle_process_page_fault(process, stack_page_va, 0x90000004UL)) {
        return;
    }

    /* 1. Copy strings to top of stack */
    if (path) {
        while (path[path_len] != '\0') path_len++;
    }
    ksp -= (path_len + 1);
    path_va = process->stack_top - (1024UL - ksp);
    if (path) {
        for (unsigned long i = 0UL; i <= path_len; i++) kstack[ksp + i] = (unsigned char)path[i];
    } else {
        kstack[ksp] = 0U;
    }

    const char *env_str = "LD_LIBRARY_PATH=/lib";
    unsigned long env_len = 0;
    while (env_str[env_len]) env_len++;
    ksp -= (env_len + 1);
    unsigned long env_va = process->stack_top - (1024UL - ksp);
    for (unsigned long i = 0UL; i <= env_len; i++) kstack[ksp + i] = (unsigned char)env_str[i];

    /* Align ksp for pointers */
    ksp &= ~15UL;
    
    /* 2. Auxv (End with AT_NULL) */
    ksp -= 2UL * sizeof(unsigned long); /* AT_NULL */
    ((unsigned long *)(kstack + ksp))[0] = (unsigned long)0; // AT_NULL
    ((unsigned long *)(kstack + ksp))[1] = 0UL;
    
    ksp -= 2UL * sizeof(unsigned long); /* AT_PAGESZ */
    ((unsigned long *)(kstack + ksp))[0] = (unsigned long)AT_PAGESZ;
    ((unsigned long *)(kstack + ksp))[1] = 4096UL;

    ksp -= 2UL * sizeof(unsigned long); /* AT_EXECFN */
    ((unsigned long *)(kstack + ksp))[0] = (unsigned long)AT_EXECFN;
    ((unsigned long *)(kstack + ksp))[1] = path_va;

    ksp -= 2UL * sizeof(unsigned long); /* AT_PHDR */
    ((unsigned long *)(kstack + ksp))[0] = (unsigned long)AT_PHDR;
    ((unsigned long *)(kstack + ksp))[1] = state->main_phdr_va;

    ksp -= 2UL * sizeof(unsigned long); /* AT_PHNUM */
    ((unsigned long *)(kstack + ksp))[0] = (unsigned long)AT_PHNUM;
    ((unsigned long *)(kstack + ksp))[1] = state->main_phnum;

    ksp -= 2UL * sizeof(unsigned long); /* AT_PHENT */
    ((unsigned long *)(kstack + ksp))[0] = (unsigned long)AT_PHENT;
    ((unsigned long *)(kstack + ksp))[1] = state->main_phent;

    ksp -= 2UL * sizeof(unsigned long); /* AT_BASE */
    ((unsigned long *)(kstack + ksp))[0] = (unsigned long)AT_BASE;
    ((unsigned long *)(kstack + ksp))[1] = state->interp_load_bias;

    ksp -= 2UL * sizeof(unsigned long); /* AT_ENTRY */
    ((unsigned long *)(kstack + ksp))[0] = (unsigned long)AT_ENTRY;
    ((unsigned long *)(kstack + ksp))[1] = state->main_entry;

    /* 3. Envp (End with NULL) */
    ksp -= sizeof(unsigned long); /* NULL terminator */
    ((unsigned long *)(kstack + ksp))[0] = 0UL;
    ksp -= sizeof(unsigned long); /* envp[0] */
    ((unsigned long *)(kstack + ksp))[0] = env_va;

    /* 4. Argv (End with NULL) */
    ksp -= sizeof(unsigned long); /* NULL terminator */
    ((unsigned long *)(kstack + ksp))[0] = 0UL;
    ksp -= sizeof(unsigned long); /* argv[0] */
    ((unsigned long *)(kstack + ksp))[0] = path_va;

    /* 5. Argc */
    ksp -= sizeof(unsigned long);
    ((unsigned long *)(kstack + ksp))[0] = 1UL; // argc = 1

    /* Align SP to 16-byte boundary as required by AArch64 ABI */
    ksp &= ~15UL;
    
    /* Copy to user stack */
    user_sp = process->stack_top - (1024UL - ksp);
    mmu_copy_to_user(process->mm, user_sp, kstack + ksp, 1024UL - ksp);
    
    process->stack_top = user_sp;
}

struct process *process_create_from_elf(const unsigned char *image, unsigned long image_size, const char *path)
{
    struct process *process;
    struct process_elf_load_state load_state;
    unsigned long index;
    unsigned long heap_start;
    const struct elf64_ehdr *ehdr;
    const struct elf64_phdr *phdrs;
    const char *interp_path = (const char *)0;

    if (!process_is_elf_image(image, image_size)) {
        return (struct process *)0;
    }

    ehdr = (const struct elf64_ehdr *)image;
    if (!process_validate_elf_header(ehdr, image_size)) {
        KER_INFO("invalid ELF image");
        return (struct process *)0;
    }

    /* Check for PT_INTERP */
    phdrs = (const struct elf64_phdr *)(image + ehdr->e_phoff);
    for (index = 0; index < (unsigned long)ehdr->e_phnum; index++) {
        if (phdrs[index].p_type == ELF_PT_INTERP) {
            if (phdrs[index].p_offset + phdrs[index].p_filesz <= image_size) {
                interp_path = (const char *)(image + phdrs[index].p_offset);
            }
            break;
        }
    }

    process = (struct process *)kmalloc(sizeof(struct process));
    if (process == (struct process *)0) {
        return (struct process *)0;
    }

    process->mm = mmu_context_create();
    if (process->mm == (struct mm_context *)0) {
        kfree(process);
        return (struct process *)0;
    }

    process_zero_bytes(&load_state, sizeof(load_state));
    load_state.process = process;

    /* Reset load base for PIE binaries in this process */
    process_next_elf_load_base = USER_CODE_BASE;

    process->entry_va = 0UL;
    process->stack_top = USER_STACK_TOP;
    process->heap_start = 0UL;
    process->brk = 0UL;
    process->heap_limit = USER_HEAP_LIMIT;
    process->heap_mapped_end = 0UL;
    process->region_count = 0U;
    process->owned_image_count = 0U;
    
    // Copy path for later use
    if (path) {
        process_copy_string(process->name, path, PROCESS_ELF_NAME_MAX);
    }

    if (!process_register_loaded_object(&load_state, "<main>", (unsigned char *)image, image, image_size, (interp_path == (const char *)0))) {
        process_destroy(process);
        return (struct process *)0;
    }

    /* Handle Interpreter if present */
    if (interp_path != (const char *)0) {
        unsigned char *interp_image;
        unsigned long interp_size;
        
        /* For this OS, we usually expect /lib/ld.so */
        if (process_load_file_image(interp_path, &interp_image, &interp_size)) {
            if (!process_register_loaded_object(&load_state, interp_path, interp_image, interp_image, interp_size, 1)) {
                kfree(interp_image);
                process_destroy(process);
                return (struct process *)0;
            }
        } else {
            KER_INFO("failed to load interpreter");
            process_destroy(process);
            return (struct process *)0;
        }
    }

    heap_start = process_align_up(load_state.max_mapped_end + PROCESS_ELF_HEAP_GUARD_SIZE, PAGE_SIZE);
    if (heap_start > USER_HEAP_LIMIT) {
        process_release_loaded_images(&load_state);
        process_destroy(process);
        return (struct process *)0;
    }

    process->heap_start = heap_start;
    process->brk = heap_start;
    process->heap_limit = USER_HEAP_LIMIT;
    process->heap_mapped_end = heap_start;

    /* Register regions for Lazy Loading */
    /* Stack region: 1MB below USER_STACK_TOP */
    process_add_region(process, USER_STACK_TOP - 0x100000, USER_STACK_TOP,
                      VM_TYPE_ANON,
                      MMU_USER_PAGE_NORMAL | MMU_USER_PAGE_AF |
                      MMU_USER_PAGE_INNER_SH | MMU_USER_PAGE_AP_RW |
                      MMU_USER_PAGE_UXN | MMU_USER_PAGE_PXN,
                      (unsigned char *)0, 0, 0);

    /* Heap region: from heap_start to USER_HEAP_LIMIT */
    process_add_region(process, heap_start, USER_HEAP_LIMIT,
                      VM_TYPE_ANON,
                      MMU_USER_PAGE_NORMAL | MMU_USER_PAGE_AF |
                      MMU_USER_PAGE_INNER_SH | MMU_USER_PAGE_AP_RW |
                      MMU_USER_PAGE_UXN | MMU_USER_PAGE_PXN,
                      (unsigned char *)0, 0, 0);

    /* Initialize user stack with arguments and auxv */
    process_populate_arg_stack(process, path, &load_state);

    cpu_invalidate_icache_all();

    process_capture_loaded_images(process, &load_state);
    process_release_loaded_images(&load_state);

    return process;
}

struct process *process_create_from_image(char *code_start, char *code_end)
{
    if (code_start == (char *)0 || code_end == (char *)0 || code_end <= code_start) {
        return (struct process *)0;
    }

    return process_create_from_buffer((const unsigned char *)code_start,
                                      (unsigned long)(code_end - code_start));
}

void process_destroy(struct process *process)
{
    if (process == (struct process *)0) {
        return;
    }

    if (process->mm != (struct mm_context *)0) {
        mmu_context_destroy(process->mm);
        process->mm = (struct mm_context *)0;
    }

    /* Free owned ELF images */
    for (unsigned int i = 0; i < process->owned_image_count; i++) {
        if (process->owned_images[i] != (unsigned char *)0) {
            kfree(process->owned_images[i]);
            process->owned_images[i] = (unsigned char *)0;
        }
    }

    kfree(process);
}

unsigned long process_brk(struct process *process, unsigned long new_break)
{
    if (process == (struct process *)0) {
        return 0UL;
    }

    if (new_break == 0UL) {
        return process->brk;
    }

    if (new_break < process->heap_start || new_break > process->heap_limit) {
        return process->brk;
    }

    process->brk = new_break;
    return process->brk;
}

unsigned long process_fork(struct exception_context *ctx)
{
    struct task *parent_task = sched_current();
    if (!parent_task) return (unsigned long)-1;

    /* 1. Allocate process structure for child */
    struct process *child_proc = (struct process *)kmalloc(sizeof(struct process));
    if (!child_proc) return (unsigned long)-1;

    /* 2. Copy process state */
    child_proc->entry_va = parent_task->process->entry_va;
    child_proc->stack_top = parent_task->process->stack_top;
    child_proc->heap_start = parent_task->process->heap_start;
    child_proc->brk = parent_task->process->brk;
    child_proc->heap_limit = parent_task->process->heap_limit;
    child_proc->heap_mapped_end = parent_task->process->heap_mapped_end;
    child_proc->region_count = parent_task->process->region_count;
    for (unsigned int i = 0; i < parent_task->process->region_count; i++) {
        child_proc->regions[i] = parent_task->process->regions[i];
    }

    /* 3. Clone MMU context (CoW happens here internally) */
    child_proc->mm = mmu_context_clone(parent_task->mm);
    if (!child_proc->mm) {
        kfree(child_proc);
        return (unsigned long)-1;
    }

    /* 4. Create new task (allocates slot and kernel stack) */
    struct task *child_task = task_create_user(child_proc, parent_task->name);
    if (child_task) {
        child_task->parent_id = parent_task->id;
    }

    /* 5. Copy kernel stack content to clone the exception context and call stack */
    /* Find the offset of the exception context from the parent's stack base */
    unsigned long stack_offset = (unsigned long)ctx - (unsigned long)parent_task->stack_base;
    
    /* Copy the entire kernel stack */
    unsigned char *src_stack = (unsigned char *)parent_task->stack_base;
    unsigned char *dst_stack = (unsigned char *)child_task->stack_base;
    for (unsigned long i = 0; i < child_task->stack_size; i++) {
        dst_stack[i] = src_stack[i];
    }

    /* 6. Adjust child's kernel SP and return address */
    /* The child should resume at fork_child_exit, which pops the context
     * from its own stack and eret to user mode. */
    child_task->context.x30 = (unsigned long)fork_child_exit;
    child_task->context.sp = (unsigned long)child_task->stack_base + stack_offset;

    /* 7. Fix the child's return value in its cloned exception context */
    struct exception_context *child_ctx = (struct exception_context *)(dst_stack + stack_offset);
    child_ctx->gpr[0] = 0;

    sched_wake_task(child_task);

    KER_LOGF("[process] fork: parent PID=%lu, child PID=%lu, offset=0x%lx, ctx=0x%lx\n", 
             parent_task->id, child_task->id, stack_offset, (unsigned long)ctx);
    KER_LOGF("          child_elr=0x%lx, child_spsr=0x%lx, child_sp_el0=0x%lx\n",
             child_ctx->elr_el1, child_ctx->spsr_el1, child_ctx->sp_el0);

    /* 8. Parent returns child PID */
    return (unsigned long)child_task->id;
}
#else
struct process *process_create_from_image(char *code_start, char *code_end)
{
    (void)code_start;
    (void)code_end;
    return (struct process *)0;
}

void process_destroy(struct process *process)
{
    (void)process;
}

unsigned long process_brk(struct process *process, unsigned long new_break)
{
    (void)process;
    (void)new_break;
    return 0UL;
}
#endif