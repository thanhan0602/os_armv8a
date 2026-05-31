# QEMU Inspector Architecture: HMP-First, GDB-Optional

## Mục tiêu

Tài liệu này mô tả kiến trúc hiện tại của `tools/qemu-inspector` và hướng mở rộng sau này nếu muốn quan sát không chỉ address translation mà cả process/task state của hệ thống.

Trọng tâm của thiết kế là tách rõ hai lớp:

- `HMP` dùng để đọc và điều khiển trạng thái VM ở mức monitor QEMU
- `GDB RSP` dùng để đặt breakpoint, đọc register, và đồng bộ với debugger bên ngoài khi cần

Thiết kế dài hạn nên giữ `HMP` là đường đọc trạng thái chính, còn `GDB` là nguồn dừng thực thi và điều khiển debug.

## Các thành phần hiện tại

### 1. QEMU monitor qua HMP

Inspector kết nối vào cổng HMP của QEMU để thực hiện các thao tác sau:

- `info status`: đọc trạng thái VM
- `stop`: dừng VM để snapshot ổn định
- `cont`: tiếp tục VM khi endpoint sở hữu quyền resume
- `x /...`: đọc physical memory trực tiếp

Điểm mạnh của HMP là đọc được physical memory, rất phù hợp với:

- page table walks
- đọc `tasks[]`
- đọc `mm_context.root_pa`
- đọc raw memory theo PA

HMP không biết symbol semantics và không có breakpoint abstraction.

### 2. QEMU gdbstub qua GDB RSP

Inspector kết nối vào cổng GDB RSP để thực hiện:

- đặt/xóa software breakpoint (`Z0` / `z0`)
- `continue`
- `interrupt`
- đọc registers
- đọc memory theo địa chỉ target nếu cần

GDB RSP là lớp đúng để đồng bộ với debugger, nhưng không phải lớp tốt nhất để xây dựng data-plane cho memory introspection của kernel, vì inspector đang cần đọc physical layout và các cấu trúc nằm trong RAM theo quy ước kernel.

### 3. FastAPI server

`tools/qemu-inspector/server.py` hiện đóng vai trò orchestrator:

- giữ HMP client
- giữ GDB client
- resolve symbol từ `build/kernel8.elf`
- cung cấp REST API cho web UI

Server hiện có hai mode quan trọng:

- mode cũ: inspector tự đặt breakpoint rồi snapshot (`/api/gdb/break_and_snapshot`)
- mode mới: inspector chỉ đọc HMP khi VM đã bị debugger ngoài dừng sẵn (`/api/hmp/snapshot`)

### 4. Web UI

`tools/qemu-inspector/index.html` hiện là thin client:

- gọi REST API
- hiển thị task list
- hiển thị page owners
- hiển thị VA walk
- hiển thị GDB breakpoint controls

Web UI không nên giữ logic phân tích kernel semantics. Phần đó phải ở backend.

## Tại sao nên đi theo HMP-first, GDB-optional

Nếu sau này developer debug bằng VS Code + GDB thật, inspector không nên tranh quyền điều khiển breakpoint với debugger IDE.

Kiến trúc nên là:

```text
VS Code GDB  <---->  QEMU gdbstub
                     |
                     |
Inspector UI/API <-> QEMU HMP
```

Điều này cho phép:

- debugger ngoài đặt breakpoint theo source line, symbol, stack frame
- inspector chỉ đọc snapshot từ HMP
- không có xung đột ownership đối với `continue`, breakpoint table, hoặc stop reason

Đây là lý do endpoint HMP-only có giá trị: khi VM đã ở `halted (gdb)`, inspector chỉ cần đọc, không cần resume.

## Semantics hiện tại của ownership

Hiện tại ownership của việc dừng/chạy VM được chia thành hai nhóm:

### Nhóm inspector-owned execution

Các API như `/api/gdb/break_and_snapshot` tự:

1. đặt breakpoint
2. continue VM
3. chờ breakpoint hit
4. snapshot qua HMP
5. xóa breakpoint

