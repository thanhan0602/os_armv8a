#include <kernel/process.h>

#include <kernel/fs.h>
#include <kernel/heap.h>
#include <kernel/log.h>
#include <kernel/mmu.h>
#include <kernel/page_alloc.h>

#ifdef CONFIG_KERNEL_VIRTUAL
#define ELF_MAGIC 0x464c457fUL
#define ELF_CLASS_64 2U
#define ELF_DATA_LE 1U
#define ELF_ET_EXEC 2U
#define ELF_ET_DYN 3U
#define ELF_EM_AARCH64 183U
#define ELF_PT_LOAD 1U
#define ELF_PT_DYNAMIC 2U
#define ELF_PF_X 0x1U
#define ELF_PF_W 0x2U

#define ELF_DT_NEEDED 1L
#define ELF_DT_PLTRELSZ 2L
#define ELF_DT_PLTGOT 3L
#define ELF_DT_NULL 0L
#define ELF_DT_STRTAB 5L
#define ELF_DT_SYMTAB 6L
#define ELF_DT_RELA 7L
#define ELF_DT_RELASZ 8L
#define ELF_DT_RELAENT 9L
#define ELF_DT_STRSZ 10L
#define ELF_DT_SYMENT 11L
#define ELF_DT_SONAME 14L
#define ELF_DT_PLTREL 20L
#define ELF_DT_JMPREL 23L

#define ELF_R_AARCH64_ABS64 257U
#define ELF_R_AARCH64_GLOB_DAT 1025U
#define ELF_R_AARCH64_JUMP_SLOT 1026U
#define ELF_R_AARCH64_RELATIVE 1027U

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

struct elf64_dyn {
    long d_tag;
    union {
        unsigned long d_val;
        unsigned long d_ptr;
    } d_un;
};

struct elf64_rela {
    unsigned long r_offset;
    unsigned long r_info;
    long r_addend;
};

struct elf64_sym {
    unsigned int st_name;
    unsigned char st_info;
    unsigned char st_other;
    unsigned short st_shndx;
    unsigned long st_value;
    unsigned long st_size;
};

struct process_elf_dynamic_info {
    const char *strtab;
    unsigned long strtab_size;
    const struct elf64_sym *symtab;
    unsigned long symtab_count;
    const struct elf64_rela *rela_entries;
    unsigned long rela_count;
    const struct elf64_rela *jmprel_entries;
    unsigned long jmprel_count;
    unsigned long needed_offsets[PROCESS_ELF_NEEDED_MAX];
    unsigned long needed_count;
    unsigned long soname_offset;
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
    struct process_elf_dynamic_info dynamic;
};

struct process_elf_load_state {
    struct process *process;
    struct process_elf_object objects[PROCESS_ELF_OBJECTS_MAX];
    unsigned long object_count;
    unsigned long max_mapped_end;
};

static unsigned long process_next_elf_load_base = USER_CODE_BASE;

static int process_map_elf_segment(struct process *process,
                                   const unsigned char *image,
                                   unsigned long image_size,
                                   const struct elf64_phdr *phdr,
                                   unsigned long load_bias);

