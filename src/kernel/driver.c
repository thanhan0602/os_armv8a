#include <kernel/driver.h>

#include <drivers/interrupt/gicv2.h>
#include <drivers/uart/pl011.h>
#include <kernel/log.h>
#include <kernel/timer.h>

static int uart_ready;
static int gic_ready;
static int timer_ready;

int driver_uart_is_ready(void)
{
    return uart_ready;
}

int driver_gic_is_ready(void)
{
    return gic_ready;
}

int driver_timer_is_ready(void)
{
    return timer_ready;
}

void driver_system_init(void)
{
    if (!uart_ready) {
        pl011_init();
        uart_ready = 1;
    }

    if (!gic_ready) {
        gicv2_init();
        gic_ready = 1;
    }

    if (!timer_ready) {
        timer_init();
        timer_ready = 1;
    }
}

void driver_system_dump(void)
{
    KER_LOGF("[driver] uart=%s gic=%s timer=%s\n",
             uart_ready ? "ready" : "off",
             gic_ready ? "ready" : "off",
             timer_ready ? "ready" : "off");
}