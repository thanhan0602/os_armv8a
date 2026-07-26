#include <kernel/ipc.h>

#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/wait_queue.h>

struct ipc_channel {
    struct spinlock lock;
    unsigned long has_message;
    unsigned long length;
    unsigned char buffer[IPC_MESSAGE_MAX];
    struct wait_queue receivers;
};

static struct ipc_channel ipc_channels[IPC_CHANNEL_MAX];

static int ipc_valid_channel(unsigned long channel_id)
{
    return channel_id < IPC_CHANNEL_MAX;
}

void ipc_init(void)
{
    unsigned long channel_index;

    for (channel_index = 0UL; channel_index < IPC_CHANNEL_MAX; channel_index++) {
        unsigned long byte_index;

        spinlock_init(&ipc_channels[channel_index].lock);
        ipc_channels[channel_index].has_message = 0UL;
        ipc_channels[channel_index].length = 0UL;
        wait_queue_init(&ipc_channels[channel_index].receivers);
        for (byte_index = 0UL; byte_index < IPC_MESSAGE_MAX; byte_index++) {
            ipc_channels[channel_index].buffer[byte_index] = 0U;
        }
    }
}

long ipc_send(unsigned long channel_id, const unsigned char *data, unsigned long len)
{
    struct ipc_channel *channel;
    struct task *waiting_receiver = (struct task *)0;
    unsigned long flags;
    unsigned long index;

    if (!ipc_valid_channel(channel_id) || data == (const unsigned char *)0 ||
        len == 0UL || len > IPC_MESSAGE_MAX) {
        return IPC_RESULT_ERROR;
    }

    channel = &ipc_channels[channel_id];
    flags = spin_lock_irqsave(&channel->lock);
    if (channel->has_message != 0UL) {
        spin_unlock_irqrestore(&channel->lock, flags);
        return IPC_RESULT_ERROR;
    }

    for (index = 0UL; index < len; index++) {
        channel->buffer[index] = data[index];
    }
    channel->length = len;
    channel->has_message = 1UL;

    waiting_receiver = wait_queue_dequeue(&channel->receivers);

    spin_unlock_irqrestore(&channel->lock, flags);

    /* Never acquire sched_lock while holding channel->lock. */
    if (waiting_receiver != (struct task *)0) {
        sched_unpark_task(waiting_receiver);
    }
    return (long)len;
}

long ipc_receive(unsigned long channel_id,
                 struct task *task,
                 unsigned char *data,
                 unsigned long capacity)
{
    struct ipc_channel *channel;
    unsigned long flags;
    unsigned long index;

    if (!ipc_valid_channel(channel_id) || task == (struct task *)0 ||
        data == (unsigned char *)0 || capacity == 0UL || capacity > IPC_MESSAGE_MAX) {
        return IPC_RESULT_ERROR;
    }

    channel = &ipc_channels[channel_id];
    flags = spin_lock_irqsave(&channel->lock);
    if (channel->has_message != 0UL) {
        if (capacity < channel->length) {
            spin_unlock_irqrestore(&channel->lock, flags);
            return IPC_RESULT_ERROR;
        }

        for (index = 0UL; index < channel->length; index++) {
            data[index] = channel->buffer[index];
        }

        channel->has_message = 0UL;
        index = channel->length;
        channel->length = 0UL;
        spin_unlock_irqrestore(&channel->lock, flags);
        return (long)index;
    }

    if (!wait_queue_enqueue(&channel->receivers, task) &&
        !wait_queue_contains(&channel->receivers, task)) {
        spin_unlock_irqrestore(&channel->lock, flags);
        return IPC_RESULT_ERROR;
    }
    spin_unlock_irqrestore(&channel->lock, flags);

    /*
     * The sender may remove and unpark this waiter between the channel unlock
     * and this call. sched_park_task() consumes that pending wakeup token and
     * therefore cannot lose the notification.
     */
    (void)sched_park_task(task);
    return IPC_RESULT_BLOCKED;
}

void ipc_detach_task(struct task *task)
{
    unsigned long channel_index;

    if (task == (struct task *)0) {
        return;
    }

    for (channel_index = 0UL; channel_index < IPC_CHANNEL_MAX; channel_index++) {
        struct ipc_channel *channel;
        unsigned long flags;

        channel = &ipc_channels[channel_index];
        flags = spin_lock_irqsave(&channel->lock);
        (void)wait_queue_remove(&channel->receivers, task);
        spin_unlock_irqrestore(&channel->lock, flags);
    }
}