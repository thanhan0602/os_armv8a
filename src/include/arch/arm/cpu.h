#ifndef ARCH_ARM_CPU_H
#define ARCH_ARM_CPU_H

static inline void cpu_yield(void)
{
    __asm__ volatile("yield" ::: "memory");
}

static inline void cpu_wfe(void)
{
    __asm__ volatile("wfe" ::: "memory");
}

static inline void cpu_sev(void)
{
    __asm__ volatile("sev" ::: "memory");
}

static inline void cpu_isb(void)
{
    __asm__ volatile("isb" ::: "memory");
}

static inline void cpu_dsb_ish(void)
{
    __asm__ volatile("dsb ish" ::: "memory");
}

static inline void cpu_invalidate_icache_all(void)
{
    __asm__ volatile("dsb ish\n ic iallu\n dsb nsh\n isb\n" ::: "memory");
}

static inline void cpu_brk(unsigned short imm)
{
    (void)imm;
    /* Use a macro or a trick for the immediate if needed, but for now #0 is common. */
    __asm__ volatile("brk #0");
}

static inline unsigned long arch_get_mpidr(void)
{
    unsigned long mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return mpidr;
}

static inline unsigned int arch_get_cpu_id(void)
{
    /* AFF0 is usually the CPU ID in simple virt setups */
    return (unsigned int)(arch_get_mpidr() & 0xFF);
}

static inline void arch_set_current_task(void *task)
{
    __asm__ volatile("msr tpidr_el1, %0" : : "r"(task));
}

static inline void *arch_get_current_task(void)
{
    void *task;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(task));
    return task;
}

static inline unsigned long arch_local_irq_save(void)
{
    unsigned long flags;
    __asm__ volatile(
        "mrs %0, daif\n"
        "msr daifset, #2"
        : "=r"(flags)
        : : "memory");
    return flags;
}

static inline void arch_local_irq_restore(unsigned long flags)
{
    __asm__ volatile("msr daif, %0" : : "r"(flags) : "memory");
}

static inline void arch_local_irq_enable(void)
{
    __asm__ volatile(
        "msr daifclr, #2\n"
        "isb"
        : : : "memory");
}

#endif
