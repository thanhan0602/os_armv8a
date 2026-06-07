#ifndef ARCH_ARM_SYSREGS_H
#define ARCH_ARM_SYSREGS_H

static inline unsigned long arch_timer_get_cntfrq(void)
{
    unsigned long val;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
}

static inline void arch_timer_set_cntv_tval(unsigned long val)
{
    __asm__ volatile("msr cntv_tval_el0, %0" : : "r"(val));
}

static inline void arch_timer_set_cntv_ctl(unsigned long val)
{
    __asm__ volatile("msr cntv_ctl_el0, %0" : : "r"(val));
}

static inline void arch_set_vbar_el1(unsigned long val)
{
    __asm__ volatile(
        "msr vbar_el1, %0\n"
        "isb"
        : : "r"(val) : "memory");
}

static inline void mmu_set_mair(unsigned long mair)
{
    __asm__ volatile("msr mair_el1, %0" : : "r"(mair) : "memory");
}

static inline void mmu_set_tcr(unsigned long tcr)
{
    __asm__ volatile("msr tcr_el1, %0" : : "r"(tcr) : "memory");
}

static inline void mmu_set_ttbr0(unsigned long ttbr0)
{
    __asm__ volatile(
        "msr ttbr0_el1, %0\n"
        "isb"
        : : "r"(ttbr0) : "memory");
}

static inline void mmu_set_ttbr1(unsigned long ttbr1)
{
    __asm__ volatile(
        "msr ttbr1_el1, %0\n"
        "isb"
        : : "r"(ttbr1) : "memory");
}

static inline void mmu_invalidate_tlb_all(void)
{
    __asm__ volatile(
        "dsb ish\n"
        "tlbi vmalle1\n"
        "dsb ish\n"
        "isb"
        : : : "memory");
}

static inline void mmu_invalidate_tlb_va(unsigned int asid, unsigned long va)
{
    unsigned long val = ((unsigned long)asid << 48) | (va >> 12);
    __asm__ volatile(
        "dsb ish\n"
        "tlbi vae1is, %0\n"
        "dsb ish\n"
        "isb"
        : : "r"(val) : "memory");
}

static inline void mmu_invalidate_tlb_asid(unsigned int asid)
{
    unsigned long val = (unsigned long)asid << 48;
    __asm__ volatile(
        "dsb ish\n"
        "tlbi aside1is, %0\n"
        "dsb ish\n"
        "isb"
        : : "r"(val) : "memory");
}

static inline unsigned long mmu_get_sctlr(void)
{
    unsigned long sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    return sctlr;
}

static inline unsigned long arch_get_aa64mmfr0(void)
{
    unsigned long val;
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(val));
    return val;
}

static inline void mmu_set_sctlr(unsigned long sctlr)
{
    __asm__ volatile(
        "msr sctlr_el1, %0\n"
        "isb"
        : : "r"(sctlr) : "memory");
}

static inline unsigned long mmu_probe_address_s1e1r(unsigned long va)
{
    unsigned long par;
    __asm__ volatile(
        "at s1e1r, %1\n"
        "isb\n"
        "mrs %0, par_el1\n"
        : "=r"(par)
        : "r"(va)
        : "memory");
    return par;
}

#endif
