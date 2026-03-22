#ifndef KERNEL_EXCEPTION_H
#define KERNEL_EXCEPTION_H

void exception_init(void);
void exception_handle_sync(unsigned long vector_id,
                           unsigned long esr_el1,
                           unsigned long elr_el1,
                           unsigned long spsr_el1,
                           unsigned long far_el1);
void exception_handle_irq(unsigned long vector_id,
                          unsigned long esr_el1,
                          unsigned long elr_el1,
                          unsigned long spsr_el1,
                          unsigned long far_el1);
void exception_trigger_test(void);

#endif