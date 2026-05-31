#ifndef DRIVERS_UART_PL011_H
#define DRIVERS_UART_PL011_H

void pl011_init(void);
void pl011_write(unsigned int value);
int pl011_can_read(void);
unsigned int pl011_read(void);

#endif