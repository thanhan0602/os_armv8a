#include <kernel/mmu.h>

#include <arch/arm/virt.h>
#include <kernel/debug_targets.h>
#include <kernel/heap.h>
#include <kernel/log.h>
#include <kernel/page_alloc.h>
#include <kernel/vm.h>

/*
 * Stage 6/7 MMU design summary:
 * - EL1 Stage-1 translation only
 * - 48-bit virtual address space with 4 KiB pages
 * - 4-level walk rooted at TTBR0_EL1
 * - broad identity map kept on purpose for bring-up and easier debug
 * - RAM starts at L1 -> L2, then the early kernel window drops to L3 pages so
 *   .text/.rodata/.data/.bss/boot stack can use different permissions
 * - the rest of RAM stays mapped by larger L2 blocks to reduce table pressure
 */
#define MMU_USE_4LEVEL 1
#define MMU_DEBUG_WALK_ENABLED 1
#define MMU_VA_BITS          48UL
#define MMU_L1_BLOCK_SIZE    0x40000000UL
#define MMU_T0SZ             (64UL - MMU_VA_BITS)
#define MMU_L2_BLOCK_SIZE    0x00200000UL
#define MMU_KERNEL_FINE_MAP_MIN_CHUNKS 2UL

#define MMU_DESC_VALID         (1UL << 0)
#define MMU_DESC_TABLE         (MMU_DESC_VALID | (1UL << 1))
#define MMU_DESC_BLOCK         MMU_DESC_VALID
#define MMU_DESC_PAGE          MMU_DESC_TABLE

/*
 * Descriptor bits used by this kernel's Stage-1 translation tables:
 * - AttrIndx[2:0] selects a MAIR_EL1 memory type slot
 * - AP[7:6] controls read/write permission at EL1
 * - SH[9:8] selects shareability for cacheable memory
 * - AF[10] must be set so the first access does not fault on access-flag checks
 * - PXN/UXN forbid instruction fetches from privileged/user execution domains
 */
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

#define MMU_MAIR_DEVICE_nGnRnE 0x00UL
#define MMU_MAIR_NORMAL_WBWA   0xffUL
#define MMU_TCR_IPS_48BIT      (5UL << 32)
#ifdef CONFIG_KERNEL_VIRTUAL
#define MMU_T1SZ               (16UL << 16)
#define MMU_TG1_4K             (2UL << 30)
#endif

/*
 * SCTLR_EL1 bits used when turning the MMU on:
 * - M[0] enables Stage-1 address translation
 * - C[2] enables data/unified caches for normal memory
 * - I[12] enables instruction cache
 * The RES1 bits are architectural mandatory-one fields for EL1.
 */
#define SCTLR_EL1_RES1         ((1UL << 29) | (1UL << 28) | (1UL << 23) | (1UL << 22) | (1UL << 20) | (1UL << 11))
#define SCTLR_EL1_M            (1UL << 0)
#define SCTLR_EL1_C            (1UL << 2)
#define SCTLR_EL1_I            (1UL << 12)

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

/*
 * Current mapping policy:
 * - L0 holds the single top-level entry used by the current identity map
 * - L1[0] covers low MMIO as a device block mapping
 * - L1 entry for RAM points to an L2 table
 * - early RAM chunks use per-page L3 entries for precise permissions
 * - later RAM chunks use coarse L2 block entries as RW/NX normal memory
 */
static unsigned long *l0_table;
static unsigned long *l1_table;
static unsigned long *l2_ram_table;
static unsigned long mmu_table_page_addresses[MMU_DEBUG_MAX_TABLE_PAGES];
static char mmu_table_page_names[MMU_DEBUG_MAX_TABLE_PAGES][MMU_DEBUG_MAX_TABLE_NAME_LEN];
static unsigned long mmu_table_page_count;
static unsigned long table_pages_used;
static int mmu_enabled;
static unsigned long fine_map_chunks_used;

#ifdef CONFIG_KERNEL_VIRTUAL
/* TTBR1 page-table root pointers for the kernel virtual address space. */
static unsigned long *l0_table_ttbr1;
static unsigned long *l1_table_ttbr1;
static unsigned long *l2_ram_table_ttbr1;
static unsigned long *ttbr0_runtime_empty_root;
#endif

extern char __text_start[];
extern char __text_end[];
extern char __rodata_start[];
extern char __rodata_end[];
extern char __data_start[];
extern char __data_end[];
extern char __bss_start[];
extern char __bss_end[];
extern char __stack_bottom[];
extern char __stack_top[];
extern char __stack_guard[];
extern char __stack_guard_end[];
extern char __kernel_end[];

struct mmu_debug_target {
    const char *name;
    unsigned long address;
};

