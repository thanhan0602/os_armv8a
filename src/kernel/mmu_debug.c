#include <kernel/mmu.h>
#include <kernel/log.h>
#include <kernel/mmu_debug.h>
#include <kernel/page_alloc.h>
#include <kernel/vm.h>

#define MMU_USE_4LEVEL 1
#define MMU_DEBUG_WALK_ENABLED 1

#define MMU_VA_BITS          48UL
#define MMU_L1_BLOCK_SIZE    0x40000000UL
#define MMU_L2_BLOCK_SIZE    0x00200000UL

#define MMU_DESC_VALID         (1UL << 0)
#define MMU_DESC_TABLE         (MMU_DESC_VALID | (1UL << 1))
#define MMU_DESC_BLOCK         MMU_DESC_VALID
#define MMU_DESC_PAGE          MMU_DESC_TABLE
#define MMU_DESC_TYPE_MASK      0x3UL
#define MMU_DESC_ADDR_MASK      0x0000fffffffff000UL
#define MMU_L1_BLOCK_ADDR_MASK  0x0000ffffc0000000UL
#define MMU_L2_BLOCK_ADDR_MASK  0x0000ffffffe00000UL
#define MMU_L3_PAGE_ADDR_MASK   0x0000fffffffff000UL

#define MMU_DEBUG_MAX_TABLE_PAGES 67UL
#define MMU_DEBUG_MAX_TABLE_NAME_LEN 24UL
#define MMU_DEBUG_VECTOR_PAGE  0x40080000UL
#define MMU_DEBUG_SYNC_PAGE    0x40081000UL
#define MMU_DEBUG_MMU_PAGE     0x40082000UL
#define MMU_DEBUG_BLOCK_PAGE   0x40400000UL

struct mmu_debug_target {
    const char *name;
    unsigned long address;
};

static unsigned long l0_index_for(unsigned long address)
{
    return (address >> 39) & 0x1ffUL;
}

static unsigned long l1_index_for(unsigned long address)
{
    return (address >> 30) & 0x1ffUL;
}

static unsigned long l2_index_for(unsigned long address)
{
    return (address >> 21) & 0x1ffUL;
}

static unsigned long l3_index_for(unsigned long address)
{
    return (address >> 12) & 0x1ffUL;
}

#define MMU_ATTR_INDEX(x)      ((unsigned long)(x) << 2)
#define MMU_ATTR_INDEX_MASK    MMU_ATTR_INDEX(0x7UL)
#define MMU_ATTR_DEVICE        MMU_ATTR_INDEX(0)
#define MMU_ATTR_NORMAL        MMU_ATTR_INDEX(1)

#define MMU_AP_RW              (0UL << 6)
#define MMU_AP_RO              (2UL << 6)
#define MMU_SH_NON_SHAREABLE   (0UL << 8)
#define MMU_SH_INNER_SHAREABLE (3UL << 8)
#define MMU_AF                 (1UL << 10)
#define MMU_PXN                (1UL << 53)
#define MMU_UXN                (1UL << 54)

extern char __text_start[];
extern char __text_end[];
extern char __rodata_start[];
extern char __rodata_end[];
extern char __data_start[];
extern char __data_end[];
extern char __stack_guard[];
extern char __stack_guard_end[];
extern char __stack_bottom[];
extern char __stack_top[];
extern char __bss_start[];
extern char __bss_end[];

const char *mmu_region_name(unsigned long address)
{
    if (address >= (unsigned long)__text_start && address < (unsigned long)__text_end) {
        return ".text";
    }

    if (address >= (unsigned long)__rodata_start && address < (unsigned long)__rodata_end) {
        return ".rodata";
    }

    if (address >= (unsigned long)__data_start && address < (unsigned long)__data_end) {
        return ".data";
    }

    if (address >= (unsigned long)__stack_guard && address < (unsigned long)__stack_guard_end) {
        return "stack-guard";
    }

    if (address >= (unsigned long)__stack_bottom && address < (unsigned long)__stack_top) {
        return "boot-stack";
    }

    if (address >= (unsigned long)__bss_start && address < (unsigned long)__bss_end) {
        return ".bss";
    }

    return "mmio-or-unmapped";
}

void mmu_debug_print_index(const char *level_name, unsigned long index)
{
    KER_LOGF("[info] %s index=%lu\n", level_name, index);
}

