#include <kernel/exception.h>

#include <drivers/interrupt/gicv2.h>
#include <kernel/log.h>
#include <kernel/timer.h>

extern char exception_vector_table[];

static const char *exception_vector_name(unsigned long vector_id)
{
    static const char *const names[16] = {
        "current_el_sp0_sync",
        "current_el_sp0_irq",
        "current_el_sp0_fiq",
        "current_el_sp0_serror",
        "current_el_spx_sync",
        "current_el_spx_irq",
        "current_el_spx_fiq",
        "current_el_spx_serror",
        "lower_el_aarch64_sync",
        "lower_el_aarch64_irq",
        "lower_el_aarch64_fiq",
        "lower_el_aarch64_serror",
        "lower_el_aarch32_sync",
        "lower_el_aarch32_irq",
        "lower_el_aarch32_fiq",
        "lower_el_aarch32_serror",
    };

    if (vector_id < 16UL) {
        return names[vector_id];
    }

    return "unknown";
}

void exception_init(void)
{
    __asm__ volatile(
        "msr vbar_el1, %0\n"
        "isb\n"
        :
        : "r"(exception_vector_table)
        : "memory");
}

static void exception_dump(unsigned long vector_id,
                           unsigned long esr_el1,
                           unsigned long elr_el1,
                           unsigned long spsr_el1,
                           unsigned long far_el1)
{
    log_write("[fault] vector=");
    log_write(exception_vector_name(vector_id));
    log_putc('\n');

    log_write("[fault] ESR_EL1=");
    log_write_hex(esr_el1);
    log_putc('\n');

    log_write("[fault] ELR_EL1=");
    log_write_hex(elr_el1);
    log_putc('\n');

    log_write("[fault] SPSR_EL1=");
    log_write_hex(spsr_el1);
    log_putc('\n');

    log_write("[fault] FAR_EL1=");
    log_write_hex(far_el1);
    log_putc('\n');

    log_info("exception handler parked the CPU");
}

void exception_handle_sync(unsigned long vector_id,
                           unsigned long esr_el1,
                           unsigned long elr_el1,
                           unsigned long spsr_el1,
                           unsigned long far_el1)
{
    exception_dump(vector_id, esr_el1, elr_el1, spsr_el1, far_el1);

    while (1) {
        __asm__ volatile("wfe");
    }
}

void exception_handle_irq(unsigned long vector_id,
                          unsigned long esr_el1,
                          unsigned long elr_el1,
                          unsigned long spsr_el1,
                          unsigned long far_el1)
{
    unsigned int intid;

    (void)esr_el1;
    (void)elr_el1;
    (void)spsr_el1;
    (void)far_el1;

    intid = gicv2_acknowledge_irq();
    if (intid >= 1020U) {
        return;
    }

    if (!timer_handle_irq(intid)) {
        log_write("[irq] unexpected vector=");
        log_write(exception_vector_name(vector_id));
        log_putc('\n');
        log_write("[irq] unexpected intid=");
        log_write_u64(intid);
        log_putc('\n');
    }

    gicv2_end_of_interrupt(intid);
}

void exception_trigger_test(void)
{
    __asm__ volatile("brk #0");
}