#include <kernel/exception.h>

#include <drivers/interrupt/gicv2.h>
#include <arch/arm/cpu.h>
#include <arch/arm/sysregs.h>
#include <kernel/log.h>
#include <kernel/mmu.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include <kernel/timer.h>

extern char exception_vector_table[];

#define EXCEPTION_CONTEXT_SIZE 800UL

static const char *exception_vector_name(unsigned long vector_id)
{
    /*
     * Use a 2D char array — NOT a const char *[] pointer array.
     * A pointer array stores absolute link-time VMA (= PA) values in
     * .rodata.  After mmu_install_empty_ttbr0_root() those PA values
     * cannot be dereferenced (TTBR0 is empty).  A char[][] stores the
     * strings inline; &names[i][0] is computed at runtime via adrp
     * (PC-relative) and therefore gives a correct kernel VA.
     */
    static const char names[16][28] = {
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
    KER_LOGF("[fault] SP_EL0=%lx\n", context->sp_el0);
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
        : "=r"(vbar)
        :
        : "memory");

    arch_set_vbar_el1(vbar);
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
 * EC = 0x15 : SVC from EL0 (AArch64)
 * EC = 0x20 : Instruction Abort from lower EL
 * EC = 0x24 : Data Abort from lower EL
 */
#define ESR_EC_SHIFT      26UL
#define ESR_EC_MASK       0x3FUL
#define ESR_EC_SVC64      0x15UL
#define ESR_EC_IABT_LOW   0x20UL
#define ESR_EC_DABT_LOW   0x24UL
/* EL1 self-faults — used to catch nested kernel exceptions */
#define ESR_EC_IABT_CUR   0x21UL
#define ESR_EC_DABT_CUR   0x25UL

/* Fault Status Code (FSC): ESR_EL1[5:0] */
#define ESR_ISS_FSC_MASK       0x3FUL
#define ESR_ISS_FSC_TRANSL     0x04UL   /* translation fault (bits [5:2]=0001) */
#define ESR_ISS_FSC_ACCFLAG    0x08UL   /* access flag fault (bits [5:2]=0010) */
#define ESR_ISS_FSC_PERM       0x0CUL   /* permission fault  (bits [5:2]=0011) */
#define ESR_ISS_FSC_TYPE_MASK  0x3CUL   /* bits [5:2]: fault type */
#define ESR_ISS_FSC_LEVEL_MASK 0x03UL   /* bits [1:0]: table walk level */
/* WnR (Write not Read): ESR_EL1[6] — set for data abort on write */
#define ESR_DABT_WNR           (1UL << 6)

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

    /*
     * Data or Instruction Abort from EL0: log the faulting address and
     * kill the offending user task.  task_exit() marks the task DEAD
     * and calls schedule(), so it does not return.
     */
    if (ec == ESR_EC_DABT_LOW || ec == ESR_EC_IABT_LOW) {
        if (mmu_handle_page_fault(far_el1, esr_el1)) {
            return 1;
        }

        struct task *curr = sched_current();
        unsigned long fsc       = esr_el1 & ESR_ISS_FSC_MASK;
        unsigned long fsc_type  = fsc & ESR_ISS_FSC_TYPE_MASK;
        unsigned long fsc_level = fsc & ESR_ISS_FSC_LEVEL_MASK;
        const char *kind   = (ec == ESR_EC_DABT_LOW) ? "data" : "instruction";
        const char *access = (ec == ESR_EC_DABT_LOW)
                               ? ((esr_el1 & ESR_DABT_WNR) ? "write" : "read")
                               : "fetch";

        /*
         * Do NOT store fault-type as a const char * variable: GCC -O2
         * may optimise the if-else into a jump table whose entries are
         * link-time PA addresses stored in .rodata.  After the empty
         * TTBR0 root is installed those PA values are inaccessible.
         * Use separate log_write() calls so each string literal address
         * is computed via adrp (PC-relative → kernel VA) at the call site.
         */
        if (curr) {
            KER_LOGF("[fault] [%s:%lu] EL0 %s abort (%s ", curr->name, curr->id, kind, access);
        } else {
            KER_LOGF("[fault] EL0 %s abort (%s ", kind, access);
        }
        if (fsc_type == ESR_ISS_FSC_TRANSL) {
            log_write("translation");
        } else if (fsc_type == ESR_ISS_FSC_PERM) {
            log_write("permission");
        } else if (fsc_type == ESR_ISS_FSC_ACCFLAG) {
            log_write("access-flag");
        } else {
            log_write("other");
        }
        KER_LOGF(" L%lu): FAR=%lx ELR=%lx ESR=%lx\n",
                 fsc_level, far_el1, elr_el1, esr_el1);
        KER_INFO("[fault] killing user task");
        task_exit();
        while (1) {
            schedule();
        }
    }

    exception_dump(vector_id, esr_el1, elr_el1, spsr_el1, far_el1, context);

    /* Distinguish nested EL1 aborts so they are visible in the log. */
    if (ec == ESR_EC_DABT_CUR || ec == ESR_EC_IABT_CUR) {
        KER_LOGF("[PANIC] EL1 %s abort FAR=%lx ELR=%lx ESR=%lx\n",
                 (ec == ESR_EC_DABT_CUR) ? "data" : "instruction",
                 far_el1, elr_el1, esr_el1);
    }

    while (1) {
        cpu_wfe();
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

    if (intid == 0U) {
        /* IPI 0: reschedule request */
        /* ACK/EOI is enough to clear the SGI */
    } else if (!timer_handle_irq(intid)) {
        KER_LOGF("[irq] unexpected vector=%s\n", exception_vector_name(vector_id));
        KER_LOGF("[irq] unexpected intid=%u\n", intid);
    }

    gicv2_end_of_interrupt(intid);

    schedule();
}

void exception_trigger_test(void)
{
    cpu_brk(0);
}