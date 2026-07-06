#include "elf_format.h"
#include "ld_syscalls.h"

#define MAX_OBJECTS 16
struct ld_object {
    char name[64];
    unsigned long base;
    Elf64_Dyn *dynamic;
    unsigned long entry;
    Elf64_Sym *symtab;
    const char *strtab;
    Elf64_Word *hashtab;     /* traditional SysV hash (unused) */
    Elf64_Word *gnu_hashtab; /* GNU hash (.gnu.hash) */
};

static struct ld_object g_objects[MAX_OBJECTS];
static int g_object_count = 0;

/*
 * ld_self_relocate thực hiện việc relocate cho chính linker.
 * Nó duyệt qua mảng _DYNAMIC để tìm các bảng RELA.
 */
__attribute__((visibility("hidden")))
void ld_self_relocate(unsigned long base, void *dyn_ptr) {
    Elf64_Dyn *dyn = (Elf64_Dyn *)dyn_ptr;
    Elf64_Rela *rela = (Elf64_Rela *)0;
    unsigned long rela_size = 0;
    unsigned long rela_ent = 0;
    unsigned long rela_count = 0;

    /* Tìm các tag liên quan đến relocation */
    for (int i = 0; dyn[i].d_tag != DT_NULL; i++) {
        if (dyn[i].d_tag == DT_RELA) {
            rela = (Elf64_Rela *)(base + dyn[i].d_un.d_ptr);
        } else if (dyn[i].d_tag == DT_RELASZ) {
            rela_size = dyn[i].d_un.d_val;
        } else if (dyn[i].d_tag == DT_RELAENT) {
            rela_ent = dyn[i].d_un.d_val;
        } else if (dyn[i].d_tag == DT_RELACOUNT || dyn[i].d_tag == 0x6ffffff9) {
            rela_count = dyn[i].d_un.d_val;
        }
    }

    if (rela && rela_ent > 0) {
        unsigned long count = rela_count ? rela_count : (rela_size / rela_ent);
        for (unsigned long i = 0; i < count; i++) {
            Elf64_Rela *r = (Elf64_Rela *)((unsigned long)rela + i * rela_ent);
            if (ELF64_R_TYPE(r->r_info) == R_AARCH64_RELATIVE) {
                unsigned long *addr = (unsigned long *)(base + r->r_offset);
                *addr = base + r->r_addend;
            }
        }
    }
}

static struct ld_object *ld_find_object(const char *name) {
    for (int i = 0; i < g_object_count; i++) {
        if (ld_strcmp(g_objects[i].name, name) == 0) return &g_objects[i];
    }
    return 0;
}

static struct ld_object *ld_load_object(const char *path, unsigned long load_bias) {
    struct ld_object *existing = ld_find_object(path);
    if (existing) return existing;

    if (g_object_count >= MAX_OBJECTS) return 0;
    struct ld_object *obj = &g_objects[g_object_count++];
    ld_strcpy(obj->name, path);

    long fd = ld_open(path);
    if (fd < 0) {
        /* Try /lib/ if it's a simple name */
        char fullpath[128];
        ld_strcpy(fullpath, "/lib/");
        ld_strcpy(fullpath + 5, path);
        fd = ld_open(fullpath);
        if (fd < 0) {
            ld_puts("Failed to open object: ");
            ld_puts(path);
            ld_puts("\n");
            g_object_count--;
            return 0;
        }
    }

    Elf64_Ehdr ehdr;
    if (ld_read(fd, &ehdr, sizeof(ehdr)) != sizeof(ehdr)) return 0;

    Elf64_Phdr phdr[16];
    if (ehdr.e_phoff != sizeof(ehdr)) {
        char dummy[1024];
        ld_read(fd, dummy, ehdr.e_phoff - sizeof(ehdr));
    }
    ld_read(fd, phdr, ehdr.e_phnum * ehdr.e_phentsize);

