#include <drivers/interrupt/gicv2.h>

#include <arch/arm/virt.h>
#include <kernel/mmu.h>
#include <kernel/vm.h>

#define GICD_BASE QEMU_VIRT_GICD_BASE
#define GICC_BASE QEMU_VIRT_GICC_BASE

#define GICD_CTLR        (GICD_BASE + 0x000UL)
#define GICD_IGROUPR0    (GICD_BASE + 0x080UL)
#define GICD_ISENABLER0  (GICD_BASE + 0x100UL)

#define GICC_CTLR        (GICC_BASE + 0x000UL)
#define GICC_PMR         (GICC_BASE + 0x004UL)
#define GICC_BPR         (GICC_BASE + 0x008UL)
#define GICC_IAR         (GICC_BASE + 0x00cUL)
#define GICC_EOIR        (GICC_BASE + 0x010UL)

static inline volatile unsigned int *mmio_va(unsigned long reg)
{
    if (mmu_is_enabled())
        return (volatile unsigned int *)(unsigned long)pa_to_va(reg);
    return (volatile unsigned int *)reg;
}

static inline void mmio_write(unsigned long reg, unsigned int value)
{
    *mmio_va(reg) = value;
}

static inline unsigned int mmio_read(unsigned long reg)
{
    return *mmio_va(reg);
}

void gicv2_init(void)
{
    unsigned int group0;

    mmio_write(GICD_CTLR, 0U);
    mmio_write(GICC_CTLR, 0U);

    group0 = mmio_read(GICD_IGROUPR0);
    group0 |= (1U << 30);
    mmio_write(GICD_IGROUPR0, group0);

    mmio_write(GICC_PMR, 0xffU);
    mmio_write(GICC_BPR, 0U);
    mmio_write(GICC_CTLR, 0x3U);
    mmio_write(GICD_CTLR, 0x3U);
}

void gicv2_enable_irq(unsigned int intid)
{
    mmio_write(GICD_ISENABLER0 + ((unsigned long)(intid / 32U) * 4UL),
               (1U << (intid % 32U)));
}

unsigned int gicv2_acknowledge_irq(void)
{
    return mmio_read(GICC_IAR);
}

void gicv2_end_of_interrupt(unsigned int intid)
{
    mmio_write(GICC_EOIR, intid);
}