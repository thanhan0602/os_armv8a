#ifndef DRIVERS_INTERRUPT_GICV2_H
#define DRIVERS_INTERRUPT_GICV2_H

void gicv2_init(void);
void gicv2_init_secondary(void);
void gicv2_enable_irq(unsigned int intid);
unsigned int gicv2_acknowledge_irq(void);
void gicv2_end_of_interrupt(unsigned int intid);
void gicv2_send_ipi(unsigned int cpu_mask, unsigned int intid);

#endif