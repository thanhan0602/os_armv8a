# QEMU Page Inspector

## Goal

Provide a VS Code visual tool for MMU/page inspection that reads live QEMU memory directly instead of relying on kernel UART logs.

## Current status

- Implemented inside `tools/vscode-qemu-log` as a new command: `qemuInspector.start`.
- The inspector opens a webview with:
  - summary cards for MMU/page allocator state
  - a VA->PA translation walk view
  - table inventory for MMU-owned page-table pages
  - a physical page grid showing `unused/free/allocated/table` pages
  - per-page details including mapping attributes when selected
- Symbol resolution uses `aarch64-linux-gnu-nm` on `build/kernel8.elf`.
- Live memory reads use the QEMU **HMP monitor socket** and `xp /Nbx ...` against physical memory.
- Runtime control uses HMP `stop`, `cont`, and `info status`.

## Why HMP, not GDB/QMP

- `aarch64-linux-gnu-gdb` is not available in this environment, so a GDB-driven backend was not viable.
- QMP TCP greeting was not reliably observable during validation in this workspace.
- HMP monitor over TCP was validated successfully with:
  - monitor banner on connect
  - `info status`
  - `xp /8bx 0x...`

## Touched files

- `tools/vscode-qemu-log/src/extension.ts`
- `tools/vscode-qemu-log/src/QemuInspectorBackend.ts`
- `tools/vscode-qemu-log/src/QemuInspectorPanel.ts`
- `tools/vscode-qemu-log/out/extension.js`
- `tools/vscode-qemu-log/out/QemuInspectorBackend.js`
- `tools/vscode-qemu-log/out/QemuInspectorPanel.js`
- `tools/vscode-qemu-log/package.json`

## Invariants

- Kernel symbol addresses are treated as physical addresses because the kernel linker VMA remains PA.
- The inspector is snapshot-based: `Pause + Snapshot` stops QEMU, reads memory, and leaves the VM paused until `Continue`.
- The page map is derived from:
  - allocator state in `page_state[]`
  - MMU table inventory in `mmu_table_page_*`
  - page-table walks rooted at `l0_table` and `l0_table_ttbr1`

## Verification

- Static validation:
  - VS Code diagnostics reported no errors in the new TS/JS files.
- Runtime validation:
  - confirmed HMP monitor banner on TCP connect
  - confirmed `info status`
  - confirmed `xp /8bx 0x0000000040091320`

## Known gaps

- The tool currently visualizes **snapshot state** and the translation walk process, not a time-ordered memory access trace.
- True access chronology would require either:
  - ingesting QEMU MMU trace output, or
  - explicit instrumentation of page faults/accesses.
- Because this workspace lacks Node/TypeScript build tooling, `src/` and `out/` were updated together manually.

## Next steps

1. Add an install/load workflow for the VS Code extension inside the repo docs.
2. Add chunk/region filters so the page grid can focus on allocator-managed RAM or MMU table pages.
3. Add a trace mode that can merge snapshot state with QEMU MMU trace events for temporal access visualization.