#include "elf_format.h"
#include "ld_syscalls.h"

#define MAX_OBJECTS 16

/* Load-address policy for dependency libraries and the fallback main object. */
#define LD_MAIN_LOAD_BASE   0x4000000UL
#define LD_LIB_LOAD_BASE    0x5000000UL
#define LD_LIB_LOAD_STRIDE  0x1000000UL

/* Hard limits used for bounds checking untrusted ELF input. */
#define LD_MAX_PHNUM        16
#define LD_PATH_MAX         128
#define LD_PAGE_MASK        0xFFFUL

static int g_debug = 0;

static void ld_log(const char *msg) {
    if (g_debug) ld_puts(msg);
}

struct ld_object {
    char name[64];
    unsigned long base;
    Elf64_Dyn *dynamic;
    unsigned long entry;
    Elf64_Sym *symtab;
    const char *strtab;
    Elf64_Word *hashtab;     /* traditional SysV hash (unused) */
    Elf64_Word *gnu_hashtab; /* GNU hash (.gnu.hash) */

    unsigned long init;
    unsigned long *init_array;
    unsigned long init_array_sz;

    /* General relocations (.rela.dyn) */
    Elf64_Rela *rela;
    unsigned long relasz;
    unsigned long relaent;

    /* For lazy binding (.rela.plt) */
    Elf64_Rela *jmprel;
    unsigned long pltrelsz;
    unsigned long pltgot;
};

extern void ld_plt_resolver(void);
static unsigned long ld_lookup_symbol(const char *name);
static struct ld_object *ld_load_object(const char *path, unsigned long load_bias);

/*
 * Parse an object's _DYNAMIC array and fill every table pointer in the struct.
 * Assumes obj->dynamic and obj->base are already set. All pointers are biased
 * by obj->base so they are directly usable at runtime.
 *
 * This is the single source of truth for DT_* handling: both the mmap-based
 * loader (ld_load_object) and the already-mapped loader
 * (ld_init_object_from_phdr) call it, so a new DT_ tag only needs adding here.
 */
static void ld_parse_dynamic(struct ld_object *obj) {
    unsigned long bias = obj->base;

    obj->symtab = 0;
    obj->strtab = 0;
    obj->hashtab = 0;
    obj->gnu_hashtab = 0;
    obj->init = 0;
    obj->init_array = 0;
    obj->init_array_sz = 0;
    obj->rela = 0;
    obj->relasz = 0;
    obj->relaent = sizeof(Elf64_Rela);
    obj->jmprel = 0;
    obj->pltrelsz = 0;
    obj->pltgot = 0;

    if (!obj->dynamic) {
        return;
    }

    for (Elf64_Dyn *d = obj->dynamic; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_SYMTAB:        obj->symtab = (Elf64_Sym *)(d->d_un.d_ptr + bias); break;
        case DT_STRTAB:        obj->strtab = (const char *)(d->d_un.d_ptr + bias); break;
        case DT_HASH:          obj->hashtab = (Elf64_Word *)(d->d_un.d_ptr + bias); break;
        case DT_GNU_HASH:      obj->gnu_hashtab = (Elf64_Word *)(d->d_un.d_ptr + bias); break;
        case DT_INIT:          obj->init = d->d_un.d_ptr + bias; break;
        case DT_INIT_ARRAY:    obj->init_array = (unsigned long *)(d->d_un.d_ptr + bias); break;
        case DT_INIT_ARRAYSZ:  obj->init_array_sz = d->d_un.d_val; break;
        case DT_RELA:          obj->rela = (Elf64_Rela *)(d->d_un.d_ptr + bias); break;
        case DT_RELASZ:        obj->relasz = d->d_un.d_val; break;
        case DT_RELAENT:       obj->relaent = d->d_un.d_val; break;
        case DT_PLTGOT:        obj->pltgot = d->d_un.d_ptr + bias; break;
        case DT_JMPREL:        obj->jmprel = (Elf64_Rela *)(d->d_un.d_ptr + bias); break;
        case DT_PLTRELSZ:      obj->pltrelsz = d->d_un.d_val; break;
        default: break;
        }
    }
}

