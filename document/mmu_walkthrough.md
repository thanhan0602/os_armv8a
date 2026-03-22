# MMU Walkthrough

This note matches the current implementation in `src/kernel/mmu.c`.

## Goal

The kernel enables a simple and debuggable EL1 Stage-1 MMU setup on QEMU `virt`.

Current priorities are:

- keep boot stable
- keep `VA = PA` during bring-up
- give kernel sections meaningful permissions
- avoid spending too many physical pages on translation tables

## Translation Model

- Exception level: `EL1`
- Translation stage: `Stage-1`
- Granule size: `4 KiB`
- Virtual address size: `48-bit`
- Table depth: `L0 -> L1 -> L2 -> L3`
- Root register: `TTBR0_EL1`

The kernel does not currently use `TTBR1_EL1`.

## Current Mapping Strategy

The design is an identity map.

- virtual address equals physical address
- low MMIO is mapped as device memory and marked execute-never
- RAM starts through an `L2` table
- the early kernel window drops to `L3` pages for fine-grained permissions
- the rest of RAM stays as `L2` blocks for lower table overhead

In practice the layout is:

```text
TTBR0_EL1
  |
  v
L0 root
  |
  +-- L1 low region -> device block mapping for MMIO
  |
  +-- L1 RAM region -> L2 RAM table
                        |
                        +-- early chunks -> L3 page tables
                        |                   .text   = RO + X
                        |                   .rodata = RO + NX
                        |                   .data   = RW + NX
                        |                   .bss    = RW + NX
                        |                   stack   = RW + NX
                        |
                        +-- later chunks -> L2 blocks = normal memory, RW + NX
```

## Important Descriptor Bits

The page-table entries use these fields:

- `VALID` bit: descriptor is usable
- `TABLE` bit: entry points to the next-level table
- `AttrIndx[2:0]`: selects a memory type from `MAIR_EL1`
- `AP[7:6]`: read/write permission
- `SH[9:8]`: shareability
- `AF[10]`: access flag
- `PXN`: privileged execute-never
- `UXN`: unprivileged execute-never

## Registers Used During MMU Enable

### `MAIR_EL1`

Defines the memory attribute slots referenced by `AttrIndx`.

- slot `0` = `Device-nGnRnE`
- slot `1` = normal memory, write-back/write-allocate cacheable

### `TCR_EL1`

Defines how addresses from `TTBR0_EL1` are interpreted.

- `T0SZ[5:0] = 16` for a `48-bit` virtual address space
- `IRGN0[9:8] = 01` for inner write-back/write-allocate walks
- `ORGN0[11:10] = 01` for outer write-back/write-allocate walks
- `SH0[13:12] = 11` for inner-shareable walks
- `TG0[15:14] = 00` for `4 KiB` granules
- `EPD1[23] = 1` so `TTBR1_EL1` walks are disabled
- `IPS[34:32] = 101` for `48-bit` physical address size

### `TTBR0_EL1`

Holds the base address of the `L0` root table.

### `SCTLR_EL1`

This is the final switch.

- `M[0]` enables Stage-1 translation
- `C[2]` enables the data/unified cache
- `I[12]` enables the instruction cache

The implementation also sets the architectural `RES1` bits required at EL1.

## Enable Sequence

The code brings the MMU online in this order:

1. allocate translation-table pages from the page allocator
2. build the identity map and section permissions
3. write `MAIR_EL1`
4. write `TCR_EL1`
5. write `TTBR0_EL1`
6. invalidate stale EL1 translations with `TLBI VMALLE1`
7. execute barriers with `DSB` and `ISB`
8. set `SCTLR_EL1.M`, `SCTLR_EL1.C`, and `SCTLR_EL1.I`
9. execute a final `ISB`

## Why Identity Map First

This is not the final architecture. It is the safest bring-up baseline because:

- early debug is much simpler when logged addresses are physical addresses
- UART, GIC, timer, and page-table memory stay easy to reason about
- software walks and hardware probes can be compared directly

Later stages can introduce a higher-half kernel or separate virtual layout.

## Debug Hooks

The kernel already includes MMU debug helpers:

- software table walks
- `AT S1E1R` plus `PAR_EL1` probes
- table-page inventory logs
- named boot debug targets

These are useful when checking whether a fault comes from:

- a missing valid bit
- a wrong table pointer
- a permission issue
- a memory-type mismatch