void mmu_debug_print_entry(const char *level_name, unsigned long entry)
{
    KER_LOGF("[info] %s entry=%lx\n", level_name, entry);
}

void mmu_debug_print_leaf_attrs(unsigned long address, unsigned long entry)
{
    const char *mem_kind;
    const char *ap_kind;
    const char *exec_kind;
    const char *share_kind;
    const char *af_kind;

    if ((entry & MMU_ATTR_INDEX_MASK) == MMU_ATTR_DEVICE) {
        mem_kind = "device";
    } else if ((entry & MMU_ATTR_INDEX_MASK) == MMU_ATTR_NORMAL) {
        mem_kind = "normal";
    } else {
        mem_kind = "unknown";
    }

    if ((entry & MMU_AP_RO) == MMU_AP_RO) {
        ap_kind = "ro";
    } else {
        ap_kind = "rw";
    }

    if ((entry & MMU_PXN) != 0) {
        exec_kind = "nx";
    } else {
        exec_kind = "x";
    }

    if ((entry & MMU_SH_INNER_SHAREABLE) == MMU_SH_INNER_SHAREABLE) {
        share_kind = "inner";
    } else {
        share_kind = "non";
    }

    if ((entry & MMU_AF) != 0) {
        af_kind = "1";
    } else {
        af_kind = "0";
    }

    KER_LOGF("[info] walk attrs: mem=%s ap=%s exec=%s sh=%s af=%s section=%s\n",
             mem_kind,
             ap_kind,
             exec_kind,
             share_kind,
             af_kind,
             mmu_region_name(address));
}

void mmu_debug_print_desc_kind(const char *level_name, const char *kind)
{
    KER_LOGF("[info] %s type=%s\n", level_name, kind);
}

/* Table page inventory recorded while page tables are allocated. */
unsigned long mmu_table_page_addresses[MMU_DEBUG_MAX_TABLE_PAGES];
char mmu_table_page_names[MMU_DEBUG_MAX_TABLE_PAGES][MMU_DEBUG_MAX_TABLE_NAME_LEN];
unsigned long mmu_table_page_count = 0UL;
unsigned long table_pages_used = 0UL;

static void mmu_debug_copy_name_internal(char *destination, const char *source)
{
    unsigned long index;

    for (index = 0UL; index < (MMU_DEBUG_MAX_TABLE_NAME_LEN - 1UL) && source[index] != '\0'; index++) {
        destination[index] = source[index];
    }
    destination[index] = '\0';
}

void mmu_debug_copy_name(char *destination, const char *source)
{
    mmu_debug_copy_name_internal(destination, source);
}

int mmu_debug_name_has_prefix(const char *name, const char *prefix)
{
    unsigned long index;

    for (index = 0UL; prefix[index] != '\0'; index++) {
        if (name[index] != prefix[index]) {
            return 0;
        }
    }

    return 1;
}

void mmu_debug_record_table_page(unsigned long address, const char *name)
{
    if (mmu_table_page_count >= MMU_DEBUG_MAX_TABLE_PAGES) {
        return;
    }

    mmu_table_page_addresses[mmu_table_page_count] = address;
    mmu_debug_copy_name_internal(mmu_table_page_names[mmu_table_page_count], name);
    mmu_table_page_count++;
    table_pages_used++;
}

void mmu_debug_set_chunk_name(char *destination, const char *prefix, unsigned long chunk_index)
{
    char digits[21];
    unsigned long digit_count;
    unsigned long value;
    unsigned long index;
    unsigned long output_index;

    value = chunk_index;
    digit_count = 0UL;
    do {
        digits[digit_count++] = (char)('0' + (value % 10UL));
        value /= 10UL;
    } while (value != 0UL && digit_count < sizeof(digits));

    output_index = 0UL;
    for (index = 0UL; prefix[index] != '\0' && output_index < (MMU_DEBUG_MAX_TABLE_NAME_LEN - 1UL); index++) {
        destination[output_index++] = prefix[index];
    }

    while (digit_count > 0UL && output_index < (MMU_DEBUG_MAX_TABLE_NAME_LEN - 1UL)) {
        destination[output_index++] = digits[--digit_count];
    }

    destination[output_index] = '\0';
}

unsigned long mmu_debug_table_page_count(void)
{
    return mmu_table_page_count;
}

unsigned long mmu_debug_table_page_address(unsigned long index)
{
    if (index >= mmu_table_page_count) {
        return 0UL;
    }

    return mmu_table_page_addresses[index];
}