static unsigned long process_align_up(unsigned long value, unsigned long alignment)
{
    return (value + alignment - 1UL) & ~(alignment - 1UL);
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

static int process_make_library_path(char *dst, const char *name)
{
    static const char prefix[] = "/lib/";
    unsigned long out_index;
    unsigned long index;

    if (dst == (char *)0 || name == (const char *)0) {
        return 0;
    }

    if (name[0] == '/') {
        return process_copy_string(dst, name, PROCESS_ELF_NAME_MAX);
    }

    out_index = 0UL;
    for (index = 0UL; prefix[index] != '\0'; index++) {
        if (out_index + 1UL >= PROCESS_ELF_NAME_MAX) {
            return 0;
        }

        dst[out_index++] = prefix[index];
    }

    for (index = 0UL; name[index] != '\0'; index++) {
        if (out_index + 1UL >= PROCESS_ELF_NAME_MAX) {
            return 0;
        }

        dst[out_index++] = name[index];
    }

    dst[out_index] = '\0';
    return 1;
}

static void process_copy_image_page(unsigned long page_pa,
                                    const unsigned char *source,
                                    unsigned long byte_count)
{
    unsigned char *page_va;
    unsigned long index;

    page_va = (unsigned char *)pa_to_va((void *)page_pa);
    for (index = 0UL; index < byte_count; index++) {
        page_va[index] = source[index];
    }
}

static void process_copy_image_page_region(unsigned long page_pa,
                                           unsigned long page_offset,
                                           const unsigned char *source,
                                           unsigned long byte_count)
{
    unsigned char *page_va;
    unsigned long index;

    page_va = (unsigned char *)pa_to_va((void *)page_pa);
    for (index = 0UL; index < byte_count; index++) {
        page_va[page_offset + index] = source[index];
    }
}

static int process_map_page(struct process *process,
                            unsigned long va,
                            unsigned long pa,
                            unsigned long flags)
{
    if (!mmu_context_add_page(process->mm, pa)) {
        return 0;
    }

    if (!mmu_map_user_page(process->mm, va, pa, flags)) {
        mmu_context_remove_page(process->mm, pa);
        return 0;
    }

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

static unsigned int process_elf_relocation_type(unsigned long r_info)
{
    return (unsigned int)(r_info & 0xffffffffUL);
}

static unsigned long process_elf_relocation_symbol(unsigned long r_info)
{
    return r_info >> 32;
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

static const unsigned char *process_elf_image_ptr(const unsigned char *image,
                                                  unsigned long image_size,
                                                  const struct elf64_phdr *phdrs,
                                                  unsigned long phnum,
                                                  unsigned long vaddr,
                                                  unsigned long size)
{
    unsigned long index;

    for (index = 0UL; index < phnum; index++) {
        const struct elf64_phdr *phdr;

        phdr = &phdrs[index];
        if (phdr->p_type != ELF_PT_LOAD || phdr->p_filesz == 0UL) {
            continue;
        }

        if (vaddr < phdr->p_vaddr || vaddr > (phdr->p_vaddr + phdr->p_filesz)) {
            continue;
        }

        if (size > (phdr->p_vaddr + phdr->p_filesz - vaddr)) {
            continue;
        }

        if (phdr->p_offset + (vaddr - phdr->p_vaddr) + size > image_size) {
            return (const unsigned char *)0;
        }

        return image + phdr->p_offset + (vaddr - phdr->p_vaddr);
    }

    return (const unsigned char *)0;
}

static int process_write_user_u64(struct process *process,
                                  unsigned long va,
                                  unsigned long value)
{
    unsigned long page_va;
    unsigned long page_pa;
    unsigned long page_offset;
    unsigned char *page_ptr;
    unsigned int index;

    page_va = va & ~(PAGE_SIZE - 1UL);
    page_offset = va & (PAGE_SIZE - 1UL);
    if (page_offset + sizeof(unsigned long) > PAGE_SIZE) {
        return 0;
    }

    if (!mmu_user_page_pa(process->mm, page_va, &page_pa)) {
        return 0;
    }

    page_ptr = (unsigned char *)pa_to_va((void *)page_pa) + page_offset;
    for (index = 0U; index < sizeof(unsigned long); index++) {
        page_ptr[index] = (unsigned char)((value >> (index * 8U)) & 0xffUL);
    }

    return 1;
}

static const char *process_object_dynamic_string(const struct process_elf_object *object,
                                                 unsigned long offset)
{
    unsigned long index;

    if (object == (const struct process_elf_object *)0 ||
        object->dynamic.strtab == (const char *)0 ||
        offset >= object->dynamic.strtab_size) {
        return (const char *)0;
    }

    for (index = offset; index < object->dynamic.strtab_size; index++) {
        if (object->dynamic.strtab[index] == '\0') {
            return object->dynamic.strtab + offset;
        }
    }

    return (const char *)0;
}

static void process_init_dynamic_info(struct process_elf_dynamic_info *dynamic_info)
{
    unsigned long index;

    dynamic_info->strtab = (const char *)0;
    dynamic_info->strtab_size = 0UL;
    dynamic_info->symtab = (const struct elf64_sym *)0;
    dynamic_info->symtab_count = 0UL;
    dynamic_info->rela_entries = (const struct elf64_rela *)0;
    dynamic_info->rela_count = 0UL;
    dynamic_info->jmprel_entries = (const struct elf64_rela *)0;
    dynamic_info->jmprel_count = 0UL;
    dynamic_info->needed_count = 0UL;
    dynamic_info->soname_offset = ~0UL;
    for (index = 0UL; index < PROCESS_ELF_NEEDED_MAX; index++) {
        dynamic_info->needed_offsets[index] = 0UL;
    }
}

static int process_parse_dynamic_info(struct process_elf_object *object)
{
    const struct elf64_dyn *dyn_entries;
    const unsigned char *strtab_bytes;
    const unsigned char *symtab_bytes;
    const unsigned char *rela_bytes;
    const unsigned char *jmprel_bytes;
    unsigned long dyn_count;
    unsigned long strtab_addr;
    unsigned long symtab_addr;
    unsigned long rela_addr;
    unsigned long rela_size;
    unsigned long rela_ent;
    unsigned long jmprel_addr;
    unsigned long jmprel_size;
    unsigned long plt_rel_type;
    unsigned long sym_ent;
    unsigned long index;

    process_init_dynamic_info(&object->dynamic);

    dyn_entries = (const struct elf64_dyn *)0;
    dyn_count = 0UL;
    for (index = 0UL; index < object->phnum; index++) {
        if (object->phdrs[index].p_type != ELF_PT_DYNAMIC) {
            continue;
        }

        if (object->phdrs[index].p_offset + object->phdrs[index].p_filesz > object->image_size ||
            object->phdrs[index].p_filesz < sizeof(struct elf64_dyn)) {
            return 0;
        }

        dyn_entries = (const struct elf64_dyn *)(object->image + object->phdrs[index].p_offset);
        dyn_count = object->phdrs[index].p_filesz / sizeof(struct elf64_dyn);
        break;
    }

    if (dyn_entries == (const struct elf64_dyn *)0) {
        return 1;
    }

    strtab_addr = 0UL;
    symtab_addr = 0UL;
    rela_addr = 0UL;
    rela_size = 0UL;
    rela_ent = sizeof(struct elf64_rela);
    jmprel_addr = 0UL;
    jmprel_size = 0UL;
    plt_rel_type = 0UL;
    sym_ent = sizeof(struct elf64_sym);
    for (index = 0UL; index < dyn_count; index++) {
        if (dyn_entries[index].d_tag == ELF_DT_NULL) {
            break;
        }

        if (dyn_entries[index].d_tag == ELF_DT_NEEDED) {
            if (object->dynamic.needed_count >= PROCESS_ELF_NEEDED_MAX) {
                return 0;
            }

            object->dynamic.needed_offsets[object->dynamic.needed_count++] =
                dyn_entries[index].d_un.d_val;
        } else if (dyn_entries[index].d_tag == ELF_DT_STRTAB) {
            strtab_addr = dyn_entries[index].d_un.d_ptr;
        } else if (dyn_entries[index].d_tag == ELF_DT_SYMTAB) {
            symtab_addr = dyn_entries[index].d_un.d_ptr;
        } else if (dyn_entries[index].d_tag == ELF_DT_RELA) {
            rela_addr = dyn_entries[index].d_un.d_ptr;
        } else if (dyn_entries[index].d_tag == ELF_DT_RELASZ) {
            rela_size = dyn_entries[index].d_un.d_val;
        } else if (dyn_entries[index].d_tag == ELF_DT_RELAENT) {
            rela_ent = dyn_entries[index].d_un.d_val;
        } else if (dyn_entries[index].d_tag == ELF_DT_STRSZ) {
            object->dynamic.strtab_size = dyn_entries[index].d_un.d_val;
        } else if (dyn_entries[index].d_tag == ELF_DT_SYMENT) {
            sym_ent = dyn_entries[index].d_un.d_val;
        } else if (dyn_entries[index].d_tag == ELF_DT_SONAME) {
            object->dynamic.soname_offset = dyn_entries[index].d_un.d_val;
        } else if (dyn_entries[index].d_tag == ELF_DT_JMPREL) {
            jmprel_addr = dyn_entries[index].d_un.d_ptr;
        } else if (dyn_entries[index].d_tag == ELF_DT_PLTRELSZ) {
            jmprel_size = dyn_entries[index].d_un.d_val;
        } else if (dyn_entries[index].d_tag == ELF_DT_PLTREL) {
            plt_rel_type = dyn_entries[index].d_un.d_val;
        }
    }

    if (strtab_addr != 0UL && object->dynamic.strtab_size != 0UL) {
        strtab_bytes = process_elf_image_ptr(object->image,
                                             object->image_size,
                                             object->phdrs,
                                             object->phnum,
                                             strtab_addr,
                                             object->dynamic.strtab_size);
        if (strtab_bytes == (const unsigned char *)0) {
            return 0;
        }

        object->dynamic.strtab = (const char *)strtab_bytes;
    }

    if (symtab_addr != 0UL) {
        if (object->dynamic.strtab == (const char *)0 || sym_ent != sizeof(struct elf64_sym) ||
            object->dynamic.strtab_size == 0UL || strtab_addr <= symtab_addr) {
            return 0;
        }

        symtab_bytes = process_elf_image_ptr(object->image,
                                             object->image_size,
                                             object->phdrs,
                                             object->phnum,
                                             symtab_addr,
                                             strtab_addr - symtab_addr);
        if (symtab_bytes == (const unsigned char *)0) {
            return 0;
        }

        object->dynamic.symtab = (const struct elf64_sym *)symtab_bytes;
        object->dynamic.symtab_count = (strtab_addr - symtab_addr) / sizeof(struct elf64_sym);
    }

    if (rela_size != 0UL) {
        if (rela_addr == 0UL || rela_ent != sizeof(struct elf64_rela) ||
            (rela_size % sizeof(struct elf64_rela)) != 0UL) {
            return 0;
        }

        rela_bytes = process_elf_image_ptr(object->image,
                                           object->image_size,
                                           object->phdrs,
                                           object->phnum,
                                           rela_addr,
                                           rela_size);
        if (rela_bytes == (const unsigned char *)0) {
            return 0;
        }

        object->dynamic.rela_entries = (const struct elf64_rela *)rela_bytes;
        object->dynamic.rela_count = rela_size / sizeof(struct elf64_rela);
    }

    if (jmprel_size != 0UL) {
        if (jmprel_addr == 0UL || plt_rel_type != ELF_DT_RELA ||
            (jmprel_size % sizeof(struct elf64_rela)) != 0UL) {
            return 0;
        }

        jmprel_bytes = process_elf_image_ptr(object->image,
                                             object->image_size,
                                             object->phdrs,
                                             object->phnum,
                                             jmprel_addr,
                                             jmprel_size);
        if (jmprel_bytes == (const unsigned char *)0) {
            return 0;
        }

        object->dynamic.jmprel_entries = (const struct elf64_rela *)jmprel_bytes;
        object->dynamic.jmprel_count = jmprel_size / sizeof(struct elf64_rela);
    }

    return 1;
}

static int process_object_defined_symbol(const struct process_elf_object *object,
                                         unsigned long sym_index,
                                         unsigned long *value)
{
    const struct elf64_sym *symbol;

    if (object == (const struct process_elf_object *)0 ||
        object->dynamic.symtab == (const struct elf64_sym *)0 ||
        sym_index >= object->dynamic.symtab_count) {
        return 0;
    }

    symbol = &object->dynamic.symtab[sym_index];
    if (symbol->st_shndx == 0U) {
        return 0;
    }

    *value = object->load_bias + symbol->st_value;
    return 1;
}

static int process_resolve_symbol_name(const struct process_elf_load_state *state,
                                       const char *symbol_name,
                                       unsigned long *value)
{
    unsigned long object_index;

    if (symbol_name == (const char *)0 || symbol_name[0] == '\0') {
        return 0;
    }

    for (object_index = 0UL; object_index < state->object_count; object_index++) {
        const struct process_elf_object *object;
        unsigned long sym_index;

        object = &state->objects[object_index];
        if (object->dynamic.symtab == (const struct elf64_sym *)0 ||
            object->dynamic.strtab == (const char *)0) {
            continue;
        }

        for (sym_index = 1UL; sym_index < object->dynamic.symtab_count; sym_index++) {
            const struct elf64_sym *symbol;
            const char *candidate_name;

            symbol = &object->dynamic.symtab[sym_index];
            if (symbol->st_shndx == 0U) {
                continue;
            }

            candidate_name = process_object_dynamic_string(object, symbol->st_name);
            if (candidate_name == (const char *)0) {
                continue;
            }

            if (!process_streq(candidate_name, symbol_name)) {
                continue;
            }

            *value = object->load_bias + symbol->st_value;
            return 1;
        }
    }

    return 0;
}

static int process_resolve_relocation_symbol(const struct process_elf_load_state *state,
                                             const struct process_elf_object *object,
                                             unsigned long sym_index,
                                             unsigned long *value)
{
    const struct elf64_sym *symbol;
    const char *symbol_name;

    if (object->dynamic.symtab == (const struct elf64_sym *)0 ||
        sym_index >= object->dynamic.symtab_count) {
        return 0;
    }

    if (process_object_defined_symbol(object, sym_index, value)) {
        return 1;
    }

    symbol = &object->dynamic.symtab[sym_index];
    symbol_name = process_object_dynamic_string(object, symbol->st_name);
    return process_resolve_symbol_name(state, symbol_name, value);
}

static int process_apply_relocation_list(struct process_elf_load_state *state,
                                         const struct process_elf_object *object,
                                         const struct elf64_rela *entries,
                                         unsigned long entry_count)
{
    unsigned long index;

    for (index = 0UL; index < entry_count; index++) {
        unsigned long relocation_value;
        unsigned long sym_index;
        unsigned int relocation_type;

        relocation_type = process_elf_relocation_type(entries[index].r_info);
        sym_index = process_elf_relocation_symbol(entries[index].r_info);
        if (relocation_type == ELF_R_AARCH64_RELATIVE) {
            if (sym_index != 0UL) {
                return 0;
            }

            relocation_value = object->load_bias + (unsigned long)entries[index].r_addend;
        } else if (relocation_type == ELF_R_AARCH64_JUMP_SLOT ||
                   relocation_type == ELF_R_AARCH64_GLOB_DAT ||
                   relocation_type == ELF_R_AARCH64_ABS64) {
            if (!process_resolve_relocation_symbol(state, object, sym_index, &relocation_value)) {
                KER_INFO("unresolved ELF symbol");
                return 0;
            }

            relocation_value += (unsigned long)entries[index].r_addend;
        } else {
            KER_INFO("unsupported ELF relocation");
            return 0;
        }

        if (!process_write_user_u64(state->process,
                                    object->load_bias + entries[index].r_offset,
                                    relocation_value)) {
            return 0;
        }
    }

    return 1;
}

static int process_apply_object_relocations(struct process_elf_load_state *state,
                                            const struct process_elf_object *object)
{
    if (!process_apply_relocation_list(state,
                                       object,
                                       object->dynamic.rela_entries,
                                       object->dynamic.rela_count)) {
        return 0;
    }

    if (!process_apply_relocation_list(state,
                                       object,
                                       object->dynamic.jmprel_entries,
                                       object->dynamic.jmprel_count)) {
        return 0;
    }

    return 1;
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

static int process_find_loaded_object(const struct process_elf_load_state *state,
                                      const char *name)
{
    unsigned long index;

    for (index = 0UL; index < state->object_count; index++) {
        if (process_streq(state->objects[index].name, name)) {
            return 1;
        }
    }

    return 0;
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
        if (object->load_bias == 0UL && object->image_start != USER_CODE_BASE) {
            return 0;
        }
    } else {
        object->load_bias = 0UL;
    }

    if (set_entry != 0) {
        if (ehdr->e_entry < object->image_start || ehdr->e_entry >= object->image_end) {
            return 0;
        }

        state->process->entry_va = object->load_bias + ehdr->e_entry;
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

    if (!process_parse_dynamic_info(object)) {
        return 0;
    }

    if (object->dynamic.soname_offset != ~0UL) {
        const char *soname;

        soname = process_object_dynamic_string(object, object->dynamic.soname_offset);
        if (soname != (const char *)0) {
            if (!process_copy_string(object->name, soname, PROCESS_ELF_NAME_MAX)) {
                return 0;
            }
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

static int process_load_needed_objects(struct process_elf_load_state *state)
{
    unsigned long object_index;

    for (object_index = 0UL; object_index < state->object_count; object_index++) {
        const struct process_elf_object *object;
        unsigned long need_index;

        object = &state->objects[object_index];
        for (need_index = 0UL; need_index < object->dynamic.needed_count; need_index++) {
            unsigned char *image;
            unsigned long image_size;
            char path[PROCESS_ELF_NAME_MAX];
            const char *needed_name;

            needed_name = process_object_dynamic_string(object, object->dynamic.needed_offsets[need_index]);
            if (needed_name == (const char *)0) {
                return 0;
            }
            if (process_find_loaded_object(state, needed_name)) {
                continue;
            }

            if (!process_make_library_path(path, needed_name)) {
                return 0;
            }

            if (!process_load_file_image(path, &image, &image_size)) {
                return 0;
            }

            if (!process_register_loaded_object(state, needed_name, image, image, image_size, 0)) {
                kfree(image);
                return 0;
            }
        }
    }

    return 1;
}

static int process_map_elf_segment(struct process *process,
                                   const unsigned char *image,
                                   unsigned long image_size,
                                   const struct elf64_phdr *phdr,
                                   unsigned long load_bias)
{
    unsigned long segment_start;
    unsigned long segment_end;
    unsigned long page_start;
    unsigned long page_end;
    unsigned long page_flags;
    unsigned long page_va;

    if (phdr->p_type != ELF_PT_LOAD || phdr->p_memsz == 0UL) {
        return 1;
    }

    if ((phdr->p_flags & ELF_PF_W) != 0U && (phdr->p_flags & ELF_PF_X) != 0U) {
        return 0;
    }

    if (phdr->p_filesz > phdr->p_memsz || phdr->p_offset + phdr->p_filesz > image_size) {
        return 0;
    }

    segment_start = load_bias + phdr->p_vaddr;
    segment_end = segment_start + phdr->p_memsz;
    if (segment_start < USER_CODE_BASE || segment_end > USER_HEAP_LIMIT || segment_end <= segment_start) {
        return 0;
    }

    page_start = segment_start & ~(PAGE_SIZE - 1UL);
    page_end = process_align_up(segment_end, PAGE_SIZE);
    page_flags = process_elf_page_flags(phdr->p_flags);

    for (page_va = page_start; page_va < page_end; page_va += PAGE_SIZE) {
        unsigned long page_pa;
        unsigned long copy_start;
        unsigned long copy_end;
        unsigned long copy_count;

        page_pa = (unsigned long)page_alloc();
        if (page_pa == 0UL) {
            return 0;
        }

        copy_start = page_va;
        if (copy_start < segment_start) {
            copy_start = segment_start;
        }

        copy_end = page_va + PAGE_SIZE;
        if (copy_end > (segment_start + phdr->p_filesz)) {
            copy_end = segment_start + phdr->p_filesz;
        }

        copy_count = 0UL;
        if (copy_end > copy_start) {
            copy_count = copy_end - copy_start;
            process_copy_image_page_region(
                page_pa,
                copy_start - page_va,
                image + phdr->p_offset + (copy_start - segment_start),
                copy_count);
        }

        if (!process_map_page(process, page_va, page_pa, page_flags)) {
            page_free((void *)page_pa);
            return 0;
        }
    }

    return 1;
}

static struct process *process_create_flat_binary(const unsigned char *code, unsigned long code_size)
{
    struct process *process;
    unsigned long code_pages;
    unsigned long page_index;
    void *stack_page;

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

    for (page_index = 0UL; page_index < code_pages; page_index++) {
        unsigned long page_pa;
        unsigned long page_copy_len;
        unsigned long page_va;

        page_pa = (unsigned long)page_alloc();
        if (page_pa == 0UL) {
            process_destroy(process);
            return (struct process *)0;
        }

        page_copy_len = code_size - (page_index * PAGE_SIZE);
        if (page_copy_len > PAGE_SIZE) {
            page_copy_len = PAGE_SIZE;
        }

        process_copy_image_page(page_pa,
                                code + (page_index * PAGE_SIZE),
                                page_copy_len);
        page_va = USER_CODE_BASE + (page_index * PAGE_SIZE);
        if (!process_map_page(process, page_va, page_pa,
                              MMU_USER_PAGE_NORMAL | MMU_USER_PAGE_AF |
                              MMU_USER_PAGE_INNER_SH | MMU_USER_PAGE_AP_RO |
                              MMU_USER_PAGE_PXN)) {
            page_free((void *)page_pa);
            process_destroy(process);
            return (struct process *)0;
        }
    }

    __asm__ volatile("dsb ish\n ic iallu\n dsb nsh\n isb\n" ::: "memory");

    stack_page = page_alloc();
    if (stack_page == (void *)0) {
        process_destroy(process);
        return (struct process *)0;
    }

    if (!process_map_page(process, USER_STACK_TOP - PAGE_SIZE, (unsigned long)stack_page,
                          MMU_USER_PAGE_NORMAL | MMU_USER_PAGE_AF |
                          MMU_USER_PAGE_INNER_SH | MMU_USER_PAGE_AP_RW |
                          MMU_USER_PAGE_UXN | MMU_USER_PAGE_PXN)) {
        page_free(stack_page);
        process_destroy(process);
        return (struct process *)0;
    }

    return process;
}

struct process *process_create_from_buffer(const unsigned char *code, unsigned long code_size)
{
    if (process_is_elf_image(code, code_size)) {
        return process_create_from_elf(code, code_size);
    }

    return process_create_flat_binary(code, code_size);
}

struct process *process_create_from_elf(const unsigned char *image, unsigned long image_size)
{
    struct process *process;
    struct process_elf_load_state load_state;
    unsigned long index;
    unsigned long heap_start;
    void *stack_page;

    if (!process_is_elf_image(image, image_size)) {
        return (struct process *)0;
    }

    if (!process_validate_elf_header((const struct elf64_ehdr *)image, image_size)) {
        KER_INFO("invalid ELF image");
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

    process_zero_bytes(&load_state, sizeof(load_state));
    load_state.process = process;

    process->entry_va = 0UL;
    process->stack_top = USER_STACK_TOP;
    process->heap_start = 0UL;
    process->brk = 0UL;
    process->heap_limit = USER_HEAP_LIMIT;
    process->heap_mapped_end = 0UL;

    if (!process_register_loaded_object(&load_state, "<main>", (unsigned char *)0, image, image_size, 1)) {
        process_destroy(process);
        return (struct process *)0;
    }
    if (!process_load_needed_objects(&load_state)) {
        process_release_loaded_images(&load_state);
        process_destroy(process);
        return (struct process *)0;
    }

    for (index = 0UL; index < load_state.object_count; index++) {
        if (!process_apply_object_relocations(&load_state, &load_state.objects[index])) {
            process_release_loaded_images(&load_state);
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

    __asm__ volatile("dsb ish\n ic iallu\n dsb nsh\n isb\n" ::: "memory");

    stack_page = page_alloc();
    if (stack_page == (void *)0) {
        process_destroy(process);
        return (struct process *)0;
    }

    if (!process_map_page(process, USER_STACK_TOP - PAGE_SIZE, (unsigned long)stack_page,
                          MMU_USER_PAGE_NORMAL | MMU_USER_PAGE_AF |
                          MMU_USER_PAGE_INNER_SH | MMU_USER_PAGE_AP_RW |
                          MMU_USER_PAGE_UXN | MMU_USER_PAGE_PXN)) {
        page_free(stack_page);
        process_release_loaded_images(&load_state);
        process_destroy(process);
        return (struct process *)0;
    }

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

    kfree(process);
}

unsigned long process_brk(struct process *process, unsigned long new_break)
{
    unsigned long desired_end;
    unsigned long old_break;
    unsigned long old_mapped_end;
    unsigned long new_mapped_end;
    unsigned long new_pages[PROCESS_BRK_ROLLBACK_MAX_PAGES];
    unsigned long mapped_count;

    if (process == (struct process *)0 || process->mm == (struct mm_context *)0) {
        return 0UL;
    }

    if (new_break == 0UL) {
        return process->brk;
    }

    if (new_break < process->heap_start || new_break > process->heap_limit) {
        return process->brk;
    }

    new_mapped_end = process_align_up(new_break, PAGE_SIZE);
    if (new_break <= process->brk) {
        while (process->heap_mapped_end > new_mapped_end) {
            unsigned long unmap_va;
            unsigned long page_pa;

            unmap_va = process->heap_mapped_end - PAGE_SIZE;
            if (!mmu_user_page_pa(process->mm, unmap_va, &page_pa)) {
                break;
            }

            if (!mmu_unmap_user_page(process->mm, unmap_va)) {
                break;
            }

            mmu_context_remove_page(process->mm, page_pa);
            page_free((void *)page_pa);
            process->heap_mapped_end = unmap_va;
        }

        process->brk = new_break;
        return process->brk;
    }

    old_break = process->brk;
    old_mapped_end = process->heap_mapped_end;
    desired_end = new_mapped_end;
    mapped_count = 0UL;
    while (process->heap_mapped_end < desired_end) {
        void *page;

        page = page_alloc();
        if (page == (void *)0) {
            break;
        }

        if (!process_map_page(process, process->heap_mapped_end, (unsigned long)page,
                              MMU_USER_PAGE_NORMAL | MMU_USER_PAGE_AF |
                              MMU_USER_PAGE_INNER_SH | MMU_USER_PAGE_AP_RW |
                              MMU_USER_PAGE_UXN | MMU_USER_PAGE_PXN)) {
            page_free(page);
            break;
        }

        new_pages[mapped_count++] = (unsigned long)page;
        process->heap_mapped_end += PAGE_SIZE;
    }

    if (process->heap_mapped_end < desired_end) {
        while (mapped_count > 0UL) {
            unsigned long rollback_va;
            unsigned long rollback_pa;

            mapped_count--;
            rollback_pa = new_pages[mapped_count];
            rollback_va = old_mapped_end + (mapped_count * PAGE_SIZE);
            mmu_unmap_user_page(process->mm, rollback_va);
            mmu_context_remove_page(process->mm, rollback_pa);
            page_free((void *)rollback_pa);
        }

        process->heap_mapped_end = old_mapped_end;
        process->brk = old_break;
        return process->brk;
    }

    process->brk = new_break;
    return process->brk;
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