Mode này tiện cho demo nhanh, nhưng không phải mode tối ưu khi IDE đã gắn debugger.

### Nhóm debugger-owned execution

Các API HMP-only như `/api/hmp/snapshot` nên hoạt động theo quy tắc:

- không đặt breakpoint
- không giả định ownership của `continue`
- không tự resume VM
- chỉ đọc snapshot từ trạng thái halted hiện có

Mode này phù hợp với workflow:

1. VS Code GDB dừng tại breakpoint
2. inspector gọi HMP để đọc memory + task state
3. user bấm continue trong debugger IDE

## Giới hạn hiện tại

Inspector hiện chưa phải một process introspection framework tổng quát. Nó vẫn dựa vào knowledge cụ thể của kernel:

- symbol `tasks`
- layout cố định của `struct task`
- layout của `mm_context`
- logic page ownership suy ra từ page tables user

Điều này đủ cho address translation và task snapshot ở repo hiện tại, nhưng sẽ khó mở rộng nếu kernel thay đổi nhiều hoặc nếu muốn view nhiều lớp runtime state hơn.

## Hướng mở rộng: từ address translation sang system/process observability

Nếu mục tiêu sau này là xem các process đang chạy trong hệ thống, nên chuyển từ cách đọc ad hoc sang một debug ABI ổn định giữa kernel và inspector.

### Bước 1: chuẩn hóa kernel debug ABI

Kernel nên export một số điểm neo ổn định cho introspection:

- danh sách tasks/processes
- current task per CPU
- run queue hoặc scheduler state
- process address space root
- process name / pid / state / parent / exit code

Cách tốt là định nghĩa một `debug snapshot header` có version, thay vì để inspector phụ thuộc trực tiếp vào offset rời rạc của nhiều struct nội bộ.

Ví dụ ý tưởng:

```c
struct debug_task_snapshot {
    uint64_t pid;
    uint64_t state;
    uint64_t mm_root_pa;
    uint64_t kstack_top;
    uint64_t last_cpu;
    char name[16];
};

struct debug_system_snapshot {
    uint32_t version;
    uint32_t cpu_count;
    uint32_t task_count;
    uint32_t reserved;
    uint64_t current_task_pa[CONFIG_MAX_CPUS];
    struct debug_task_snapshot tasks[CONFIG_MAX_TASKS];
};
```

Lúc đó inspector chỉ cần biết một symbol gốc, ví dụ `kernel_debug_snapshot`, rồi parse theo version.

### Bước 2: tách các lớp dữ liệu trong inspector

Backend nên tách thành các adapter rõ ràng:

- `transport`: HMP, GDB
- `symbol resolver`: ELF symbol lookup
- `kernel schema`: parse task/mm/scheduler/debug ABI
- `view models`: tasks, processes, page owners, vm maps, scheduler state

Như vậy việc thêm view mới không kéo logic transport lẫn với logic kernel semantics.

### Bước 3: thêm các view hệ thống

Từ nền hiện tại, các view hợp lý tiếp theo là:

- danh sách task/process với state machine rõ ràng
- current task trên từng CPU
- kernel stacks per task
- address-space summary theo task
- open mappings hoặc vùng VA của từng process
- recent faults/exits
- scheduler run queue snapshot

### Bước 4: thêm navigation theo process thay vì chỉ theo VA

UI sau này nên cho phép:

- chọn process
- xem metadata của process
- xem root page table
- walk VA trong context process đó
- xem code/stack/heap/user mappings
- xem task đang chạy ở CPU nào

Điều này tốt hơn mô hình hiện tại là nhập VA thủ công rồi chọn task index.

## SMP: điều gì thay đổi khi hệ thống không còn một CPU logic duy nhất

Khi chuyển sang SMP thực sự, khó khăn lớn không còn là đọc page table mà là tính nhất quán của snapshot.

### 1. Một breakpoint không còn đồng nghĩa với toàn hệ thống đứng yên tại một điểm logic duy nhất

