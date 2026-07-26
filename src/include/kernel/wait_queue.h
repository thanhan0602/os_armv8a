#ifndef KERNEL_WAIT_QUEUE_H
#define KERNEL_WAIT_QUEUE_H

struct task;

/*
 * Intrusive FIFO wait queue using task->wait_next.
 *
 * The queue does not provide internal locking. Callers must serialize every
 * operation with the lock that protects the containing subsystem object.
 * Scheduler park/unpark calls must still happen after releasing that lock.
 */
struct wait_queue {
    struct task *head;
    struct task *tail;
};

void wait_queue_init(struct wait_queue *queue);
int wait_queue_empty(const struct wait_queue *queue);
int wait_queue_contains(const struct wait_queue *queue,
                        const struct task *task);
int wait_queue_enqueue(struct wait_queue *queue, struct task *task);
struct task *wait_queue_dequeue(struct wait_queue *queue);
int wait_queue_remove(struct wait_queue *queue, struct task *task);

#endif
