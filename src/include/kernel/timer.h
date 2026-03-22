#ifndef KERNEL_TIMER_H
#define KERNEL_TIMER_H

void timer_init(void);
int timer_handle_irq(unsigned int intid);
unsigned long timer_tick_count(void);

#endif