static const struct mmu_debug_target mmu_boot_debug_targets[] = {
    { "vector", MMU_DEBUG_VECTOR_PAGE },
    { "sync", MMU_DEBUG_SYNC_PAGE },
    { "mmu", MMU_DEBUG_MMU_PAGE },
    { "bss", (unsigned long)__bss_start },
    { "stack", (unsigned long)__stack_bottom },
    { "block", MMU_DEBUG_BLOCK_PAGE },
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

static unsigned long align_up(unsigned long value, unsigned long alignment)
{
    return (value + alignment - 1UL) & ~(alignment - 1UL);
}

static void mmu_debug_copy_name(char *destination, const char *source)
{
    unsigned long index;

    for (index = 0UL; index < (MMU_DEBUG_MAX_TABLE_NAME_LEN - 1UL) && source[index] != '\0'; index++) {
        destination[index] = source[index];
    }
    destination[index] = '\0';
}

static int mmu_debug_name_has_prefix(const char *name, const char *prefix)
{
    unsigned long index;

    for (index = 0UL; prefix[index] != '\0'; index++) {
        if (name[index] != prefix[index]) {
            return 0;
        }
    }

    return 1;
}

static void mmu_debug_record_table_page(unsigned long address, const char *name)
{
    if (mmu_table_page_count >= MMU_DEBUG_MAX_TABLE_PAGES) {
        return;
    }

    mmu_table_page_addresses[mmu_table_page_count] = address;
    mmu_debug_copy_name(mmu_table_page_names[mmu_table_page_count], name);
    mmu_table_page_count++;
}

static void mmu_debug_set_chunk_name(char *destination, const char *prefix, unsigned long chunk_index)
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

static unsigned long *alloc_named_table_page(const char *name)
{
    unsigned long *table;

    table = (unsigned long *)page_alloc();
    if (table != (unsigned long *)0) {
        mmu_debug_record_table_page((unsigned long)table, name);
        table_pages_used++;
    }

    return table;
}

static const char *mmu_region_name(unsigned long address)
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

    if (address >= QEMU_VIRT_RAM_BASE && address < QEMU_VIRT_RAM_END) {
        return "ram-other";
    }

    return "mmio-or-unmapped";
}

/*
 * The fine-mapped kernel region is split by linker sections so we can keep
 * .text executable, .rodata read-only, and writable data non-executable.
 */
static unsigned long kernel_page_attrs(unsigned long address)
{
    /* Guard page: intentionally unmapped to catch stack overflow. */
    if (address >= (unsigned long)__stack_guard && address < (unsigned long)__stack_guard_end) {
        return 0;
    }

    if (address >= (unsigned long)__text_start && address < (unsigned long)__text_end) {
        return MMU_ATTR_NORMAL |
               MMU_AP_RO |
               MMU_SH_INNER_SHAREABLE |
               MMU_AF;
    }

    if (address >= (unsigned long)__rodata_start && address < (unsigned long)__rodata_end) {
        return MMU_ATTR_NORMAL |
               MMU_AP_RO |
               MMU_SH_INNER_SHAREABLE |
               MMU_AF |
               MMU_PXN |
               MMU_UXN;
    }

    if (address >= (unsigned long)__data_start && address < (unsigned long)__data_end) {
        return MMU_ATTR_NORMAL |
               MMU_AP_RW |
               MMU_SH_INNER_SHAREABLE |
               MMU_AF |
               MMU_PXN |
               MMU_UXN;
    }

    if (address >= (unsigned long)__bss_start && address < (unsigned long)__bss_end) {
        return MMU_ATTR_NORMAL |
               MMU_AP_RW |
               MMU_SH_INNER_SHAREABLE |
               MMU_AF |
               MMU_PXN |
               MMU_UXN;
    }

    if (address >= (unsigned long)__stack_bottom && address < (unsigned long)__stack_top) {
        return MMU_ATTR_NORMAL |
               MMU_AP_RW |
               MMU_SH_INNER_SHAREABLE |
               MMU_AF |
               MMU_PXN |
               MMU_UXN;
    }

    return MMU_ATTR_NORMAL |
           MMU_AP_RW |
           MMU_SH_INNER_SHAREABLE |
           MMU_AF |
           MMU_PXN |
           MMU_UXN;
}

#if MMU_USE_4LEVEL && MMU_DEBUG_WALK_ENABLED
static void mmu_debug_print_index(const char *level_name, unsigned long index)
{
    KER_LOGF("[info] %s index=%lu\n", level_name, index);
}

static void mmu_debug_print_entry(const char *level_name, unsigned long entry)
{
    KER_LOGF("[info] %s entry=%lx\n", level_name, entry);
}

static void mmu_debug_print_leaf_attrs(unsigned long address, unsigned long entry)
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

static void mmu_debug_print_desc_kind(const char *level_name, const char *kind)
{
    KER_LOGF("[info] %s type=%s\n", level_name, kind);
}

/*
 * Software walk helper for bring-up: it follows the page-table pointers we just
 * built in RAM and prints where translation stops. Descriptor addresses are always
 * physical. Before the MMU is enabled we dereference them directly; after, we
 * access them through the TTBR1 kernel VA.
 */
static unsigned long *desc_pa_to_table(unsigned long pa)
{
    if (mmu_enabled) {
        return (unsigned long *)pa_to_va(pa);
    }

    return (unsigned long *)pa;
}

