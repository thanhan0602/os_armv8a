#include <drivers/uart/pl011.h>

#include <arch/arm/virt.h>
#include <kernel/mmu.h>
#include <kernel/vm.h>

#define PL011_BASE QEMU_VIRT_PL011_BASE
#define PL011_DR   (PL011_BASE + 0x00UL)
#define PL011_FR   (PL011_BASE + 0x18UL)
#define PL011_IBRD (PL011_BASE + 0x24UL)
#define PL011_FBRD (PL011_BASE + 0x28UL)
#define PL011_LCRH (PL011_BASE + 0x2cUL)
#define PL011_CR   (PL011_BASE + 0x30UL)
#define PL011_IMSC (PL011_BASE + 0x38UL)
#define PL011_ICR  (PL011_BASE + 0x44UL)

#define PL011_FR_TXFF (1U << 5)
#define PL011_FR_RXFE (1U << 4)
#define PL011_LCRH_FEN  (1U << 4)
#define PL011_LCRH_WLEN (3U << 5)
#define PL011_CR_UARTEN (1U << 0)
#define PL011_CR_TXE    (1U << 8)
#define PL011_CR_RXE    (1U << 9)

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

void pl011_init(void)
{
    mmio_write(PL011_CR, 0U);
    mmio_write(PL011_IMSC, 0U);
    mmio_write(PL011_ICR, 0x7ffU);
    mmio_write(PL011_IBRD, 13U);
    mmio_write(PL011_FBRD, 1U);
    mmio_write(PL011_LCRH, PL011_LCRH_FEN | PL011_LCRH_WLEN);
    mmio_write(PL011_CR, PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE);
}

void pl011_write(unsigned int value)
{
    while ((mmio_read(PL011_FR) & PL011_FR_TXFF) != 0U) {
    }

    mmio_write(PL011_DR, value);
}

int pl011_can_read(void)
{
    return (mmio_read(PL011_FR) & PL011_FR_RXFE) == 0U;
}

unsigned int pl011_read(void)
{
    while (!pl011_can_read()) {
    }

    return mmio_read(PL011_DR) & 0xffU;
}