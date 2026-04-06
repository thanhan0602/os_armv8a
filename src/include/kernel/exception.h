#ifndef KERNEL_EXCEPTION_H
#define KERNEL_EXCEPTION_H

struct exception_context {
    unsigned long gpr[31];
    unsigned long reserved0;
    unsigned long fpcr;
    unsigned long fpsr;
};

void exception_init(void);
void exception_handle_sync(unsigned long vector_id,
                           unsigned long esr_el1,
                           unsigned long elr_el1,
                           unsigned long spsr_el1,
                           unsigned long far_el1,
                           const struct exception_context *context);
void exception_handle_irq(unsigned long vector_id,
                          unsigned long esr_el1,
                          unsigned long elr_el1,
                          unsigned long spsr_el1,
                          unsigned long far_el1,
                          const struct exception_context *context);
void exception_trigger_test(void);

#endif