static unsigned long *mmu_root_table_for_walk(void)
{
    if (l0_table == (unsigned long *)0) {
        return (unsigned long *)0;
    }

    return desc_pa_to_table((unsigned long)l0_table);
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

    l0_walk_table = mmu_root_table_for_walk();
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

    l1_walk_table = desc_pa_to_table(l0_entry & MMU_DESC_ADDR_MASK);
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

    l2_walk_table = desc_pa_to_table(l1_entry & MMU_DESC_ADDR_MASK);
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

    l3_walk_table = desc_pa_to_table(l2_entry & MMU_DESC_ADDR_MASK);
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

static unsigned long mmu_probe_translate(unsigned long address)
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
#endif

void mmu_debug_walk_address(unsigned long address)
{
#if MMU_USE_4LEVEL && MMU_DEBUG_WALK_ENABLED
    if (l0_table == (unsigned long *)0) {
        KER_INFO("walk unavailable: mmu tables not initialized");
        return;
    }

    mmu_debug_walk(address);
#else
    (void)address;
    KER_INFO("walk unavailable: MMU debug walk disabled at build time");
#endif
}

unsigned long mmu_debug_probe_address(unsigned long address)
{
#if MMU_USE_4LEVEL
    return mmu_probe_translate(address);
#else
    (void)address;
    return ~0UL;
#endif
}

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

static int build_identity_map(void)
{
    unsigned long fine_map_end;
    unsigned long kernel_map_end;
    unsigned long minimum_map_end;
    unsigned long chunk_base;
    unsigned long address;

    /*
     * Active table layout for the current kernel map:
     *
     *   TTBR0_EL1
     *      |
     *      v
     *     L0 root
     *      |
     *      +--> L1[0]          -> 1 GiB device block for low MMIO
     *      |
     *      +--> L1[RAM base]   -> L2 RAM table
     *                              |
     *                              +--> early entries -> L3 page tables
     *                              |                     (.text/.rodata/.data/.bss/stack)
     *                              |
     *                              +--> later entries -> 2 MiB normal-memory blocks
     *
     * The mapping remains identity-based: VA == PA for the current kernel.
     */

    /*
     * L1 block for the low physical region on QEMU virt.
     * This keeps MMIO strongly ordered as Device-nGnRnE and marks it NX.
     */
    l1_table[l1_index_for(0x00000000UL)] = 0x00000000UL |
                                           MMU_ATTR_DEVICE |
                                           MMU_AP_RW |
                                           MMU_SH_NON_SHAREABLE |
                                           MMU_AF |
                                           MMU_PXN |
                                           MMU_UXN |
                                           MMU_DESC_BLOCK;

    /* TTBR0_EL1 root -> L0 -> L1 for the active lower VA space. */
    l0_table[l0_index_for(0x00000000UL)] = ((unsigned long)l1_table) | MMU_DESC_TABLE;

    /*
     * RAM fans out through L2 so the first part can either terminate as an L2
     * block or continue into L3 page tables for fine-grained permissions.
     */
    l1_table[l1_index_for(QEMU_VIRT_RAM_BASE)] = ((unsigned long)l2_ram_table) | MMU_DESC_TABLE;

    kernel_map_end = align_up((unsigned long)__kernel_end, MMU_L2_BLOCK_SIZE);
    minimum_map_end = QEMU_VIRT_RAM_BASE + (MMU_KERNEL_FINE_MAP_MIN_CHUNKS * MMU_L2_BLOCK_SIZE);
    fine_map_end = kernel_map_end;
    if (fine_map_end < minimum_map_end) {
        fine_map_end = minimum_map_end;
    }

    fine_map_chunks_used = (fine_map_end - QEMU_VIRT_RAM_BASE) / MMU_L2_BLOCK_SIZE;

    /*
     * Allocate one L3 table per 2 MiB chunk in the fine-mapped window.
     * Each 4 KiB page can then carry the permission chosen by
     * kernel_page_attrs():
     * - .text   -> normal, RO, executable
     * - .rodata -> normal, RO, PXN/UXN
     * - .data/.bss/stack -> normal, RW, PXN/UXN
     */
    for (chunk_base = QEMU_VIRT_RAM_BASE; chunk_base < fine_map_end; chunk_base += MMU_L2_BLOCK_SIZE) {
        unsigned long *l3_table;
        unsigned long chunk_index;

        chunk_index = (chunk_base - QEMU_VIRT_RAM_BASE) / MMU_L2_BLOCK_SIZE;
        mmu_debug_set_chunk_name(mmu_table_page_names[mmu_table_page_count], "l3-chunk-", chunk_index);
        l3_table = alloc_named_table_page(mmu_table_page_names[mmu_table_page_count]);
        if (l3_table == (unsigned long *)0) {
            KER_INFO("mmu init failed: no free pages for l3 table");
            return 0;
        }

        l2_ram_table[l2_index_for(chunk_base)] = ((unsigned long)l3_table) | MMU_DESC_TABLE;

        for (address = chunk_base; address < chunk_base + MMU_L2_BLOCK_SIZE; address += PAGE_SIZE) {
            unsigned long attrs = kernel_page_attrs(address);

            if (attrs == 0)
                continue; /* guard page: leave entry invalid */
            l3_table[l3_index_for(address)] = (address & ~(PAGE_SIZE - 1UL)) |
                                              attrs |
                                              MMU_DESC_PAGE;
        }
    }

    /* The remaining RAM stays mapped as L2 normal-memory blocks, RW and NX. */
    for (address = fine_map_end; address < QEMU_VIRT_RAM_END; address += MMU_L2_BLOCK_SIZE) {
        l2_ram_table[l2_index_for(address)] = (address & ~(MMU_L2_BLOCK_SIZE - 1UL)) |
                                              MMU_ATTR_NORMAL |
                                              MMU_AP_RW |
                                              MMU_SH_INNER_SHAREABLE |
                                              MMU_AF |
                                              MMU_PXN |
                                              MMU_UXN |
                                              MMU_DESC_BLOCK;
    }

    return 1;
}

#ifdef CONFIG_KERNEL_VIRTUAL
/*
 * Build a second set of page tables for the kernel virtual address space
 * reachable through TTBR1_EL1.  The mapping covers the same physical
 * regions with the same permissions as the identity map.  Once active,
 * the CPU selects these tables whenever the upper VA bits are all ones
 * (bits[63:48] == 0xFFFF for T1SZ == 16), making every PA accessible at
 * VA = PA + 0xFFFF000000000000.
 *
 * The TTBR1 tables are independent allocations so the identity map in
 * TTBR0 can be removed later without touching the kernel tables.
 */
static int build_kernel_map(void)
{
    unsigned long fine_map_end;
    unsigned long kernel_map_end;
    unsigned long minimum_map_end;
    unsigned long chunk_base;
    unsigned long address;

    /* L1 device block — same PA as the identity map. */
    l1_table_ttbr1[l1_index_for(0x00000000UL)] = 0x00000000UL |
                                                   MMU_ATTR_DEVICE |
                                                   MMU_AP_RW |
                                                   MMU_SH_NON_SHAREABLE |
                                                   MMU_AF |
                                                   MMU_PXN |
                                                   MMU_UXN |
                                                   MMU_DESC_BLOCK;

    l0_table_ttbr1[l0_index_for(0x00000000UL)] = ((unsigned long)l1_table_ttbr1) | MMU_DESC_TABLE;

    l1_table_ttbr1[l1_index_for(QEMU_VIRT_RAM_BASE)] = ((unsigned long)l2_ram_table_ttbr1) | MMU_DESC_TABLE;

    kernel_map_end = align_up((unsigned long)__kernel_end, MMU_L2_BLOCK_SIZE);
    minimum_map_end = QEMU_VIRT_RAM_BASE + (MMU_KERNEL_FINE_MAP_MIN_CHUNKS * MMU_L2_BLOCK_SIZE);
    fine_map_end = kernel_map_end;
    if (fine_map_end < minimum_map_end) {
        fine_map_end = minimum_map_end;
    }

    for (chunk_base = QEMU_VIRT_RAM_BASE; chunk_base < fine_map_end; chunk_base += MMU_L2_BLOCK_SIZE) {
        unsigned long *l3_table;
        unsigned long chunk_index;

        chunk_index = (chunk_base - QEMU_VIRT_RAM_BASE) / MMU_L2_BLOCK_SIZE;
        mmu_debug_set_chunk_name(mmu_table_page_names[mmu_table_page_count], "t1-l3-chunk-", chunk_index);
        l3_table = alloc_named_table_page(mmu_table_page_names[mmu_table_page_count]);
        if (l3_table == (unsigned long *)0) {
            KER_INFO("mmu init failed: no free pages for t1 l3 table");
            return 0;
        }

        l2_ram_table_ttbr1[l2_index_for(chunk_base)] = ((unsigned long)l3_table) | MMU_DESC_TABLE;

        for (address = chunk_base; address < chunk_base + MMU_L2_BLOCK_SIZE; address += PAGE_SIZE) {
            unsigned long attrs = kernel_page_attrs(address);

            if (attrs == 0)
                continue; /* guard page: leave entry invalid */
            l3_table[l3_index_for(address)] = (address & ~(PAGE_SIZE - 1UL)) |
                                              attrs |
                                              MMU_DESC_PAGE;
        }
    }

    for (address = fine_map_end; address < QEMU_VIRT_RAM_END; address += MMU_L2_BLOCK_SIZE) {
        l2_ram_table_ttbr1[l2_index_for(address)] = (address & ~(MMU_L2_BLOCK_SIZE - 1UL)) |
                                                     MMU_ATTR_NORMAL |
                                                     MMU_AP_RW |
                                                     MMU_SH_INNER_SHAREABLE |
                                                     MMU_AF |
                                                     MMU_PXN |
                                                     MMU_UXN |
                                                     MMU_DESC_BLOCK;
    }

    return 1;
}
#endif /* CONFIG_KERNEL_VIRTUAL */

void mmu_init(void)
{
    unsigned long mair;
    unsigned long tcr;
    unsigned long sctlr;

    if (mmu_enabled) {
        return;
    }

    mmu_table_page_count = 0UL;

    l0_table = alloc_named_table_page("l0-root");
    l1_table = alloc_named_table_page("l1-root");
    l2_ram_table = alloc_named_table_page("l2-ram");
    if (l0_table == (unsigned long *)0 ||
        l1_table == (unsigned long *)0 ||
        l2_ram_table == (unsigned long *)0) {
        KER_INFO("mmu init failed: no free pages for page tables");
        return;
    }

#ifdef CONFIG_KERNEL_VIRTUAL
    l0_table_ttbr1 = alloc_named_table_page("t1-l0-root");
    l1_table_ttbr1 = alloc_named_table_page("t1-l1-root");
    l2_ram_table_ttbr1 = alloc_named_table_page("t1-l2-ram");
    if (l0_table_ttbr1 == (unsigned long *)0 ||
        l1_table_ttbr1 == (unsigned long *)0 ||
        l2_ram_table_ttbr1 == (unsigned long *)0) {
        KER_INFO("mmu init failed: no free pages for ttbr1 tables");
        return;
    }
#endif

    if (!build_identity_map()) {
        return;
    }

#ifdef CONFIG_KERNEL_VIRTUAL
    if (!build_kernel_map()) {
        return;
    }

#endif

    kernel_debug_log_mmu_boot_targets();

    /*
     * MAIR_EL1 encodes the memory types referenced by AttrIndx in descriptors:
     * - slot 0 = Device-nGnRnE for MMIO
     * - slot 1 = Normal WB/WA cacheable memory for RAM
     */
    mair = (MMU_MAIR_DEVICE_nGnRnE << 0) | (MMU_MAIR_NORMAL_WBWA << 8);
    /*
     * TCR_EL1 defines how TTBR0_EL1 (and TTBR1_EL1) addresses are translated:
     * - T0SZ[5:0]   = 16  -> 48-bit VA space for TTBR0
     * - IRGN0[9:8]  = 01  -> inner WB/WA cacheability for TTBR0 walks
     * - ORGN0[11:10]= 01  -> outer WB/WA cacheability for TTBR0 walks
     * - SH0[13:12]  = 11  -> inner-shareable TTBR0 walks
     * - TG0[15:14]  = 00  -> 4 KiB granule for TTBR0
     * When CONFIG_KERNEL_VIRTUAL:
     * - T1SZ[21:16] = 16  -> 48-bit VA space for TTBR1
     * - IRGN1[25:24]= 01  -> inner WB/WA cacheability for TTBR1 walks
     * - ORGN1[27:26]= 01  -> outer WB/WA cacheability for TTBR1 walks
     * - SH1[29:28]  = 11  -> inner-shareable TTBR1 walks
     * - TG1[31:30]  = 10  -> 4 KiB granule for TTBR1
     * Otherwise:
     * - EPD1[23]    = 1   -> disable TTBR1 translations
     * Common:
     * - IPS[34:32]  = 101 -> 48-bit physical address size
     */
    tcr = MMU_T0SZ |
          (1UL << 8) |
          (1UL << 10) |
          (3UL << 12) |
          (0UL << 14) |
#ifdef CONFIG_KERNEL_VIRTUAL
          MMU_T1SZ |
          (1UL << 24) |
          (1UL << 26) |
          (3UL << 28) |
          MMU_TG1_4K |
#else
          (1UL << 23) |  /* EPD1 = 1: disable TTBR1 translations */
#endif
          MMU_TCR_IPS_48BIT;

    /*
     * Bring the translation regime live in this order:
     * 1. MAIR_EL1  <- memory attribute slots used by descriptors
     * 2. TCR_EL1   <- translation size/shareability/cacheability/granule
     * 3. TTBR0_EL1 <- base address of the identity map L0 root table
     * 4. TTBR1_EL1 <- base address of the kernel VA L0 root table
     * 5. TLBI      <- discard any stale EL1 Stage-1 translations
     * 6. DSB/ISB   <- complete the register/TLB programming before SCTLR_EL1.M
     */
    __asm__ volatile(
        "dsb ish\n"
        "msr mair_el1, %0\n"
        "msr tcr_el1, %1\n"
        "msr ttbr0_el1, %2\n"
#ifdef CONFIG_KERNEL_VIRTUAL
        "msr ttbr1_el1, %3\n"
#endif
        "tlbi vmalle1\n"
        "dsb ish\n"
        "isb\n"
        :
#ifdef CONFIG_KERNEL_VIRTUAL
        : "r"(mair), "r"(tcr), "r"(l0_table), "r"(l0_table_ttbr1)
#else
        : "r"(mair), "r"(tcr), "r"(l0_table)
#endif
        : "memory");

    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));

    /* Enable the MMU and both caches with the required architectural RES1 bits. */
    sctlr = SCTLR_EL1_RES1 | SCTLR_EL1_M | SCTLR_EL1_C | SCTLR_EL1_I;

    /*
     * Writing SCTLR_EL1 is the commit point:
     * - M starts Stage-1 translation
     * - C allows data cache on normal memory
     * - I allows instruction cache
     * The trailing ISB makes later instructions execute under the new regime.
     */
    __asm__ volatile(
        "msr sctlr_el1, %0\n"
        "isb\n"
        :
        : "r"(sctlr)
        : "memory");

    mmu_enabled = 1;
}