Nếu một CPU hit breakpoint:

- CPU đó halted
- CPU khác có thể vẫn còn đang chạy, tùy cơ chế QEMU/GDB mode
- scheduler state và run queue có thể đang bị thay đổi đồng thời

Do đó, muốn có snapshot nhất quán cần định nghĩa rõ:

- stop-the-world hay không
- nếu không stop-the-world, snapshot chỉ là best-effort

### 2. Cần per-CPU state rõ ràng

Khi có SMP, inspector cần đọc thêm:

- current task của mỗi CPU
- per-CPU run queue hoặc local scheduler state
- per-CPU exception frame nếu CPU đang halted trong kernel
- IPI / reschedule state nếu có

### 3. Breakpoint ownership phức tạp hơn

Nếu debugger IDE đang attach:

- continue/pause phải do debugger quyết định
- inspector không nên tự động `cont`
- inspector cần hiểu trạng thái kiểu `halted on cpu N`

### 4. TLB, ASID và shootdown trở thành một phần của observability

Khi phân tích process state trong SMP, chỉ nhìn page tables là chưa đủ. Có thể cần thêm thông tin:

- ASID theo address space
- pending TLB invalidation
- CPU nào đang chạy address space nào
- shootdown đang diễn ra hay chưa

Nếu muốn debug memory bugs thật sự trong SMP, inspector có thể cần view riêng cho `ASID/TLB activity` hoặc ít nhất là event log liên quan.

## Đề xuất kiến trúc dài hạn

### Nguyên tắc 1

`HMP` là đường đọc snapshot chính cho observability.

### Nguyên tắc 2

`GDB` là cơ chế điều khiển debug và breakpoint, không phải nơi chứa business logic của inspector.

### Nguyên tắc 3

Inspector không nên phụ thuộc quá sâu vào layout private của kernel nếu mục tiêu là mở rộng lâu dài. Nên có debug ABI versioned.

### Nguyên tắc 4

Mọi API đọc snapshot phải khai báo rõ ownership semantics:

- endpoint nào được phép `cont`
- endpoint nào tuyệt đối không `cont`
- endpoint nào yêu cầu VM đã halted sẵn

## API direction gợi ý

Một hướng hợp lý cho backend tương lai:

- `/api/hmp/snapshot`
  - snapshot task/process/page owners từ VM đã halted sẵn
- `/api/processes`
  - danh sách process/task với metadata ổn định
- `/api/process/{id}`
  - chi tiết một process
- `/api/process/{id}/walk?va=...`
  - walk VA trong context process
- `/api/cpus`
  - current task, PC, mode, state per CPU
- `/api/scheduler`
  - run queue, current task per CPU, dead/reap queues
- `/api/events`
  - fault/exit/schedule history nếu kernel export ring buffer debug

## Workflow khuyến nghị

### Workflow A: demo nhanh, không attach IDE debugger

- dùng `/api/gdb/break_and_snapshot`
- phù hợp để demo page table walk
- inspector tự sở hữu breakpoint và continue

### Workflow B: debug thật bằng VS Code GDB

- VS Code gắn GDB vào QEMU
- VS Code đặt breakpoint trong source code
- khi VM dừng, inspector gọi `/api/hmp/snapshot`
- user dùng debugger IDE để continue

Workflow B là hướng nên ưu tiên nếu sau này muốn mở rộng inspector thành system observability tool thay vì chỉ là demo MMU walker.

## Kết luận

Inspector hiện tại đã đủ tốt cho live page-table inspection, nhưng để đi tới quan sát process/system state bền vững thì cần ba thay đổi kiến trúc:

1. dịch từ parsing struct ad hoc sang debug ABI ổn định do kernel export
2. giữ HMP làm data plane đọc snapshot
3. để breakpoint/continue thuộc ownership của debugger bên ngoài khi có attach IDE

Nếu làm đúng ba điểm này, inspector có thể tiến từ một công cụ `view address translation` thành một công cụ `view live operating system state`.