#include <kernel/wait_queue.h>
#include <kernel/sched.h>

void wait_queue_init(struct wait_queue *queue)
{
    if (queue == (struct wait_queue *)0) {
        return;
    }
    queue->head = (struct task *)0;
    queue->tail = (struct task *)0;
}

int wait_queue_empty(const struct wait_queue *queue)
{
    return queue == (const struct wait_queue *)0 ||
           queue->head == (struct task *)0;
}

int wait_queue_contains(const struct wait_queue *queue,
                        const struct task *task)
{
    const struct task *current;

    if (queue == (const struct wait_queue *)0 ||
        task == (const struct task *)0) {
        return 0;
    }

    current = queue->head;
    while (current != (const struct task *)0) {
        if (current == task) {
            return 1;
        }
        current = current->wait_next;
    }
    return 0;
}

int wait_queue_enqueue(struct wait_queue *queue, struct task *task)
{
    if (queue == (struct wait_queue *)0 || task == (struct task *)0 ||
        wait_queue_contains(queue, task)) {
        return 0;
    }

    task->wait_next = (struct task *)0;
    if (queue->tail == (struct task *)0) {
        queue->head = task;
        queue->tail = task;
    } else {
        queue->tail->wait_next = task;
        queue->tail = task;
    }
    return 1;
}

struct task *wait_queue_dequeue(struct wait_queue *queue)
{
    struct task *task;

    if (queue == (struct wait_queue *)0 ||
        queue->head == (struct task *)0) {
        return (struct task *)0;
    }

    task = queue->head;
    queue->head = task->wait_next;
    if (queue->head == (struct task *)0) {
        queue->tail = (struct task *)0;
    }
    task->wait_next = (struct task *)0;
    return task;
}

int wait_queue_remove(struct wait_queue *queue, struct task *task)
{
    struct task *previous = (struct task *)0;
    struct task *current;

    if (queue == (struct wait_queue *)0 || task == (struct task *)0) {
        return 0;
    }

    current = queue->head;
    while (current != (struct task *)0) {
        if (current == task) {
            if (previous == (struct task *)0) {
                queue->head = current->wait_next;
            } else {
                previous->wait_next = current->wait_next;
            }
            if (queue->tail == current) {
                queue->tail = previous;
            }
            current->wait_next = (struct task *)0;
            return 1;
        }
        previous = current;
        current = current->wait_next;
    }
    return 0;
}
