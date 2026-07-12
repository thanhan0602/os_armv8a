# Mutex Implementation HLD/LLD

## High-Level Design (HLD)

### Goal
Implement a blocking synchronization primitive (Mutex) for the AArch64 SMP kernel that allows a task to sleep when the lock is unavailable, preventing CPU waste (unlike spinlocks).

### Data Flow
1. **Task A** calls `mutex_lock`.
2. If `mutex->locked == 0`:
   - Set `mutex->locked = 1`.
   - Set `mutex->owner = Task A`.
3. If `mutex->locked == 1`:
   - Add **Task A** to `mutex->wait_queue`.
   - Set **Task A->state = TASK_STATE_BLOCKED`.
   - Trigger `schedule()` to switch to another task.
4. **Task B** calls `mutex_unlock`.
5. If `mutex->wait_queue` is empty:
   - Set `mutex->locked = 0`.
   - Set `mutex->owner = NULL`.
6. If `mutex->wait_queue` is not empty:
   - Pop **Task C** from `wait_queue`.
   - Set `mutex->owner = Task C`.
   - Set **Task C->state = TASK_STATE_READY`.
   - Send IPI (SGI 0) to other cores to signal rescheduling.

### Architecture
- **SMP Safety**: Use an internal `spinlock_t` within the mutex structure to protect the `locked` flag and the `wait_queue`.
- **Interrupt Safety**: Use `irqsave/irqrestore` when acquiring the internal spinlock.

## Low-Level Design (LLD)

### Data Structures
```c
struct mutex {
    struct spinlock lock;     // Internal spinlock for SMP safety
    int locked;               // 0 = free, 1 = locked
    struct task *owner;       // Current owner (for debugging/tracking)
    struct task *wait_queue;  // Simple linked list of waiting tasks (using task->wait_next)
};
```

### Registers and Instructions
- `ldaxr` / `stxr`: Used inside the `spinlock` implementation for atomic access to the mutex's internal lock.
- `tpidr_el1`: Used to get the current task via `arch_get_current_task()`.

### Algorithms
- **mutex_lock(m)**:
    1. `flags = spin_lock_irqsave(&m->lock)`
    2. If `!m->locked`:
        - `m->locked = 1`, `m->owner = current`
        - `spin_unlock_irqrestore(&m->lock, flags)`
        - Return.
    3. Else:
        - `current->wait_next = NULL`
        - Enqueue `current` to `m->wait_queue`.
        - `sched_block_task(current)`
        - `spin_unlock_irqrestore(&m->lock, flags)`
        - `schedule()`
        - Return (we are now owner).

- **mutex_unlock(m)**:
    1. `flags = spin_lock_irqsave(&m->lock)`
    2. If `m->wait_queue == NULL`:
        - `m->locked = 0`, `m->owner = NULL`
    3. Else:
        - `next = dequeue(m->wait_queue)`
        - `m->owner = next`
        - `sched_wake_task(next)`
    4. `spin_unlock_irqrestore(&m->lock, flags)`

## Plan
1. [DONE] Modify `struct task` to include `wait_next`.
2. [DONE] Define `struct mutex` in `src/include/kernel/mutex.h`.
3. [DONE] Implement `mutex_init`, `mutex_lock`, `mutex_unlock` in `src/kernel/mutex.c`.
4. [DONE] Add verification task in `src/kernel/mutex_test.c`.
5. [DONE] Verify on QEMU.
6. [TODO] Update documentation (Handoff already done, need to check roadmap).