    obj->base = load_bias;
    obj->entry = ehdr.e_entry + load_bias;
    obj->dynamic = 0;
    obj->symtab = 0;
    obj->strtab = 0;
    obj->hashtab = 0;

    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdr[i].p_type == ELF_PT_LOAD) {
            int prot = 0;
            if (phdr[i].p_flags & ELF_PF_R) prot |= 1;
            if (phdr[i].p_flags & ELF_PF_W) prot |= 2;
            if (phdr[i].p_flags & ELF_PF_X) prot |= 4;

            unsigned long vaddr = phdr[i].p_vaddr + load_bias;
            unsigned long memsz = phdr[i].p_memsz;
            unsigned long filesz = phdr[i].p_filesz;
            unsigned long offset = phdr[i].p_offset;

            unsigned long align_vaddr = vaddr & ~0xFFFUL;
            unsigned long align_offset = offset & ~0xFFFUL;
            unsigned long diff = vaddr - align_vaddr;
            unsigned long align_memsz = (memsz + diff + 0xFFFUL) & ~0xFFFUL;

            ld_mmap((void *)align_vaddr, align_memsz, prot, 0, fd, align_offset);
            
            /* If it's a WRITE segment, zero out the padding if memsz > filesz */
            // For now, mmap should handle zeroing if it's fresh pages, but we should be careful.
        } else if (phdr[i].p_type == ELF_PT_DYNAMIC) {
            obj->dynamic = (Elf64_Dyn *)(phdr[i].p_vaddr + load_bias);
        }
    }

    /* Parse dynamic section for symbols */
    if (obj->dynamic) {
        for (Elf64_Dyn *d = obj->dynamic; d->d_tag != DT_NULL; d++) {
            if (d->d_tag == DT_SYMTAB)
                obj->symtab = (Elf64_Sym *)(d->d_un.d_ptr + load_bias);
            else if (d->d_tag == DT_STRTAB)
                obj->strtab = (const char *)(d->d_un.d_ptr + load_bias);
            else if (d->d_tag == DT_HASH)
                obj->hashtab = (Elf64_Word *)(d->d_un.d_ptr + load_bias);
            else if (d->d_tag == DT_GNU_HASH)
                obj->gnu_hashtab = (Elf64_Word *)(d->d_un.d_ptr + load_bias);
        }
    }

    /* Load dependencies */
    if (obj->dynamic) {
        const char *strtab = 0;
        for (Elf64_Dyn *d = obj->dynamic; d->d_tag != DT_NULL; d++) {
            if (d->d_tag == DT_STRTAB) strtab = (const char *)(d->d_un.d_ptr + load_bias);
        }

        if (strtab) {
            for (Elf64_Dyn *d = obj->dynamic; d->d_tag != DT_NULL; d++) {
                if (d->d_tag == DT_NEEDED) {
                    const char *libname = strtab + d->d_un.d_val;
                    /* Load library at a different bias. For simplicity, just increment bias */
                    static unsigned long next_bias = 0x5000000;
                    ld_load_object(libname, next_bias);
                    next_bias += 0x1000000;
                }
            }
        }
    }

    return obj;
}

/* Compute GNU hash (DJB2 variant used in GNU ld) */
static unsigned int ld_gnu_hash(const char *name) {
    unsigned int h = 5381U;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        h = h * 33U + *p;
    return h;
}

/* Look up symbol in one object using its GNU hash table */
static unsigned long ld_lookup_gnu_hash(struct ld_object *obj, const char *name) {
    if (!obj->gnu_hashtab || !obj->symtab || !obj->strtab)
        return 0;

    unsigned int *ht        = (unsigned int *)obj->gnu_hashtab;
    unsigned int  nbuckets  = ht[0];
    unsigned int  symoffset = ht[1];
    unsigned int  bloom_sz  = ht[2];
    /* ht[3] = bloom_shift (skip bloom filter for correctness) */

    /* bloom words are 64-bit on AArch64 */
    unsigned long *bloom   = (unsigned long *)&ht[4];
    unsigned int  *buckets = (unsigned int  *)&bloom[bloom_sz];
    unsigned int  *chains  = &buckets[nbuckets];

    unsigned int h          = ld_gnu_hash(name);
    unsigned int bucket_idx = h % nbuckets;
    unsigned int sym_idx    = buckets[bucket_idx];

    if (sym_idx < symoffset)
        return 0;

    for (unsigned int ci = sym_idx - symoffset; ; ci++, sym_idx++) {
        unsigned int c = chains[ci];
        if ((c | 1U) == (h | 1U)) {
            Elf64_Sym *sym = &obj->symtab[sym_idx];
            if (sym->st_shndx != 0 &&
                ld_strcmp(obj->strtab + sym->st_name, name) == 0) {
                return obj->base + sym->st_value;
            }
        }
        if (c & 1U) break; /* last entry in this chain */
    }
    return 0;
}

static unsigned long ld_lookup_symbol(const char *name) {
    for (int i = 0; i < g_object_count; i++) {
        unsigned long addr = ld_lookup_gnu_hash(&g_objects[i], name);
        if (addr) return addr;
    }
    return 0;
}

