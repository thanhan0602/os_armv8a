#ifndef KERNEL_IPC_H
#define KERNEL_IPC_H

struct task;

#define IPC_CHANNEL_MAX      8UL
#define IPC_MESSAGE_MAX      64UL
#define IPC_RESULT_ERROR     (-1L)
#define IPC_RESULT_BLOCKED   (-2L)

void ipc_init(void);
long ipc_send(unsigned long channel_id, const unsigned char *data, unsigned long len);
long ipc_receive(unsigned long channel_id,
                 struct task *task,
                 unsigned char *data,
                 unsigned long capacity);
void ipc_detach_task(struct task *task);

#endif