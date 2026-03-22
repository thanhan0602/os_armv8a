#ifndef KERNEL_BOOT_H
#define KERNEL_BOOT_H

extern volatile unsigned long boot_stage;
extern volatile unsigned long boot_heartbeat;

void kernel_main(void);

#endif