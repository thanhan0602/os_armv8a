#ifndef DRIVERS_INTERRUPT_GIC_H
#define DRIVERS_INTERRUPT_GIC_H

void gic_init(void);
void gic_init_secondary(void);
void gic_enable_irq(unsigned int intid);
unsigned int gic_acknowledge_irq(void);
void gic_end_of_interrupt(unsigned int iar);
void gic_send_ipi(unsigned int cpu_mask, unsigned int intid);

#endif