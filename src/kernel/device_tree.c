#include <kernel/device_tree.h>

struct fdt_header {
    unsigned int magic;
    unsigned int totalsize;
    unsigned int off_dt_struct;
    unsigned int off_dt_strings;
    unsigned int off_mem_rsvmap;
    unsigned int version;
    unsigned int last_comp_version;
    unsigned int boot_cpuid_phys;
    unsigned int size_dt_strings;
    unsigned int size_dt_struct;
};

#define FDT_MAGIC 0xd00dfeedU

static unsigned long device_tree_pa;
static unsigned long device_tree_size;
static int device_tree_valid;

int device_tree_init(unsigned long fdt_pa)
{
    const struct fdt_header *header;

    device_tree_pa = 0UL;
    device_tree_size = 0UL;
    device_tree_valid = 0;

    if (fdt_pa == 0UL) {
        return 0;
    }

    header = (const struct fdt_header *)(unsigned long)fdt_pa;
    if (header->magic != FDT_MAGIC || header->totalsize == 0U) {
        return 0;
    }

    device_tree_pa = fdt_pa;
    device_tree_size = (unsigned long)header->totalsize;
    device_tree_valid = 1;
    return 1;
}

int device_tree_is_valid(void)
{
    return device_tree_valid;
}

unsigned long device_tree_blob_pa(void)
{
    return device_tree_pa;
}

unsigned long device_tree_blob_size(void)
{
    return device_tree_size;
}