static void ld_relocate_object(struct ld_object *obj) {
    if (!obj->dynamic) return;

    unsigned long load_bias = obj->base;
    Elf64_Rela *rela = 0;
    unsigned long relasz = 0;
    unsigned long relaent = 24; /* default RELA entry size; overridden by DT_RELAENT */
    Elf64_Rela *jmprel = 0;
    unsigned long pltrelsz = 0;

    for (Elf64_Dyn *d = obj->dynamic; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_RELA) {
            rela = (Elf64_Rela *)(d->d_un.d_ptr + load_bias);
        } else if (d->d_tag == DT_RELASZ) {
            relasz = d->d_un.d_val;
        } else if (d->d_tag == DT_RELAENT) {
            relaent = d->d_un.d_val;
        } else if (d->d_tag == DT_JMPREL) {
            jmprel = (Elf64_Rela *)(d->d_un.d_ptr + load_bias);
        } else if (d->d_tag == DT_PLTRELSZ) {
            pltrelsz = d->d_un.d_val;
        }
    }

    if (rela && relasz && relaent) {
        for (unsigned long i = 0; i < relasz / relaent; i++) {
            Elf64_Rela *r = (Elf64_Rela *)((unsigned long)rela + i * relaent);
            unsigned int type = ELF64_R_TYPE(r->r_info);
            unsigned int sym_idx = ELF64_R_SYM(r->r_info);
            unsigned long *addr = (unsigned long *)(r->r_offset + load_bias);

            if (type == R_AARCH64_RELATIVE) {
                *addr = r->r_addend + load_bias;
            } else if (type == R_AARCH64_GLOB_DAT || type == R_AARCH64_JUMP_SLOT) {
                if (obj->symtab && obj->strtab) {
                    const char *symname = obj->strtab + obj->symtab[sym_idx].st_name;
                    unsigned long symval = ld_lookup_symbol(symname);
                    if (symval) {
                        *addr = symval + r->r_addend;
                    } else {
                        ld_puts("Symbol not found: ");
                        ld_puts(symname);
                        ld_puts("\n");
                    }
                }
            } else if (type == R_AARCH64_ABS64) {
                 if (obj->symtab && obj->strtab) {
                    const char *symname = obj->strtab + obj->symtab[sym_idx].st_name;
                    unsigned long symval = ld_lookup_symbol(symname);
                     if (symval) {
                        *addr = symval + r->r_addend;
                    }
                 }
            }
        }
    }

    if (jmprel && pltrelsz && relaent) {
        for (unsigned long i = 0; i < pltrelsz / relaent; i++) {
            Elf64_Rela *r = (Elf64_Rela *)((unsigned long)jmprel + i * relaent);
            unsigned int type = ELF64_R_TYPE(r->r_info);
            unsigned int sym_idx = ELF64_R_SYM(r->r_info);
            unsigned long *addr = (unsigned long *)(r->r_offset + load_bias);

            if (type == R_AARCH64_JUMP_SLOT) {
                if (obj->symtab && obj->strtab) {
                    const char *symname = obj->strtab + obj->symtab[sym_idx].st_name;
                    unsigned long symval = ld_lookup_symbol(symname);
                    if (symval) {
                        *addr = symval + r->r_addend;
                    } else {
                        ld_puts("PLT Symbol not found: ");
                        ld_puts(symname);
                        ld_puts("\n");
                    }
                }
            }
        }
    }
}

__attribute__((visibility("hidden")))
unsigned long ld_load_elf(const char *path, unsigned long *load_bias_out) {
    /* Reset state */
    g_object_count = 0;

    struct ld_object *main_obj = ld_load_object(path, 0x4000000);
    if (!main_obj) return 0;

    *load_bias_out = main_obj->base;

    /* Relocate all objects */
    for (int i = 0; i < g_object_count; i++) {
        ld_relocate_object(&g_objects[i]);
    }

    return main_obj->entry;
}

__attribute__((visibility("hidden")))
void ld_main(void *stack) {
    (void)stack;
    ld_puts("Dynamic Linker started!\n");

    /* 
     * In a real OS, ld.so would get the path to the app from the kernel 
     * via Aux vectors or argv. For this demo, let's load /bin/hello.elf 
     */
    unsigned long load_bias = 0;
    unsigned long entry = ld_load_elf("/bin/hello.elf", &load_bias);

    if (entry == 0) {
        ld_puts("Failed to load /bin/hello.elf\n");
        ld_exit(1);
    }

    ld_puts("App loaded successfully! Jumping to entry point...\n");

    /* Jump to entry point */
    /* We need to pass the original stack to the app */
    asm volatile (
        "mov sp, %1\n"
        "br %0\n"
        : : "r"(entry), "r"(stack) : "memory"
    );

    while(1);
}