int mmu_is_enabled(void)
{
    return mmu_enabled;
}

unsigned long mmu_table_pages_used(void)
{
    return table_pages_used;
}

#ifdef CONFIG_KERNEL_VIRTUAL
void mmu_install_empty_ttbr0_root(void)
{
    unsigned long *new_root;
    unsigned long read_index;
    unsigned long write_index;

    if (!mmu_enabled) {
        return;
    }

    if (ttbr0_runtime_empty_root != (unsigned long *)0 && l0_table == ttbr0_runtime_empty_root) {
        return;
    }

    new_root = (unsigned long *)page_alloc();
    if (new_root == (unsigned long *)0) {
        KER_INFO("ttbr0 empty-root install failed: no free pages");
        return;
    }

    __asm__ volatile(
        "msr ttbr0_el1, %0\n"
        "isb\n"
        "tlbi vmalle1\n"
        "dsb ish\n"
        "isb\n"
        :
        : "r"(new_root)
        : "memory");

    ttbr0_runtime_empty_root = new_root;
    l0_table = new_root;
    l1_table = (unsigned long *)0;
    l2_ram_table = (unsigned long *)0;

    write_index = 0UL;
    for (read_index = 0UL; read_index < mmu_table_page_count; read_index++) {
        const char *name;

        name = mmu_table_page_names[read_index];
        if (mmu_debug_name_has_prefix(name, "t1-")) {
            if (write_index != read_index) {
                mmu_table_page_addresses[write_index] = mmu_table_page_addresses[read_index];
                mmu_debug_copy_name(mmu_table_page_names[write_index], name);
            }
            write_index++;
            continue;
        }

        page_free((void *)mmu_table_page_addresses[read_index]);
        table_pages_used--;
    }

    mmu_table_page_count = write_index;
    mmu_debug_record_table_page((unsigned long)new_root, "t0-empty-root");
    table_pages_used++;

}