const char *mmu_debug_table_page_name(unsigned long index)
{
    if (index >= mmu_table_page_count) {
        return "mmu-table-invalid";
    }

    return mmu_table_page_names[index];
}

unsigned long mmu_table_pages_used(void)
{
    return table_pages_used;
}

/* Boot targets */
static const struct mmu_debug_target mmu_boot_debug_targets[] = {
    { "vector", MMU_DEBUG_VECTOR_PAGE },
    { "sync", MMU_DEBUG_SYNC_PAGE },
    { "mmu", MMU_DEBUG_MMU_PAGE },
    { "bss", (unsigned long)__bss_start },
    { "stack", (unsigned long)__stack_bottom },
    { "block", MMU_DEBUG_BLOCK_PAGE },
};

unsigned long mmu_debug_boot_target_count(void)
{
    return sizeof(mmu_boot_debug_targets) / sizeof(mmu_boot_debug_targets[0]);
}

const char *mmu_debug_boot_target_name(unsigned long index)
{
    if (index >= mmu_debug_boot_target_count()) {
        return "mmu-boot-target-invalid";
    }

    return mmu_boot_debug_targets[index].name;
}

unsigned long mmu_debug_boot_target_address(unsigned long index)
{
    if (index >= mmu_debug_boot_target_count()) {
        return 0UL;
    }

    return mmu_boot_debug_targets[index].address;
}

/* Compact table pages and free non-ttbr1 pages when installing empty ttbr0 root */
void mmu_debug_compact_for_ttbr0_install(unsigned long new_root)
{
    unsigned long read_index;
    unsigned long write_index;

    write_index = 0UL;
    for (read_index = 0UL; read_index < mmu_table_page_count; read_index++) {
        const char *name = mmu_table_page_names[read_index];
        if (mmu_debug_name_has_prefix(name, "t1-")) {
            if (write_index != read_index) {
                mmu_table_page_addresses[write_index] = mmu_table_page_addresses[read_index];
                mmu_debug_copy_name_internal(mmu_table_page_names[write_index], name);
            }
            write_index++;
            continue;
        }

        page_free((void *)mmu_table_page_addresses[read_index]);
        if (table_pages_used > 0) table_pages_used--;
    }

    mmu_table_page_count = write_index;
    mmu_debug_record_table_page(new_root, "t0-empty-root");
}

void mmu_debug_reset(void)
{
    mmu_table_page_count = 0UL;
    table_pages_used = 0UL;
}

static unsigned long *mmu_debug_desc_pa_to_table(unsigned long pa)
{
    if (mmu_is_enabled()) {
        return (unsigned long *)pa_to_va(pa);
    }

    return (unsigned long *)pa;
}

static unsigned long *mmu_debug_root_table_for_walk(void)
{
    unsigned long *root;

    root = mmu_debug_ttbr0_root();
    if (root == (unsigned long *)0) {
        return (unsigned long *)0;
    }

    return mmu_debug_desc_pa_to_table((unsigned long)root);
}