/*
 * Load every DT_NEEDED dependency of an object. Separated from parsing so the
 * parser stays free of side effects (and recursion into the loader).
 */
static void ld_load_needed(struct ld_object *obj) {
    if (!obj->dynamic || !obj->strtab) {
        return;
    }

    static unsigned long next_bias = LD_LIB_LOAD_BASE;
    for (Elf64_Dyn *d = obj->dynamic; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_NEEDED) {
            const char *libname = obj->strtab + d->d_un.d_val;
            ld_load_object(libname, next_bias);
            next_bias += LD_LIB_LOAD_STRIDE;
        }
    }
}

__attribute__((visibility("hidden")))
unsigned long ld_lazy_resolve(struct ld_object *obj, unsigned long *got_entry_va) {
    if (!obj || !obj->jmprel) return 0;
    
    /* Find the relocation entry that corresponds to this GOT address */
    Elf64_Rela *rel = 0;
    for (unsigned long i = 0; i < obj->pltrelsz / sizeof(Elf64_Rela); i++) {
        if ((unsigned long *)(obj->jmprel[i].r_offset + obj->base) == got_entry_va) {
            rel = &obj->jmprel[i];
            break;
        }
    }

    if (!rel) return 0;

    unsigned int sym_idx = ELF64_R_SYM(rel->r_info);
    const char *symname = obj->strtab + obj->symtab[sym_idx].st_name;
    
    ld_log("Lazy resolving: ");
    ld_log(symname);
    ld_log("\n");

    unsigned long addr = ld_lookup_symbol(symname);
    if (addr) {
        /* Update GOT entry patch the address */
        *got_entry_va = addr + rel->r_addend;
        return *got_entry_va;
    }

    /*
     * Unresolved lazy symbol. Returning 0 here would make the PLT resolver
     * branch to address 0 and take an opaque instruction abort. Abort loudly
     * instead so the failure is attributable to the missing symbol.
     */
    ld_puts("Lazy symbol not found: ");
    ld_puts(symname);
    ld_puts("\n");
    ld_exit(127);
    return 0;
}

static struct ld_object g_objects[MAX_OBJECTS];
static int g_object_count = 0;

/* Symbol lookup cache to avoid expensive string comparisons */
#define SYMBOL_CACHE_SIZE 128
struct ld_symbol_cache {
    const char *name;
    unsigned long addr;
};
static struct ld_symbol_cache g_sym_cache[SYMBOL_CACHE_SIZE];
static unsigned int g_sym_cache_next = 0;

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

/*
 * Validate an ELF header read from an untrusted file. Returns 1 if the object
 * is a 64-bit little-endian AArch64 ELF whose program-header table we can hold.
 */
static int ld_validate_ehdr(const Elf64_Ehdr *ehdr) {
    if (ehdr->e_ident[ELF_EI_MAG0] != ELF_MAG0 ||
        ehdr->e_ident[ELF_EI_MAG1] != ELF_MAG1 ||
        ehdr->e_ident[ELF_EI_MAG2] != ELF_MAG2 ||
        ehdr->e_ident[ELF_EI_MAG3] != ELF_MAG3) {
        return 0;
    }
    if (ehdr->e_ident[ELF_EI_CLASS] != ELF_CLASS_64 ||
        ehdr->e_ident[ELF_EI_DATA] != ELF_DATA_LE) {
        return 0;
    }
    if (ehdr->e_machine != ELF_EM_AARCH64) {
        return 0;
    }
    if (ehdr->e_phnum == 0 || ehdr->e_phnum > LD_MAX_PHNUM) {
        return 0;
    }
    if (ehdr->e_phentsize != sizeof(Elf64_Phdr)) {
        return 0;
    }
    return 1;
}

/* Open a shared object by name, searching the standard library paths. */
static long ld_open_object(const char *path) {
    long fd = ld_open(path);
    if (fd >= 0) {
        return fd;
    }

    const char *search_paths[] = {"/lib/", "/usr/lib/", "/bin/", 0};
    for (int i = 0; search_paths[i]; i++) {
        char fullpath[LD_PATH_MAX];
        unsigned long plen = ld_strlen(search_paths[i]);
        unsigned long nlen = ld_strlen(path);
        if (plen + nlen + 1 > LD_PATH_MAX) {
            continue; /* would overflow fullpath */
        }
        ld_strcpy(fullpath, search_paths[i]);
        ld_strcpy(fullpath + plen, path);
        fd = ld_open(fullpath);
        if (fd >= 0) {
            return fd;
        }
    }
    return -1;
}