/*
 * ASID allocation state.
 * ASID 0 is reserved for kernel tasks / the empty lower-half root.
 * 8-bit ASIDs (TCR_EL1.AS=0): valid user range is 1–255.
 * Recycle ASIDs when processes are destroyed instead of assuming the
 * lifetime number of processes never exceeds the live-task limit.
 */
static unsigned int next_asid = 1;
static unsigned char asid_in_use[256];

static void mmu_tlbi_asid(unsigned int asid)
{
    unsigned long tlbi_val;

    if (asid == 0U) {
        return;
    }

    tlbi_val = (unsigned long)asid << 48;
    __asm__ volatile(
        "dsb ish\n"
        "tlbi aside1is, %0\n"
        "dsb ish\n"
        "isb\n"
        :
        : "r"(tlbi_val)
        : "memory");
}

static unsigned int mmu_asid_alloc(void)
{
    unsigned int attempts;
    unsigned int asid;

    for (attempts = 0U; attempts < 255U; attempts++) {
        asid = next_asid;
        next_asid++;
        if (next_asid == 0U) {
            next_asid = 1U;
        }

        if (asid == 0U || asid_in_use[asid] != 0U) {
            continue;
        }

        asid_in_use[asid] = 1U;
        mmu_tlbi_asid(asid);
        return asid;
    }

    return 0U;
}

