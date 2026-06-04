#ifndef KERNEL_DRIVER_H
#define KERNEL_DRIVER_H

void driver_system_init(void);
void driver_system_dump(void);

int driver_uart_is_ready(void);
int driver_gic_is_ready(void);
int driver_timer_is_ready(void);

#endif