static struct ld_object *ld_load_object(const char *path, unsigned long load_bias) {
    struct ld_object *existing = ld_find_object(path);
    if (existing) return existing;

    ld_log("Loading object: ");
    ld_log(path);
    ld_log("\n");

    if (g_object_count >= MAX_OBJECTS) return 0;
    struct ld_object *obj = &g_objects[g_object_count++];
    ld_strcpy(obj->name, path);

    long fd = ld_open_object(path);
    if (fd < 0) {
        ld_puts("Failed to open object: ");
        ld_puts(path);
        ld_puts("\n");
        g_object_count--;
        return 0;
    }

    Elf64_Ehdr ehdr;
    if (ld_read(fd, &ehdr, sizeof(ehdr)) != (long)sizeof(ehdr) ||
        !ld_validate_ehdr(&ehdr)) {
        ld_puts("Invalid ELF object: ");
        ld_puts(path);
        ld_puts("\n");
        g_object_count--;
        return 0;
    }

    Elf64_Phdr phdr[LD_MAX_PHNUM];
    if (ehdr.e_phoff != sizeof(ehdr)) {
        char dummy[1024];
        ld_read(fd, dummy, ehdr.e_phoff - sizeof(ehdr));
    }
    unsigned long phdr_bytes = (unsigned long)ehdr.e_phnum * ehdr.e_phentsize;
    if (ld_read(fd, phdr, phdr_bytes) != (long)phdr_bytes) {
        ld_puts("Failed to read program headers: ");
        ld_puts(path);
        ld_puts("\n");
        g_object_count--;
        return 0;
    }

    obj->base = load_bias;
    obj->entry = ehdr.e_entry + load_bias;
    obj->dynamic = 0;

    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdr[i].p_type == ELF_PT_LOAD) {
            int prot = 0;
            if (phdr[i].p_flags & ELF_PF_R) prot |= LD_PROT_READ;
            if (phdr[i].p_flags & ELF_PF_W) prot |= LD_PROT_WRITE;
            if (phdr[i].p_flags & ELF_PF_X) prot |= LD_PROT_EXEC;

            unsigned long vaddr = phdr[i].p_vaddr + load_bias;
            unsigned long memsz = phdr[i].p_memsz;
            unsigned long offset = phdr[i].p_offset;

            unsigned long align_vaddr = vaddr & ~LD_PAGE_MASK;
            unsigned long align_offset = offset & ~LD_PAGE_MASK;
            unsigned long diff = vaddr - align_vaddr;
            unsigned long align_memsz = (memsz + diff + LD_PAGE_MASK) & ~LD_PAGE_MASK;

            void *mapped = ld_mmap((void *)align_vaddr, align_memsz, prot, 0, fd, align_offset);
            if (mapped == (void *)-1 || mapped == 0) {
                ld_puts("mmap failed for segment in: ");
                ld_puts(path);
                ld_puts("\n");
                g_object_count--;
                return 0;
            }
        } else if (phdr[i].p_type == ELF_PT_DYNAMIC) {
            obj->dynamic = (Elf64_Dyn *)(phdr[i].p_vaddr + load_bias);
        }
    }

    /* Fill all DT_* table pointers, then load dependencies. */
    ld_parse_dynamic(obj);
    ld_load_needed(obj);

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
    unsigned int  bloom_shift = ht[3];

    /* bloom words are 64-bit on AArch64 */
    unsigned long *bloom   = (unsigned long *)&ht[4];
    unsigned int  *buckets = (unsigned int  *)&bloom[bloom_sz];
    unsigned int  *chains  = &buckets[nbuckets];

    unsigned int h          = ld_gnu_hash(name);

    /* Phase 1: Bloom Filter check (massive speedup for symbol misses) */
    unsigned long bloom_word = bloom[(h / 64) % bloom_sz];
    unsigned long mask = (1UL << (h % 64)) | (1UL << ((h >> bloom_shift) % 64));
    if ((bloom_word & mask) != mask) return 0;

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
    /* Check cache first */
    for (int i = 0; i < SYMBOL_CACHE_SIZE; i++) {
        if (g_sym_cache[i].name && ld_strcmp(g_sym_cache[i].name, name) == 0) {
            return g_sym_cache[i].addr;
        }
    }

    /* Fallback to full lookup */
    for (int i = 0; i < g_object_count; i++) {
        unsigned long addr = ld_lookup_gnu_hash(&g_objects[i], name);
        if (addr) {
            /* Update cache (simple round-robin replacement) */
            g_sym_cache[g_sym_cache_next].name = name;
            g_sym_cache[g_sym_cache_next].addr = addr;
            g_sym_cache_next = (g_sym_cache_next + 1) % SYMBOL_CACHE_SIZE;
            return addr;
        }
    }
    return 0;
}

