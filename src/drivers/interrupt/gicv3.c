#include <drivers/interrupt/gic.h>

#include <arch/arm/cpu.h>
#include <arch/arm/virt.h>
#include <kernel/mmu.h>
#include <kernel/vm.h>

#define GICD_CTLR               0x0000UL
#define GICD_ISENABLER          0x0100UL
#define GICD_CTLR_ENABLE_G1NS   (1U << 1)
#define GICD_CTLR_ARE_NS        (1U << 4)
#define GICD_CTLR_RWP           (1U << 31)

#define GICR_STRIDE             0x20000UL
#define GICR_WAKER              0x0014UL
#define GICR_WAKER_PS           (1U << 1)
#define GICR_WAKER_CA           (1U << 2)
#define GICR_SGI_BASE           0x10000UL
#define GICR_IGROUPR0           0x0080UL
#define GICR_ISENABLER0         0x0100UL

static inline volatile unsigned int *gic_mmio(unsigned long pa)
{
    if (mmu_is_enabled()) {
        return (volatile unsigned int *)(unsigned long)pa_to_va(pa);
    }
    return (volatile unsigned int *)pa;
}

static inline unsigned int gic_read32(unsigned long pa)
{
    return *gic_mmio(pa);
}

static inline void gic_write32(unsigned long pa, unsigned int value)
{
    *gic_mmio(pa) = value;
}

static void gicd_wait_rwp(void)
{
    while ((gic_read32(QEMU_VIRT_GICD_BASE + GICD_CTLR) &
            GICD_CTLR_RWP) != 0U) {
        cpu_yield();
    }
}

static unsigned long gicr_base_for_cpu(unsigned int cpu_id)
{
    return QEMU_VIRT_GICR_BASE + ((unsigned long)cpu_id * GICR_STRIDE);
}

static void gicv3_enable_system_registers(void)
{
    unsigned long value;

    __asm__ volatile("mrs %0, icc_sre_el1" : "=r"(value));
    value |= 1UL;
    __asm__ volatile("msr icc_sre_el1, %0\n isb" : : "r"(value) : "memory");

    value = 0xffUL;
    __asm__ volatile("msr icc_pmr_el1, %0" : : "r"(value) : "memory");
    value = 0UL;
    __asm__ volatile("msr icc_bpr1_el1, %0" : : "r"(value) : "memory");
    value = 1UL;
    __asm__ volatile("msr icc_igrpen1_el1, %0\n isb" : : "r"(value) : "memory");
}

void gic_init_secondary(void)
{
    unsigned int cpu_id = arch_get_cpu_id();
    unsigned long redistributor = gicr_base_for_cpu(cpu_id);
    unsigned long waker = redistributor + GICR_WAKER;
    unsigned int value;

    value = gic_read32(waker);
    value &= ~GICR_WAKER_PS;
    gic_write32(waker, value);
    while ((gic_read32(waker) & GICR_WAKER_CA) != 0U) {
        cpu_yield();
    }

    /* SGIs and PPIs are configured through this CPU's Redistributor. */
    gic_write32(redistributor + GICR_SGI_BASE + GICR_IGROUPR0,
                0xffffffffU);
    gic_write32(redistributor + GICR_SGI_BASE + GICR_ISENABLER0,
                (1U << 0) | (1U << 1));

    gicv3_enable_system_registers();
}

void gic_init(void)
{
    gic_write32(QEMU_VIRT_GICD_BASE + GICD_CTLR, 0U);
    gicd_wait_rwp();
    gic_write32(QEMU_VIRT_GICD_BASE + GICD_CTLR,
                GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_G1NS);
    gicd_wait_rwp();
    gic_init_secondary();
}

void gic_enable_irq(unsigned int intid)
{
    if (intid < 32U) {
        unsigned long redistributor = gicr_base_for_cpu(arch_get_cpu_id());
        gic_write32(redistributor + GICR_SGI_BASE + GICR_ISENABLER0,
                    1U << intid);
        return;
    }

    gic_write32(QEMU_VIRT_GICD_BASE + GICD_ISENABLER +
                ((unsigned long)(intid / 32U) * 4UL),
                1U << (intid % 32U));
}

unsigned int gic_acknowledge_irq(void)
{
    unsigned long iar;
    __asm__ volatile("mrs %0, icc_iar1_el1" : "=r"(iar));
    return (unsigned int)iar;
}

void gic_end_of_interrupt(unsigned int iar)
{
    unsigned long value = iar;
    __asm__ volatile("msr icc_eoir1_el1, %0\n isb" : : "r"(value) : "memory");
}

void gic_send_ipi(unsigned int cpu_mask, unsigned int intid)
{
    unsigned long sgi1r;

    if ((cpu_mask & 0xffffU) == 0U || intid > 15U) {
        return;
    }

    /* QEMU virt CPUs 0-3 share Aff3:Aff2:Aff1=0; target list uses Aff0. */
    sgi1r = ((unsigned long)(intid & 0xfU) << 24) |
            (unsigned long)(cpu_mask & 0xffffU);
    __asm__ volatile("dsb ishst\n msr icc_sgi1r_el1, %0\n isb"
                     : : "r"(sgi1r) : "memory");
}