static void mmu_asid_free(unsigned int asid)
{
    if (asid == 0U) {
        return;
    }

    mmu_tlbi_asid(asid);
    asid_in_use[asid] = 0U;
}

static int mmu_context_has_page(const struct mm_context *mm, unsigned long pa)
{
    unsigned int index;

    if (mm == (const struct mm_context *)0) {
        return 0;
    }

    for (index = 0U; index < mm->page_count; index++) {
        if (mm->pages[index] == pa) {
            return 1;
        }
    }

    return 0;
}

int mmu_context_add_page(struct mm_context *mm, unsigned long pa)
{
    if (mm == (struct mm_context *)0 || pa == 0UL) {
        return 0;
    }

    if (mmu_context_has_page(mm, pa)) {
        return 1;
    }

    if (mm->page_count >= MM_MAX_TRACKED_PAGES) {
        return 0;
    }

    mm->pages[mm->page_count++] = pa;
    return 1;
}

int mmu_context_remove_page(struct mm_context *mm, unsigned long pa)
{
    unsigned int index;

    if (mm == (struct mm_context *)0 || pa == 0UL) {
        return 0;
    }

    for (index = 0U; index < mm->page_count; index++) {
        unsigned int tail;

        if (mm->pages[index] != pa) {
            continue;
        }

        for (tail = index + 1U; tail < mm->page_count; tail++) {
            mm->pages[tail - 1U] = mm->pages[tail];
        }

        mm->page_count--;
        mm->pages[mm->page_count] = 0UL;
        return 1;
    }

    return 0;
}

static int mmu_resolve_user_page(const struct mm_context *mm,
                                 unsigned long va,
                                 unsigned long *page_pa,
                                 unsigned long *entry_out)
{
    unsigned long *l0;
    unsigned long *l1;
    unsigned long *l2;
    unsigned long *l3;
    unsigned long entry;

    if (mm == (const struct mm_context *)0 || page_pa == (unsigned long *)0) {
        return 0;
    }

    l0 = (unsigned long *)pa_to_va((void *)mm->root_pa);
    entry = l0[l0_index_for(va)];
    if ((entry & MMU_DESC_VALID) == 0UL || (entry & MMU_DESC_TYPE_MASK) != MMU_DESC_TABLE) {
        return 0;
    }

    l1 = (unsigned long *)pa_to_va((void *)(entry & MMU_DESC_ADDR_MASK));
    entry = l1[l1_index_for(va)];
    if ((entry & MMU_DESC_VALID) == 0UL || (entry & MMU_DESC_TYPE_MASK) != MMU_DESC_TABLE) {
        return 0;
    }

    l2 = (unsigned long *)pa_to_va((void *)(entry & MMU_DESC_ADDR_MASK));
    entry = l2[l2_index_for(va)];
    if ((entry & MMU_DESC_VALID) == 0UL || (entry & MMU_DESC_TYPE_MASK) != MMU_DESC_TABLE) {
        return 0;
    }

    l3 = (unsigned long *)pa_to_va((void *)(entry & MMU_DESC_ADDR_MASK));
    entry = l3[l3_index_for(va)];
    if ((entry & MMU_DESC_VALID) == 0UL || (entry & MMU_DESC_TYPE_MASK) != MMU_DESC_PAGE) {
        return 0;
    }

    *page_pa = entry & MMU_L3_PAGE_ADDR_MASK;
    if (entry_out != (unsigned long *)0) {
        *entry_out = entry;
    }

    return 1;
}

static int mmu_user_access_allowed(unsigned long entry, int write)
{
    unsigned long ap_bits;

    ap_bits = entry & (3UL << 6);
    if (write != 0) {
        return ap_bits == MMU_USER_PAGE_AP_RW;
    }

    return ap_bits == MMU_USER_PAGE_AP_RW || ap_bits == MMU_USER_PAGE_AP_RO;
}