static void ld_call_init(struct ld_object *obj) {
    if (obj->init) {
        void (*init_func)(void) = (void (*)(void))obj->init;
        init_func();
    }
    if (obj->init_array && obj->init_array_sz) {
        unsigned long count = obj->init_array_sz / sizeof(unsigned long);
        for (unsigned long i = 0; i < count; i++) {
            void (*init_func)(void) = (void (*)(void))obj->init_array[i];
            if (init_func) init_func();
        }
    }
}

static void ld_relocate_object(struct ld_object *obj) {
    if (!obj->dynamic) return;

    unsigned long load_bias = obj->base;
    Elf64_Rela *rela = obj->rela;
    unsigned long relasz = obj->relasz;
    unsigned long relaent = obj->relaent ? obj->relaent : sizeof(Elf64_Rela);
    Elf64_Rela *jmprel = obj->jmprel;
    unsigned long pltrelsz = obj->pltrelsz;

    if (rela && relasz && relaent) {
        for (unsigned long i = 0; i < relasz / relaent; i++) {
            Elf64_Rela *r = (Elf64_Rela *)((unsigned long)rela + i * relaent);
            unsigned int type = ELF64_R_TYPE(r->r_info);
            unsigned int sym_idx = ELF64_R_SYM(r->r_info);
            unsigned long *addr = (unsigned long *)(r->r_offset + load_bias);

            if (type == R_AARCH64_RELATIVE) {
                *addr = r->r_addend + load_bias;
            } else if (type == R_AARCH64_GLOB_DAT || type == R_AARCH64_ABS64) {
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
            } else if (type == R_AARCH64_JUMP_SLOT) {
                /* 
                 * For Lazy binding: We MUST add the load_bias to the GOT entry 
                 * because it points to the PLT stub, which is shifted.
                 * This is done for all JUMP_SLOTs initially.
                 */
                *addr += load_bias;
            }
        }
    }

    if (jmprel && pltrelsz && relaent) {
        /* Set up Lazy Binding (PLT0 needs help from Linker) */
        if (obj->pltgot) {
            unsigned long *got = (unsigned long *)obj->pltgot;
            got[1] = (unsigned long)obj;
            got[2] = (unsigned long)ld_plt_resolver;
        }

        /* 
         * If BIND_NOW is set, we SHOULD resolve these eagerly.
         * Otherwise, we leave them for the PLT resolver.
         */
        int bind_now = 0;
        for (Elf64_Dyn *d = obj->dynamic; d->d_tag != DT_NULL; d++) {
            if (d->d_tag == DT_FLAGS && (d->d_un.d_val & DF_BIND_NOW)) bind_now = 1;
            if (d->d_tag == DT_FLAGS_1 && (d->d_un.d_val & DF_1_NOW)) bind_now = 1;
        }

        if (bind_now) {
            for (unsigned long i = 0; i < pltrelsz / relaent; i++) {
                Elf64_Rela *r = (Elf64_Rela *)((unsigned long)jmprel + i * relaent);
                unsigned int sym_idx = ELF64_R_SYM(r->r_info);
                unsigned long *addr = (unsigned long *)(r->r_offset + load_bias);
                const char *symname = obj->strtab + obj->symtab[sym_idx].st_name;
                unsigned long symval = ld_lookup_symbol(symname);
                if (symval) {
                    *addr = symval + r->r_addend;
                }
            }
        } else {
            /*
             * Lazy binding: the initial GOT entry for each PLT slot holds the
             * *link-time* address of PLT0 (e.g. 0x600). It must be biased by
             * load_bias so the first call reaches the real PLT0 at
             * load_bias + 0x600, which then traps into ld_plt_resolver.
             * Without this, the first call branches to the unbiased address
             * and faults (EL0 instruction abort at the un-relocated offset).
             */
            for (unsigned long i = 0; i < pltrelsz / relaent; i++) {
                Elf64_Rela *r = (Elf64_Rela *)((unsigned long)jmprel + i * relaent);
                unsigned long *addr = (unsigned long *)(r->r_offset + load_bias);
                *addr += load_bias;
            }
        }
    }
}

