#include <kernel/mmu.h>
#include <kernel/log.h>
#include <kernel/mmu_debug.h>
#include <kernel/page_alloc.h>
#include <kernel/vm.h>
#include <arch/arm/sysregs.h>

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

#define MMU_DEBUG_VECTOR_PAGE  0x40080000UL
#define MMU_DEBUG_SYNC_PAGE    0x40081000UL
#define MMU_DEBUG_MMU_PAGE     0x40082000UL
#define MMU_DEBUG_BLOCK_PAGE   0x40400000UL

struct mmu_debug_target {
    const char *name;
    unsigned long address;
};

unsigned long *mmu_debug_ttbr0_root(void)
{
    return mmu_l0_table;
}

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
    return mmu_probe_address_s1e1r(address);
}

void mmu_debug_walk_address(unsigned long address)
{
    if (mmu_debug_ttbr0_root() == (unsigned long *)0) {
        KER_INFO("walk unavailable: mmu tables not initialized");
        return;
    }

    mmu_debug_walk(address);
}