static int mmu_copy_user_range(const struct mm_context *mm,
                               unsigned char *kernel_buffer,
                               unsigned long user_va,
                               unsigned long len,
                               int write_to_user)
{
    while (len != 0UL) {
        unsigned long page_pa;
        unsigned long entry;
        unsigned long page_offset;
        unsigned long chunk;
        unsigned char *page_va;
        unsigned long index;

        if (!mmu_resolve_user_page(mm, user_va, &page_pa, &entry)) {
            return 0;
        }

        if (!mmu_user_access_allowed(entry, write_to_user)) {
            return 0;
        }

        page_offset = user_va & (PAGE_SIZE - 1UL);
        chunk = PAGE_SIZE - page_offset;
        if (chunk > len) {
            chunk = len;
        }

        page_va = (unsigned char *)pa_to_va((void *)(page_pa + page_offset));
        for (index = 0UL; index < chunk; index++) {
            if (write_to_user != 0) {
                page_va[index] = kernel_buffer[index];
            } else {
                kernel_buffer[index] = page_va[index];
            }
        }

        kernel_buffer += chunk;
        user_va += chunk;
        len -= chunk;
    }

    return 1;
}

struct mm_context *mmu_context_create(void)
{
    struct mm_context *mm;
    unsigned long *root;
    unsigned int index;

    mm = (struct mm_context *)kmalloc(sizeof(struct mm_context));
    if (mm == (struct mm_context *)0) {
        return (struct mm_context *)0;
    }

    root = (unsigned long *)page_alloc();
    if (root == (unsigned long *)0) {
        kfree(mm);
        return (struct mm_context *)0;
    }

    mm->root_pa = (unsigned long)root;
    mm->asid    = mmu_asid_alloc();
    mm->page_count = 0U;
    for (index = 0U; index < MM_MAX_TRACKED_PAGES; index++) {
        mm->pages[index] = 0UL;
    }

    if (mm->asid == 0U) {
        page_free(root);
        kfree(mm);
        return (struct mm_context *)0;
    }

    if (!mmu_context_add_page(mm, mm->root_pa)) {
        page_free(root);
        mmu_asid_free(mm->asid);
        kfree(mm);
        return (struct mm_context *)0;
    }

    return mm;
}

void mmu_context_destroy(struct mm_context *mm)
{
    if (mm == (struct mm_context *)0) {
        return;
    }

    while (mm->page_count > 0U) {
        mm->page_count--;
        if (mm->pages[mm->page_count] != 0UL) {
            page_free((void *)mm->pages[mm->page_count]);
        }
    }

    mmu_asid_free(mm->asid);

    kfree(mm);
}

int mmu_copy_from_user(const struct mm_context *mm, void *dst,
                       unsigned long src_va, unsigned long len)
{
    if (len == 0UL) {
        return 1;
    }

    if (dst == (void *)0) {
        return 0;
    }

    return mmu_copy_user_range(mm, (unsigned char *)dst, src_va, len, 0);
}

int mmu_copy_to_user(const struct mm_context *mm, unsigned long dst_va,
                     const void *src, unsigned long len)
{
    if (len == 0UL) {
        return 1;
    }

    if (src == (const void *)0) {
        return 0;
    }

    return mmu_copy_user_range(mm, (unsigned char *)src, dst_va, len, 1);
}

int mmu_user_page_pa(const struct mm_context *mm, unsigned long va,
                     unsigned long *page_pa_out)
{
    unsigned long page_pa;

    if (page_pa_out == (unsigned long *)0) {
        return 0;
    }

    if (!mmu_resolve_user_page(mm, va, &page_pa, (unsigned long *)0)) {
        return 0;
    }

    *page_pa_out = page_pa;
    return 1;
}

void mmu_context_switch(struct mm_context *mm)
{
    unsigned long ttbr0_val;

    if (!mmu_enabled) {
        return;
    }

    if (mm == (struct mm_context *)0) {
        /*
         * Kernel task: ASID=0, empty lower-half root.
         * TLB entries for user processes (ASID > 0) are automatically
         * invisible while ASID=0 is active.
         */
        ttbr0_val = (unsigned long)ttbr0_runtime_empty_root;
    } else {
        /*
         * User task: embed the 8-bit ASID in TTBR0_EL1[55:48].
         * TCR_EL1.AS=0 (8-bit), TCR_EL1.A1=0 (TTBR0 provides ASID).
         * No TLB invalidation is needed: each process has a unique ASID,
         * so its TLB entries (tagged at creation) are never visible while
         * another ASID is active.
         */
        ttbr0_val = mm->root_pa | ((unsigned long)mm->asid << 48);
    }

    __asm__ volatile(
        "msr ttbr0_el1, %0\n"
        "isb\n"
        :
        : "r"(ttbr0_val)
        : "memory");
}

/*
 * Map a single 4 KiB page in the user address space described by mm.
 *
 * All descriptor values use physical addresses (page_alloc returns PA).
 * Intermediate tables are accessed via pa_to_va() so they can be
 * dereferenced while the kernel runs at high VA (TTBR1).
 *
 * The function does NOT track sub-table pages in the mmu_table_page_*
 * debug arrays — Stage 9 is the first increment; that can be added later.
 */
