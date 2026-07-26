#ifndef ARCH_ARM_PSCI_H
#define ARCH_ARM_PSCI_H

/* PSCI function IDs for AArch64 */
#define PSCI_0_2_FN_PSCI_VERSION        0x84000000
#define PSCI_0_2_FN64_CPU_ON            0xc4000003
#define PSCI_0_2_FN_SYSTEM_OFF           0x84000008

static inline int psci_version(void)
{
    register unsigned long x0 __asm__("x0") = PSCI_0_2_FN_PSCI_VERSION;
    __asm__ volatile("hvc #0" : "+r"(x0) :: "memory");
    return (int)x0;
}

static inline int psci_cpu_on(unsigned long target_cpu, unsigned long entry_point, unsigned long context_id)
{
    register unsigned long x0 __asm__("x0") = PSCI_0_2_FN64_CPU_ON;
    register unsigned long x1 __asm__("x1") = target_cpu;
    register unsigned long x2 __asm__("x2") = entry_point;
    register unsigned long x3 __asm__("x3") = context_id;

    __asm__ volatile(
        "hvc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3)
        : "memory");

    return (int)x0;
}

static inline void psci_system_off(void)
{
    register unsigned long x0 __asm__("x0") = PSCI_0_2_FN_SYSTEM_OFF;

    __asm__ volatile("hvc #0" : "+r"(x0) :: "memory");

    /* PSCI SYSTEM_OFF must not return on success. */
    for (;;) {
        __asm__ volatile("wfe" ::: "memory");
    }
}

#endif /* ARCH_ARM_PSCI_H */