static void ld_log_hex(unsigned long val) {
    if (!g_debug) return;
    char buf[17];
    char *digits = "0123456789abcdef";
    for (int i = 15; i >= 0; i--) {
        buf[i] = digits[val & 0xf];
        val >>= 4;
    }
    buf[16] = 0;
    ld_puts("0x");
    ld_puts(buf);
}

static struct ld_object *ld_init_object_from_phdr(const char *name, Elf64_Phdr *phdr, int phnum, unsigned long load_bias) {
    if (g_object_count >= MAX_OBJECTS) return 0;
    if (phnum <= 0 || phnum > LD_MAX_PHNUM) return 0;

    struct ld_object *obj = &g_objects[g_object_count++];
    ld_strcpy(obj->name, name);

    obj->base = load_bias;
    obj->dynamic = 0;

    /* Segments are already mapped (by the kernel or a prior mmap); just locate
     * the dynamic section so ld_parse_dynamic() can fill the table pointers. */
    for (int i = 0; i < phnum; i++) {
        if (phdr[i].p_type == ELF_PT_DYNAMIC) {
            obj->dynamic = (Elf64_Dyn *)(phdr[i].p_vaddr + load_bias);
        }
    }

    ld_parse_dynamic(obj);
    ld_load_needed(obj);

    return obj;
}

__attribute__((visibility("hidden")))
unsigned long ld_load_elf(const char *path, unsigned long *load_bias_out) {
    /* Reset state */
    g_object_count = 0;

    struct ld_object *main_obj = ld_load_object(path, LD_MAIN_LOAD_BASE);
    if (!main_obj) return 0;

    *load_bias_out = main_obj->base;

    /* Relocate all objects */
    for (int i = 0; i < g_object_count; i++) {
        ld_relocate_object(&g_objects[i]);
    }

    /* Call init routines in reverse order (dependencies first) */
    for (int i = g_object_count - 1; i >= 0; i--) {
        ld_call_init(&g_objects[i]);
    }

    return main_obj->entry;
}

