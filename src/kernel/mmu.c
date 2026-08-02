#include <kernel/mmu.h>
#include <arch/arm/virt.h>
#include <arch/arm/sysregs.h>
#include <kernel/debug_targets.h>
#include <kernel/mmu_table.h>
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

void mmu_setup_core(void)
{
    unsigned long mair;
    unsigned long tcr;

    /*
     * MAIR_EL1 encodes the memory types referenced by AttrIndx in descriptors:
     * - slot 0 = Device-nGnRnE for MMIO
     * - slot 1 = Normal WB/WA cacheable memory for RAM
     */
    mair = (MMU_MAIR_DEVICE_nGnRnE << 0) | (MMU_MAIR_NORMAL_WBWA << 8);

    /*
     * TCR_EL1 defines how TTBR0_EL1 (and TTBR1_EL1) addresses are translated:
     */
    tcr = MMU_T0SZ |
          (1UL << 8) |
          (1UL << 10) |
          (3UL << 12) |
          (0UL << 14) |
          (0UL << 22) |  /* A1 = 0: ASID comes from TTBR0_EL1 */
          /* (1UL << 36) |  AS = 1: 16-bit ASID */
          MMU_T1SZ |
          (1UL << 24) |
          (1UL << 26) |
          (3UL << 28) |
          MMU_TG1_4K |
          MMU_TCR_IPS_48BIT;

    mmu_set_mair(mair);
    mmu_set_tcr(tcr);
    mmu_set_ttbr0((unsigned long)mmu_l0_table);
    mmu_set_ttbr1((unsigned long)l0_table_ttbr1);
    mmu_invalidate_tlb_all();

    unsigned long sctlr = SCTLR_EL1_RES1 | SCTLR_EL1_M | SCTLR_EL1_C | SCTLR_EL1_I;
    mmu_set_sctlr(sctlr);
}

void mmu_init(void)
{
    if (mmu_enabled) {
        return;
    }

    mmu_table_registry_reset();

    // mmu_log_kernel_layout();

    /* 
     * Setup boot tables. 
     */
    if (!mmu_build_identity_map()) {
        return;
    }

    if (!build_kernel_map()) {
        return;
    }

    kernel_debug_log_mmu_boot_targets();

    /* Check ASID bits support */
    unsigned long mmfr0 = arch_get_aa64mmfr0();
    int asid_bits = ((mmfr0 >> 4) & 0xf) == 2 ? 16 : 8;
    KER_LOGF("[mmu] hardware supports %d-bit ASID\n", asid_bits);

    mmu_setup_core();
    mmu_enabled = 1;
}

void mmu_init_secondary(void)
{
    mmu_setup_core();
}

int mmu_is_enabled(void)
{
    return mmu_enabled;
}
