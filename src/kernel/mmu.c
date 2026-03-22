#include <kernel/mmu.h>

#include <arch/arm/virt.h>
#include <kernel/debug_targets.h>
#include <kernel/log.h>
#include <kernel/page_alloc.h>

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

static void mmu_debug_set_chunk_name(char *destination, unsigned long chunk_index)
{
    static const char prefix[] = "l3-chunk-";
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
    for (index = 0UL; index < (sizeof(prefix) - 1UL) && output_index < (MMU_DEBUG_MAX_TABLE_NAME_LEN - 1UL); index++) {
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
        if (mmu_table_page_count < MMU_DEBUG_MAX_TABLE_PAGES) {
            mmu_table_page_addresses[mmu_table_page_count] = (unsigned long)table;
            mmu_debug_copy_name(mmu_table_page_names[mmu_table_page_count], name);
            mmu_table_page_count++;
        }
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
    log_write("[info] ");
    log_write(level_name);
    log_write(" index=");
    log_write_u64(index);
    log_putc('\n');
}

static void mmu_debug_print_entry(const char *level_name, unsigned long entry)
{
    log_write("[info] ");
    log_write(level_name);
    log_write(" entry=");
    log_write_hex(entry);
    log_putc('\n');
}

static void mmu_debug_print_leaf_attrs(unsigned long address, unsigned long entry)
{
    unsigned long attr_index;

    log_write("[info] walk attrs: mem=");
    attr_index = entry & MMU_ATTR_INDEX_MASK;
    if (attr_index == MMU_ATTR_DEVICE) {
        log_write("device");
    } else if (attr_index == MMU_ATTR_NORMAL) {
        log_write("normal");
    } else {
        log_write("unknown");
    }

    log_write(" ap=");
    if ((entry & MMU_AP_RO) == MMU_AP_RO) {
        log_write("ro");
    } else {
        log_write("rw");
    }

    log_write(" exec=");
    if ((entry & MMU_PXN) != 0) {
        log_write("nx");
    } else {
        log_write("x");
    }

    log_write(" sh=");
    if ((entry & MMU_SH_INNER_SHAREABLE) == MMU_SH_INNER_SHAREABLE) {
        log_write("inner");
    } else {
        log_write("non");
    }

    log_write(" af=");
    if ((entry & MMU_AF) != 0) {
        log_write("1");
    } else {
        log_write("0");
    }

    log_write(" section=");
    log_write(mmu_region_name(address));
    log_putc('\n');
}

static void mmu_debug_print_desc_kind(const char *level_name, const char *kind)
{
    log_write("[info] ");
    log_write(level_name);
    log_write(" type=");
    log_write(kind);
    log_putc('\n');
}

/*
 * Software walk helper for bring-up: it follows the page-table pointers we just
 * built in RAM and prints where translation stops. Because Stage 6 still uses an
 * identity map, the table addresses can be dereferenced directly before MMU enable.
 */
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
    unsigned long *l1_walk_table;
    unsigned long *l2_walk_table;
    unsigned long *l3_walk_table;
    unsigned long pa;

    log_write("[info] walk va=");
    log_write_hex(address);
    log_putc('\n');
    log_write("[info] walk region=");
    log_write(mmu_region_name(address));
    log_putc('\n');

    l0_index = l0_index_for(address);
    l0_entry = l0_table[l0_index];
    mmu_debug_print_index("walk l0", l0_index);
    mmu_debug_print_entry("walk l0", l0_entry);
    if ((l0_entry & MMU_DESC_VALID) == 0) {
        mmu_debug_print_desc_kind("walk l0", "invalid");
        log_info("walk stopped: invalid l0 entry");
        return;
    }
    mmu_debug_print_desc_kind("walk l0", "table");

    l1_walk_table = (unsigned long *)(l0_entry & MMU_DESC_ADDR_MASK);
    l1_index = l1_index_for(address);
    l1_entry = l1_walk_table[l1_index];
    mmu_debug_print_index("walk l1", l1_index);
    mmu_debug_print_entry("walk l1", l1_entry);
    if ((l1_entry & MMU_DESC_VALID) == 0) {
        mmu_debug_print_desc_kind("walk l1", "invalid");
        log_info("walk stopped: invalid l1 entry");
        return;
    }

    if ((l1_entry & MMU_DESC_TYPE_MASK) == MMU_DESC_BLOCK) {
        mmu_debug_print_desc_kind("walk l1", "block");
        mmu_debug_print_leaf_attrs(address, l1_entry);
        pa = (l1_entry & MMU_L1_BLOCK_ADDR_MASK) | (address & (MMU_L1_BLOCK_SIZE - 1UL));
        log_write("[info] walk pa from l1 block=");
        log_write_hex(pa);
        log_putc('\n');
        return;
    }
    mmu_debug_print_desc_kind("walk l1", "table");

    l2_walk_table = (unsigned long *)(l1_entry & MMU_DESC_ADDR_MASK);
    l2_index = l2_index_for(address);
    l2_entry = l2_walk_table[l2_index];
    mmu_debug_print_index("walk l2", l2_index);
    mmu_debug_print_entry("walk l2", l2_entry);
    if ((l2_entry & MMU_DESC_VALID) == 0) {
        mmu_debug_print_desc_kind("walk l2", "invalid");
        log_info("walk stopped: invalid l2 entry");
        return;
    }

    if ((l2_entry & MMU_DESC_TYPE_MASK) == MMU_DESC_BLOCK) {
        mmu_debug_print_desc_kind("walk l2", "block");
        mmu_debug_print_leaf_attrs(address, l2_entry);
        pa = (l2_entry & MMU_L2_BLOCK_ADDR_MASK) | (address & (MMU_L2_BLOCK_SIZE - 1UL));
        log_write("[info] walk pa from l2 block=");
        log_write_hex(pa);
        log_putc('\n');
        return;
    }
    mmu_debug_print_desc_kind("walk l2", "table");

    l3_walk_table = (unsigned long *)(l2_entry & MMU_DESC_ADDR_MASK);
    l3_index = l3_index_for(address);
    l3_entry = l3_walk_table[l3_index];
    mmu_debug_print_index("walk l3", l3_index);
    mmu_debug_print_entry("walk l3", l3_entry);
    if ((l3_entry & MMU_DESC_VALID) == 0) {
        mmu_debug_print_desc_kind("walk l3", "invalid");
        log_info("walk stopped: invalid l3 entry");
        return;
    }

    mmu_debug_print_desc_kind("walk l3", "page");
    mmu_debug_print_leaf_attrs(address, l3_entry);
    pa = (l3_entry & MMU_L3_PAGE_ADDR_MASK) | (address & (PAGE_SIZE - 1UL));
    log_write("[info] walk pa from l3 page=");
    log_write_hex(pa);
    log_putc('\n');
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
        log_info("walk unavailable: mmu tables not initialized");
        return;
    }

    mmu_debug_walk(address);