__attribute__((visibility("hidden")))
void ld_main(void *stack) {
    g_debug = 1; // Force debug for now to confirm it works
    long argc = *(unsigned long *)stack;
    char **argv = (char **)((unsigned long *)stack + 1);
    char **envp = argv + argc + 1;
    
    /* Find Auxv and check environment */
    char **p = envp;
    while (p && *p) {
        if (ld_strncmp(*p, "LD_DEBUG=", 9) == 0) {
            g_debug = 1;
        }
        p++;
    }
    Elf64_auxv_t *auxv = (Elf64_auxv_t *)(p + 1);
    
    const char *app_path = 0;
    Elf64_Phdr *app_phdr = 0;
    int app_phnum = 0;
    unsigned long app_entry = 0;
    unsigned long interp_base = 0;

    for (Elf64_auxv_t *a = auxv; a->a_type != AT_NULL; a++) {
        if (a->a_type == AT_EXECFN) app_path = (const char *)a->a_val;
        if (a->a_type == AT_PHDR) app_phdr = (Elf64_Phdr *)a->a_val;
        if (a->a_type == AT_PHNUM) app_phnum = (int)a->a_val;
        if (a->a_type == AT_ENTRY) app_entry = a->a_val;
        if (a->a_type == AT_BASE) interp_base = a->a_val;
    }

    ld_log("Dynamic Linker started!\n");

    unsigned long entry_point = 0;

    /*
     * Linux-style: AT_BASE != 0 means the kernel loaded us as an interpreter
     * for another ELF. The app's segments are already mapped. We just need to:
     *   1. Find the app's load_bias via AT_PHDR
     *   2. Load dependencies (DT_NEEDED) via mmap
     *   3. Perform relocations
     *   4. Jump to AT_ENTRY
     */
    if (interp_base != 0 && app_phdr && app_phnum > 0 && app_entry != 0) {
        /* 
         * Compute app's load_bias from AT_PHDR.
         * With the PT_PHDR segment (SIZEOF_HEADERS layout), AT_PHDR points
         * to the PHDR table at link-time vaddr 0x40 within the ELF file.
         */
        unsigned long app_load_bias = 0;

        /* Method 1: Find PT_PHDR segment - p_vaddr is its link-time address */
        for (int i = 0; i < app_phnum; i++) {
            if (app_phdr[i].p_type == ELF_PT_PHDR) {
                app_load_bias = (unsigned long)app_phdr - app_phdr[i].p_vaddr;
                break;
            }
        }

        /* Method 2: The PHDR table is always at e_phoff=0x40 from the ELF header.
         * Since the ELF header is at vaddr 0, AT_PHDR = load_bias + 0x40.
         * So load_bias = AT_PHDR_va - 0x40.
         */
        if (app_load_bias == 0) {
            Elf64_Ehdr *ehdr = (Elf64_Ehdr *)((unsigned long)app_phdr - 64);
            if (ehdr->e_ident[0] == 0x7f && ehdr->e_ident[1] == 'E') {
                app_load_bias = (unsigned long)ehdr;
            }
        }

        ld_log("App: ");
        if (app_path) ld_log(app_path);
        ld_log(" bias=");
        ld_log_hex(app_load_bias);
        ld_log(" interp=");
        ld_log_hex(interp_base);
        ld_log("\n");

        /* Initialize object list */
        g_object_count = 0;

        /* Register main app (already mapped by kernel) */
        struct ld_object *main_obj = ld_init_object_from_phdr(
            app_path ? app_path : "main", app_phdr, app_phnum, app_load_bias);

        if (main_obj) {
            /* Register ld.so itself (for symbol lookup by the app/libs) */
            Elf64_Ehdr *interp_ehdr = (Elf64_Ehdr *)interp_base;
            Elf64_Phdr *interp_phdr = (Elf64_Phdr *)(interp_base + interp_ehdr->e_phoff);
            ld_init_object_from_phdr("ld.so", interp_phdr, interp_ehdr->e_phnum, interp_base);

            /* Relocate all objects (app + its DT_NEEDED libs) */
            for (int i = 0; i < g_object_count; i++) {
                ld_relocate_object(&g_objects[i]);
            }

            /* Call init routines (dependencies first, then app) */
            for (int i = g_object_count - 1; i >= 0; i--) {
                ld_call_init(&g_objects[i]);
            }

            entry_point = app_entry;
        }
    } else {
        /*
         * Fallback / standalone mode:
         * - AT_BASE == 0: ld.so was invoked directly (not as interpreter)
         * - No auxv: older kernel or static load
         * Manually load the app from the filesystem.
         */
        if (!app_path || ld_strcmp(app_path, "/lib/ld.so") == 0) {
            ld_log("Standalone mode, loading default app.\n");
            app_path = "/bin/hello.elf";
        }
        ld_log("Loading: ");
        ld_log(app_path);
        ld_log("\n");
        unsigned long bias = 0;
        entry_point = ld_load_elf(app_path, &bias);
    }

    if (entry_point == 0) {
        ld_puts("Failed to load application.\n");
        ld_exit(1);
    }

    ld_log("Jumping to entry...\n");

    /* Restore original stack pointer and jump to app entry */
    asm volatile (
        "mov sp, %1\n"
        "br %0\n"
        : : "r"(entry_point), "r"(stack) : "memory"
    );

    while(1);
}
