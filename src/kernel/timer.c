#include <kernel/timer.h>

#include <arch/arm/virt.h>
#include <drivers/interrupt/gicv2.h>
#include <kernel/log.h>

#define CNTV_CTL_ENABLE  (1UL << 0)
#define TIMER_PPI        QEMU_VIRT_TIMER_PPI

static volatile unsigned long timer_ticks;
static unsigned long timer_frequency;
static unsigned long timer_interval;

static void local_irq_enable(void)
{
    __asm__ volatile(
        "msr daifclr, #2\n"
        "isb\n"
        ::: "memory");
}

static void timer_program_next_tick(void)
{
    unsigned long timer_ctl;

    __asm__ volatile("msr cntv_tval_el0, %0" : : "r"(timer_interval));

    timer_ctl = CNTV_CTL_ENABLE;
    __asm__ volatile("msr cntv_ctl_el0, %0" : : "r"(timer_ctl));
    __asm__ volatile("isb");
}

void timer_init(void)
{
    gicv2_init();
    gicv2_enable_irq(TIMER_PPI);

    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(timer_frequency));
    timer_interval = timer_frequency / 2UL;
    if (timer_interval == 0UL) {
        timer_interval = 1UL;
    }

    timer_program_next_tick();
    local_irq_enable();

    KER_LOGF("[info] generic timer frequency=%lu\n", timer_frequency);
    KER_LOGF("[info] timer interval cycles=%lu\n", timer_interval);
    KER_INFO("stage 4 timer irq enabled");
}

int timer_handle_irq(unsigned int intid)
{
    if (intid != TIMER_PPI) {
        return 0;
    }

    timer_ticks++;
    timer_program_next_tick();

    if (timer_ticks <= 4UL) {
        KER_LOGF("[info] timer tick=%lu\n", timer_ticks);
    }

    return 1;
}

unsigned long timer_tick_count(void)
{
    return timer_ticks;
}