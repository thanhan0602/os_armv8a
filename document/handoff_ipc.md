# IPC handoff

## Mục tiêu hiện tại

Giữ một IPC slice nhỏ nhưng thực dụng cho runtime hiện tại:

- task có thể block trong syscall path
- kernel có thể wake đúng task khi event đến
- shell có thể verify end-to-end bằng user apps thật

## Trạng thái đã implement

- `src/kernel/ipc.c` + `src/include/kernel/ipc.h`
  - fixed channels `0..7`
  - mailbox một-message mỗi channel
  - `IPC_MESSAGE_MAX=64`
  - `ipc_send()` copy message vào kernel mailbox
  - `ipc_receive()` return ngay nếu có message, hoặc block task hiện tại nếu mailbox rỗng
  - receiver dùng `struct wait_queue` FIFO chung thay cho một raw `struct task *`, cho phép nhiều task chờ trên cùng channel
- `src/kernel/wait_queue.c` + `src/include/kernel/wait_queue.h`
  - cung cấp intrusive FIFO queue qua `task->wait_next`
  - hỗ trợ init, enqueue, dequeue, contains và remove
  - không tự khóa; caller phải giữ subsystem lock khi thao tác queue
- `src/kernel/sched.c` + `src/include/kernel/sched.h`
  - thêm `TASK_STATE_BLOCKED`
  - IPC blocking path dùng `sched_park_task()` và `sched_unpark_task()` với wake token để tránh lost wakeup
  - `ipc_detach_task()` được gọi khi task bị kill/reap để tránh dangling waiter pointer
- `src/kernel/syscall.c` + `src/include/kernel/syscall.h`
  - thêm `SYS_IPC_SEND=451`
  - thêm `SYS_IPC_RECV=452`
- `user/include/user/syscall.h`
  - thêm `user_ipc_send()` / `user_ipc_recv()`
- demo apps built-in:
  - `/bin/ipc_recv.elf`
  - `/bin/ipc_send.elf`

## Validation đã pass

Trên shell QEMU:

```text
load /bin/ipc_recv.elf
load /bin/ipc_send.elf
```

Output mong đợi:

```text
[ipc-send] sent message on channel 1
[ipc-recv] got: hello from ipc_send
[syscall] user task exited code=0
[syscall] user task exited code=0
```

## Bug quan trọng vừa fix

IPC là increment đầu tiên làm user task sleep ngay trong syscall path. Điều đó lộ ra một bug exception-frame cũ:

- trước đó kernel đã save/restore `ELR_EL1` và `SPSR_EL1`
- nhưng chưa save/restore `SP_EL0`
- kết quả: sau khi wake từ `SYS_IPC_RECV`, user task return về đúng EL0 PC nhưng local stack pointer sai, gây EL0 data abort trên local variables

Fix hiện tại nằm ở:

- `src/arch/arm/exception_vectors.S`
- `src/include/kernel/exception.h`
- `src/kernel/exception.c`

## SMP locking và lifetime

- Receiver được enqueue vào `channel->receivers` dưới `channel->lock`, sau đó channel lock được nhả trước khi gọi `sched_park_task()`.
- Sender dequeue đúng một waiter theo FIFO dưới `channel->lock`, nhả khóa, rồi mới gọi `sched_unpark_task()`.
- Nếu sender wake trước khi receiver kịp park, scheduler lưu `wake_pending`; lần park kế tiếp tiêu thụ token thay vì chuyển task sang BLOCKED.
- Reaper nhả `sched_lock` trước khi gọi `ipc_detach_task()`, vì vậy không còn cạnh khóa ngược `sched_lock -> channel->lock`.
- Task slot chỉ được clear và tái sử dụng sau khi IPC và mutex detach hoàn tất.

## Limitations hiện tại

- mailbox chỉ chứa được một message tại một thời điểm
- `send` hiện fail nếu mailbox đang đầy, không block
- chưa có channel create/destroy động
- chưa có close semantics, broadcast, stream/pipe semantics, hoặc request/reply routing
- Wait queue vẫn intrusive qua `task->wait_next`, nên một task chỉ có thể nằm trong một subsystem wait queue tại một thời điểm. Reaper vẫn phải detach trước khi clear và tái sử dụng task slot.

## Hướng đi hợp lý tiếp theo

1. Thêm regression riêng cho nhiều IPC receiver, FIFO wake và kill waiter giữa queue.
2. Quyết định primitive kế tiếp: semaphore/condition variable hay pipe/stream.
3. Nếu đi tiếp tới process manager, cân nhắc thêm `waitpid`-style event IPC thay vì mở rộng shell polling.