#else
    (void)address;
    log_info("walk unavailable: MMU debug walk disabled at build time");
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
        mmu_debug_set_chunk_name(mmu_table_page_names[mmu_table_page_count], chunk_index);
        l3_table = alloc_named_table_page(mmu_table_page_names[mmu_table_page_count]);
        if (l3_table == (unsigned long *)0) {
            log_info("mmu init failed: no free pages for l3 table");
            return 0;
        }

        l2_ram_table[l2_index_for(chunk_base)] = ((unsigned long)l3_table) | MMU_DESC_TABLE;

        for (address = chunk_base; address < chunk_base + MMU_L2_BLOCK_SIZE; address += PAGE_SIZE) {
            l3_table[l3_index_for(address)] = (address & ~(PAGE_SIZE - 1UL)) |
                                              kernel_page_attrs(address) |
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

void mmu_init(void)
{
    unsigned long mair;
    unsigned long tcr;
    unsigned long sctlr;

    if (mmu_enabled) {
        return;
    }

    log_info("mmu init start");

    mmu_table_page_count = 0UL;

    l0_table = alloc_named_table_page("l0-root");
    l1_table = alloc_named_table_page("l1-root");
    l2_ram_table = alloc_named_table_page("l2-ram");
    if (l0_table == (unsigned long *)0 ||
        l1_table == (unsigned long *)0 ||
        l2_ram_table == (unsigned long *)0) {
        log_info("mmu init failed: no free pages for page tables");
        return;
    }

    log_info("mmu tables allocated");

    if (!build_identity_map()) {
        return;
    }

    log_info("mmu maps built");

    log_write("[info] l0 root entry=");
    log_write_hex(l0_table[l0_index_for(QEMU_VIRT_RAM_BASE)]);
    log_putc('\n');
    log_write("[info] l1 device entry=");
    log_write_hex(l1_table[l1_index_for(0x00000000UL)]);
    log_putc('\n');
    log_write("[info] l1 ram entry=");
    log_write_hex(l1_table[l1_index_for(QEMU_VIRT_RAM_BASE)]);
    log_putc('\n');
    log_write("[info] l2 ram[0] entry=");
    log_write_hex(l2_ram_table[l2_index_for(QEMU_VIRT_RAM_BASE)]);
    log_putc('\n');
    log_write("[info] l3 fine map chunks=");
    log_write_u64(fine_map_chunks_used);
    log_putc('\n');

    kernel_debug_log_mmu_boot_targets();

    /*
     * MAIR_EL1 encodes the memory types referenced by AttrIndx in descriptors:
     * - slot 0 = Device-nGnRnE for MMIO
     * - slot 1 = Normal WB/WA cacheable memory for RAM
     */
    mair = (MMU_MAIR_DEVICE_nGnRnE << 0) | (MMU_MAIR_NORMAL_WBWA << 8);
    /*
     * TCR_EL1 defines how TTBR0_EL1 addresses are translated:
     * - T0SZ[5:0]   = 16  -> 48-bit VA space
     * - IRGN0[9:8]  = 01  -> inner WB/WA cacheability for table walks
     * - ORGN0[11:10]= 01  -> outer WB/WA cacheability for table walks
     * - SH0[13:12]  = 11  -> inner-shareable table walks
     * - TG0[15:14]  = 00  -> 4 KiB granule
     * - EPD1[23]    = 1   -> disable TTBR1_EL1 walks; this kernel only uses TTBR0_EL1
     * - IPS[34:32]  = 101 -> 48-bit physical address size
     */
    tcr = MMU_T0SZ |
          (1UL << 8) |
          (1UL << 10) |
          (3UL << 12) |
          (0UL << 14) |
          (1UL << 23) |
            MMU_TCR_IPS_48BIT;

    /*
     * Bring the translation regime live in this order:
     * 1. MAIR_EL1  <- memory attribute slots used by descriptors
     * 2. TCR_EL1   <- translation size/shareability/cacheability/granule
     * 3. TTBR0_EL1 <- base address of the L0 root table
     * 4. TLBI      <- discard any stale EL1 Stage-1 translations
     * 5. DSB/ISB   <- complete the register/TLB programming before SCTLR_EL1.M
     */
    __asm__ volatile(
        "dsb ish\n"
        "msr mair_el1, %0\n"
        "msr tcr_el1, %1\n"
        "msr ttbr0_el1, %2\n"
        "tlbi vmalle1\n"
        "dsb ish\n"
        "isb\n"
        :
        : "r"(mair), "r"(tcr), "r"(l0_table)
        : "memory");

    log_info("mmu control registers programmed");

    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));

    log_write("[info] sctlr_el1 before=");
    log_write_hex(sctlr);
    log_putc('\n');

    /* Enable the MMU and both caches with the required architectural RES1 bits. */
    sctlr = SCTLR_EL1_RES1 | SCTLR_EL1_M | SCTLR_EL1_C | SCTLR_EL1_I;

    log_write("[info] sctlr_el1 after=");
    log_write_hex(sctlr);
    log_putc('\n');

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

    log_info("stage 6 mmu enabled");
    log_write("[info] ttbr0_el1=");
    log_write_hex((unsigned long)l0_table);
    log_putc('\n');
    log_write("[info] mmu table pages=");
    log_write_u64(table_pages_used);
    log_putc('\n');
}

int mmu_is_enabled(void)
{
    return mmu_enabled;
}

unsigned long mmu_table_pages_used(void)
{
    return table_pages_used;
}