static void mmu_debug_walk(unsigned long address)
{
    unsigned long l0_index;
    unsigned long l1_index;
    unsigned long l2_index;
    unsigned long l3_index;
    unsigned long l0_entry;
    unsigned long l1_entry;
    unsigned long l2_entry;
    unsigned long l3_entry;
    unsigned long *l0_walk_table;
    unsigned long *l1_walk_table;
    unsigned long *l2_walk_table;
    unsigned long *l3_walk_table;
    unsigned long pa;

    KER_LOGF("[info] walk va=%lx\n", address);
    KER_LOGF("[info] walk region=%s\n", mmu_region_name(address));

    l0_walk_table = mmu_debug_root_table_for_walk();
    if (l0_walk_table == (unsigned long *)0) {
        KER_INFO("walk unavailable: no active ttbr0 root");
        return;
    }

    l0_index = l0_index_for(address);
    l0_entry = l0_walk_table[l0_index];
    mmu_debug_print_index("walk l0", l0_index);
    mmu_debug_print_entry("walk l0", l0_entry);
    if ((l0_entry & MMU_DESC_VALID) == 0) {
        mmu_debug_print_desc_kind("walk l0", "invalid");
        KER_INFO("walk stopped: invalid l0 entry");
        return;
    }
    mmu_debug_print_desc_kind("walk l0", "table");

    l1_walk_table = mmu_debug_desc_pa_to_table(l0_entry & MMU_DESC_ADDR_MASK);
    l1_index = l1_index_for(address);
    l1_entry = l1_walk_table[l1_index];
    mmu_debug_print_index("walk l1", l1_index);
    mmu_debug_print_entry("walk l1", l1_entry);
    if ((l1_entry & MMU_DESC_VALID) == 0) {
        mmu_debug_print_desc_kind("walk l1", "invalid");
        KER_INFO("walk stopped: invalid l1 entry");
        return;
    }

    if ((l1_entry & MMU_DESC_TYPE_MASK) == MMU_DESC_BLOCK) {
        mmu_debug_print_desc_kind("walk l1", "block");
        mmu_debug_print_leaf_attrs(address, l1_entry);
        pa = (l1_entry & MMU_L1_BLOCK_ADDR_MASK) | (address & (MMU_L1_BLOCK_SIZE - 1UL));
        KER_LOGF("[info] walk pa from l1 block=%lx\n", pa);
        return;
    }
    mmu_debug_print_desc_kind("walk l1", "table");

    l2_walk_table = mmu_debug_desc_pa_to_table(l1_entry & MMU_DESC_ADDR_MASK);
    l2_index = l2_index_for(address);
    l2_entry = l2_walk_table[l2_index];
    mmu_debug_print_index("walk l2", l2_index);
    mmu_debug_print_entry("walk l2", l2_entry);
    if ((l2_entry & MMU_DESC_VALID) == 0) {
        mmu_debug_print_desc_kind("walk l2", "invalid");
        KER_INFO("walk stopped: invalid l2 entry");
        return;
    }

    if ((l2_entry & MMU_DESC_TYPE_MASK) == MMU_DESC_BLOCK) {
        mmu_debug_print_desc_kind("walk l2", "block");
        mmu_debug_print_leaf_attrs(address, l2_entry);
        pa = (l2_entry & MMU_L2_BLOCK_ADDR_MASK) | (address & (MMU_L2_BLOCK_SIZE - 1UL));
        KER_LOGF("[info] walk pa from l2 block=%lx\n", pa);
        return;
    }
    mmu_debug_print_desc_kind("walk l2", "table");

    l3_walk_table = mmu_debug_desc_pa_to_table(l2_entry & MMU_DESC_ADDR_MASK);
    l3_index = l3_index_for(address);
    l3_entry = l3_walk_table[l3_index];
    mmu_debug_print_index("walk l3", l3_index);
    mmu_debug_print_entry("walk l3", l3_entry);
    if ((l3_entry & MMU_DESC_VALID) == 0) {
        mmu_debug_print_desc_kind("walk l3", "invalid");
        KER_INFO("walk stopped: invalid l3 entry");
        return;
    }

    mmu_debug_print_desc_kind("walk l3", "page");
    mmu_debug_print_leaf_attrs(address, l3_entry);
    pa = (l3_entry & MMU_L3_PAGE_ADDR_MASK) | (address & (PAGE_SIZE - 1UL));
    KER_LOGF("[info] walk pa from l3 page=%lx\n", pa);
}

unsigned long mmu_debug_probe_address(unsigned long address)
{
    unsigned long par_el1;

    __asm__ volatile(
        "at s1e1r, %1\n"
        "isb\n"
        "mrs %0, par_el1\n"
        : "=r"(par_el1)
        : "r"(address)
        : "memory");

    return par_el1;
}

void mmu_debug_walk_address(unsigned long address)
{
    if (mmu_debug_ttbr0_root() == (unsigned long *)0) {
        KER_INFO("walk unavailable: mmu tables not initialized");
        return;
    }

    mmu_debug_walk(address);
}

/* Allocate a page, record it with a generated chunk name, and return pointer. */
unsigned long *mmu_debug_alloc_named_table_page_chunk(const char *prefix, unsigned long chunk_index)
{
    unsigned long *table = (unsigned long *)page_alloc();
    if (table == (unsigned long *)0) {
        return (unsigned long *)0;
    }

    if (mmu_table_page_count < MMU_DEBUG_MAX_TABLE_PAGES) {
        mmu_debug_set_chunk_name(mmu_table_page_names[mmu_table_page_count], prefix, chunk_index);
        mmu_debug_record_table_page((unsigned long)table, mmu_table_page_names[mmu_table_page_count]);
    }

    return table;
}
