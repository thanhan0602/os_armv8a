#include <kernel/timer.h>

#include <arch/arm/virt.h>
#include <arch/arm/cpu.h>
#include <arch/arm/sysregs.h>
#include <drivers/interrupt/gicv2.h>
#include <kernel/log.h>
#include <kernel/sched.h>

#define CNTV_CTL_ENABLE  (1UL << 0)
#define TIMER_PPI        QEMU_VIRT_TIMER_PPI

static volatile unsigned long timer_ticks;
static unsigned long timer_frequency;
static unsigned long timer_interval;

static void timer_program_next_tick(void)
{
    unsigned long timer_ctl;

    arch_timer_set_cntv_tval(timer_interval);

    timer_ctl = CNTV_CTL_ENABLE;
    arch_timer_set_cntv_ctl(timer_ctl);
    cpu_isb();
}

void timer_init(void)
{
    gicv2_enable_irq(TIMER_PPI);

    timer_frequency = arch_timer_get_cntfrq();
    timer_interval = timer_frequency / 2UL;
    if (timer_interval == 0UL) {
        timer_interval = 1UL;
    }

    timer_program_next_tick();
    arch_local_irq_enable();
}

int timer_handle_irq(unsigned int intid)
{
    if (intid != TIMER_PPI) {
        return 0;
    }

    if (arch_get_cpu_id() == 0) {
        timer_ticks++;
        sched_tick();
    }

    timer_program_next_tick();

    return 1;
}

unsigned long timer_tick_count(void)
{
    return timer_ticks;
}