#include <kernel/mmu.h>
#include <arch/arm/virt.h>
#include <arch/arm/sysregs.h>
#include <kernel/debug_targets.h>
#include <kernel/mmu_debug.h>
#include <kernel/log.h>
#include <kernel/page_alloc.h>
#include <kernel/vm.h>

/*
 * Core MMU design summary:
 * - EL1 Stage-1 translation only
 * - 48-bit virtual address space with 4 KiB pages
 * - Enable/Disable logic, MAIR, TCR, SCTLR configuration.
 */

static int mmu_enabled;

void mmu_init(void)
{
    unsigned long mair;
    unsigned long tcr;
    unsigned long sctlr;

    if (mmu_enabled) {
        return;
    }

    mmu_debug_reset();

    /* 
     * Setup boot tables. 
     * These functions now handle their own allocations via alloc_named_table_page.
     */
    if (!mmu_build_identity_map()) {
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
     * - A1[22]      = 0   -> ASID is provided in TTBR0_EL1
     * - AS[36]      = 0   -> 8-bit ASID size
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
          (0UL << 22) |  /* A1 = 0: ASID comes from TTBR0_EL1 */
          (0UL << 36) |  /* AS = 0: 8-bit ASID */
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
     */
    mmu_set_mair(mair);
    mmu_set_tcr(tcr);
    mmu_set_ttbr0((unsigned long)mmu_l0_table);
#ifdef CONFIG_KERNEL_VIRTUAL
    mmu_set_ttbr1((unsigned long)l0_table_ttbr1);
#endif
    mmu_invalidate_tlb_all();

    sctlr = mmu_get_sctlr();

    /* Enable the MMU and both caches with the required architectural RES1 bits. */
    sctlr = SCTLR_EL1_RES1 | SCTLR_EL1_M | SCTLR_EL1_C | SCTLR_EL1_I;

    /*
     * Writing SCTLR_EL1 is the commit point:
     * - M starts Stage-1 translation
     * - C allows data cache on normal memory
     * - I allows instruction cache
     */
    mmu_set_sctlr(sctlr);

    mmu_enabled = 1;
}

int mmu_is_enabled(void)
{
    return mmu_enabled;
}

unsigned long *mmu_debug_ttbr0_root(void)
{
    return mmu_l0_table;
}