int mmu_map_user_page(struct mm_context *mm, unsigned long va,
                      unsigned long pa, unsigned long flags)
{
    unsigned long *l0;
    unsigned long *l1;
    unsigned long *l2;
    unsigned long *l3;
    void *new_page;
    unsigned long idx;

    if (mm == (struct mm_context *)0) {
        return 0;
    }

    /* Access L0 via kernel VA (mm->root_pa is the PA of the L0 table). */
    l0 = (unsigned long *)pa_to_va((void *)mm->root_pa);

    /* L0 -> L1 */
    idx = l0_index_for(va);
    if ((l0[idx] & MMU_DESC_VALID) == 0UL) {
        new_page = page_alloc();
        if (new_page == (void *)0) {
            return 0;
        }
        if (!mmu_context_add_page(mm, (unsigned long)new_page)) {
            page_free(new_page);
            return 0;
        }
        l0[idx] = (unsigned long)new_page | MMU_DESC_TABLE;
    }
    l1 = (unsigned long *)pa_to_va((void *)(l0[idx] & MMU_DESC_ADDR_MASK));

    /* L1 -> L2 */
    idx = l1_index_for(va);
    if ((l1[idx] & MMU_DESC_VALID) == 0UL) {
        new_page = page_alloc();
        if (new_page == (void *)0) {
            return 0;
        }
        if (!mmu_context_add_page(mm, (unsigned long)new_page)) {
            page_free(new_page);
            return 0;
        }
        l1[idx] = (unsigned long)new_page | MMU_DESC_TABLE;
    }
    l2 = (unsigned long *)pa_to_va((void *)(l1[idx] & MMU_DESC_ADDR_MASK));

    /* L2 -> L3 */
    idx = l2_index_for(va);
    if ((l2[idx] & MMU_DESC_VALID) == 0UL) {
        new_page = page_alloc();
        if (new_page == (void *)0) {
            return 0;
        }
        if (!mmu_context_add_page(mm, (unsigned long)new_page)) {
            page_free(new_page);
            return 0;
        }
        l2[idx] = (unsigned long)new_page | MMU_DESC_TABLE;
    }
    l3 = (unsigned long *)pa_to_va((void *)(l2[idx] & MMU_DESC_ADDR_MASK));

    /* Install the L3 page descriptor.
     * Force nG=1 (bit 11) so the TLB entry is tagged with the current ASID
     * rather than being global — required for correct ASID-based isolation.
     */
    idx = l3_index_for(va);
    l3[idx] = (pa & MMU_L3_PAGE_ADDR_MASK) | flags | MMU_DESC_PAGE | (1UL << 11);

    /*
     * Invalidate the specific VA / ASID combination in the Inner Shareable
     * domain.  TLBI VAE1IS Xt: bits[63:48]=ASID, bits[43:0]=VA[55:12].
     * This is cheaper than VMALLE1 (all-VA flush) and correct because the
     * page was either unmapped before (no cached translation) or is being
     * remapped to a new PA (stale entry must go).
     */
    {
        unsigned long tlbi_val = ((unsigned long)mm->asid << 48) | (va >> 12);

        __asm__ volatile(
            "dsb ish\n"
            "tlbi vae1is, %0\n"
            "dsb ish\n"
            "isb\n"
            :
            : "r"(tlbi_val)
            : "memory");
    }

    return 1;
}

int mmu_unmap_user_page(struct mm_context *mm, unsigned long va)
{
    unsigned long *l0;
    unsigned long *l1;
    unsigned long *l2;
    unsigned long *l3;
    unsigned long idx;
    unsigned long tlbi_val;

    if (mm == (struct mm_context *)0) {
        return 0;
    }

    l0 = (unsigned long *)pa_to_va((void *)mm->root_pa);
    idx = l0_index_for(va);
    if ((l0[idx] & MMU_DESC_VALID) == 0UL || (l0[idx] & MMU_DESC_TYPE_MASK) != MMU_DESC_TABLE) {
        return 0;
    }

    l1 = (unsigned long *)pa_to_va((void *)(l0[idx] & MMU_DESC_ADDR_MASK));
    idx = l1_index_for(va);
    if ((l1[idx] & MMU_DESC_VALID) == 0UL || (l1[idx] & MMU_DESC_TYPE_MASK) != MMU_DESC_TABLE) {
        return 0;
    }

    l2 = (unsigned long *)pa_to_va((void *)(l1[idx] & MMU_DESC_ADDR_MASK));
    idx = l2_index_for(va);
    if ((l2[idx] & MMU_DESC_VALID) == 0UL || (l2[idx] & MMU_DESC_TYPE_MASK) != MMU_DESC_TABLE) {
        return 0;
    }

    l3 = (unsigned long *)pa_to_va((void *)(l2[idx] & MMU_DESC_ADDR_MASK));
    idx = l3_index_for(va);
    if ((l3[idx] & MMU_DESC_VALID) == 0UL || (l3[idx] & MMU_DESC_TYPE_MASK) != MMU_DESC_PAGE) {
        return 0;
    }

    l3[idx] = 0UL;

    tlbi_val = ((unsigned long)mm->asid << 48) | (va >> 12);
    __asm__ volatile(
        "dsb ish\n"
        "tlbi vae1is, %0\n"
        "dsb ish\n"
        "isb\n"
        :
        : "r"(tlbi_val)
        : "memory");

    return 1;
}
#endif /* CONFIG_KERNEL_VIRTUAL */