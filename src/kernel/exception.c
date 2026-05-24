#include <kernel/exception.h>

#include <drivers/interrupt/gicv2.h>
#include <kernel/log.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include <kernel/timer.h>

extern char exception_vector_table[];

#define EXCEPTION_CONTEXT_SIZE 800UL

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

static void exception_dump_registers(const struct exception_context *context)
{
    unsigned long index;

    if (context == (const struct exception_context *)0) {
        KER_INFO("exception register dump unavailable");
        return;
    }

    for (index = 0UL; index < 31UL; index += 2UL) {
        if ((index + 1UL) < 31UL) {
            KER_LOGF("[fault] x%lu=%lx x%lu=%lx\n",
                     index, context->gpr[index],
                     index + 1UL, context->gpr[index + 1UL]);
        } else {
            KER_LOGF("[fault] x%lu=%lx\n", index, context->gpr[index]);
        }
    }

    KER_LOGF("[fault] SP_EL1(ex-entry)=%lx\n",
             (unsigned long)context + EXCEPTION_CONTEXT_SIZE);
    KER_LOGF("[fault] FPCR=%lx FPSR=%lx\n", context->fpcr, context->fpsr);
}

void exception_init(void)
{
    unsigned long vbar;

    /*
     * Use adrp+add to compute the PC-relative address of the vector table.
     * This returns the physical address when running pre-trampoline and the
     * kernel virtual address when running post-trampoline, so the same
     * function works correctly in both contexts.
     */
    __asm__ volatile(
        "adrp %0, exception_vector_table\n"
        "add  %0, %0, :lo12:exception_vector_table\n"
        "msr vbar_el1, %0\n"
        "isb\n"
        : "=r"(vbar)
        :
        : "memory");
}

static void exception_dump(unsigned long vector_id,
                           unsigned long esr_el1,
                           unsigned long elr_el1,
                           unsigned long spsr_el1,
                           unsigned long far_el1,
                           const struct exception_context *context)
{
    KER_LOGF("[fault] vector=%s\n", exception_vector_name(vector_id));
    KER_LOGF("[fault] ESR_EL1=%lx\n", esr_el1);
    KER_LOGF("[fault] ELR_EL1=%lx\n", elr_el1);
    KER_LOGF("[fault] SPSR_EL1=%lx\n", spsr_el1);
    KER_LOGF("[fault] FAR_EL1=%lx\n", far_el1);

    exception_dump_registers(context);

    KER_INFO("exception handler parked the CPU");
}

/*
 * ESR_EL1 exception class (EC) field: bits [31:26].
 * EC = 0x15 means "SVC instruction in AArch64 state from EL0".
 */
#define ESR_EC_SHIFT   26UL
#define ESR_EC_MASK    0x3FUL
#define ESR_EC_SVC64   0x15UL

int exception_handle_sync(unsigned long vector_id,
                           unsigned long esr_el1,
                           unsigned long elr_el1,
                           unsigned long spsr_el1,
                           unsigned long far_el1,
                           struct exception_context *context)
{
    unsigned long ec = (esr_el1 >> ESR_EC_SHIFT) & ESR_EC_MASK;

    /*
     * SVC from EL0 (AArch64): dispatch to the syscall handler and
     * return 1 so the assembly stub does restore_context + eret.
     */
    if (ec == ESR_EC_SVC64) {
        syscall_dispatch(context->gpr[8], context);
        return 1;
    }

    exception_dump(vector_id, esr_el1, elr_el1, spsr_el1, far_el1, context);

    while (1) {
        __asm__ volatile("wfe");
    }

    return 0;
}

void exception_handle_irq(unsigned long vector_id,
                          unsigned long esr_el1,
                          unsigned long elr_el1,
                          unsigned long spsr_el1,
                          unsigned long far_el1,
                          const struct exception_context *context)
{
    unsigned int intid;

    (void)esr_el1;
    (void)elr_el1;
    (void)spsr_el1;
    (void)far_el1;
    (void)context;

    intid = gicv2_acknowledge_irq();
    if (intid >= 1020U) {
        return;
    }

    if (!timer_handle_irq(intid)) {
        KER_LOGF("[irq] unexpected vector=%s\n", exception_vector_name(vector_id));
        KER_LOGF("[irq] unexpected intid=%u\n", intid);
    }

    gicv2_end_of_interrupt(intid);

    schedule();
}

void exception_trigger_test(void)
{
    __asm__ volatile("brk #0");
}