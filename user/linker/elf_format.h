#ifndef ELF_FORMAT_H
#define ELF_FORMAT_H

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
#define ELF_PF_R 0x4U

#define DT_NULL 0L
#define DT_NEEDED 1L
#define DT_PLTRELSZ 2L
#define DT_PLTGOT 3L
#define DT_HASH 4L
#define DT_STRTAB 5L
#define DT_SYMTAB 6L
#define DT_RELA 7L
#define DT_RELASZ 8L
#define DT_RELAENT 9L
#define DT_STRSZ 10L
#define DT_SYMENT 11L
#define DT_SONAME 14L
#define DT_PLTREL 20L
#define DT_JMPREL 23L
#define DT_RELACOUNT 0x6ffffff9L
#define DT_GNU_HASH  0x6ffffef5L

#define R_AARCH64_ABS64 257U
#define R_AARCH64_GLOB_DAT 1025U
#define R_AARCH64_JUMP_SLOT 1026U
#define R_AARCH64_RELATIVE 1027U

#define ELF64_R_TYPE(info) ((info) & 0xffffffffU)
#define ELF64_R_SYM(info) ((info) >> 32)

typedef unsigned int Elf64_Word;

typedef struct {
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
} Elf64_Ehdr;

typedef struct {
    unsigned int p_type;
    unsigned int p_flags;
    unsigned long p_offset;
    unsigned long p_vaddr;
    unsigned long p_paddr;
    unsigned long p_filesz;
    unsigned long p_memsz;
    unsigned long p_align;
} Elf64_Phdr;

typedef struct {
    long d_tag;
    union {
        unsigned long d_val;
        unsigned long d_ptr;
    } d_un;
} Elf64_Dyn;

typedef struct {
    unsigned long r_offset;
    unsigned long r_info;
    long r_addend;
} Elf64_Rela;

typedef struct {
    unsigned int st_name;
    unsigned char st_info;
    unsigned char st_other;
    unsigned short st_shndx;
    unsigned long st_value;
    unsigned long st_size;
} Elf64_Sym;

#endif
