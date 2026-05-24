#!/usr/bin/env python3
"""QEMU Page Inspector – FastAPI backend.

Connects to QEMU HMP monitor via TCP, reads ELF symbols, decodes page tables
and physical page allocator state, serves a REST API for the HTML frontend.

Usage:
    python3 server.py [--host 127.0.0.1] [--port 7777]
                      [--elf build/kernel8.elf]
                      [--monitor-host 127.0.0.1] [--monitor-port 4444]
"""
from __future__ import annotations

import argparse
import asyncio
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

import uvicorn
from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import HTMLResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

# ── constants ────────────────────────────────────────────────────────────────
RAM_BASE = 0x40000000
RAM_SIZE = 0x08000000
RAM_END  = RAM_BASE + RAM_SIZE
PAGE_SIZE = 0x1000
PAGE_COUNT = RAM_SIZE // PAGE_SIZE
KERNEL_VA_OFFSET = 0xFFFF000000000000

MMU_VALID       = 1 << 0
MMU_TABLE       = MMU_VALID | (1 << 1)
MMU_BLOCK       = MMU_VALID
MMU_TYPE_MASK   = 0x3
MMU_ADDR_MASK   = 0x0000FFFFFFFFF000
MMU_L1_BLK_MASK = 0x0000FFFFC0000000
MMU_L2_BLK_MASK = 0x0000FFFFFFE00000
MMU_L3_PG_MASK  = 0x0000FFFFFFFFF000
MMU_ATTR_MASK   = 0x1C
MMU_ATTR_DEVICE = 0x0
MMU_ATTR_NORMAL = 0x4
MMU_AP_RO       = 0x80
MMU_SH_INNER    = 0x300
MMU_AF          = 0x400
MMU_PXN         = 1 << 53

# ── HMP client ───────────────────────────────────────────────────────────────
class HmpClient:
    def __init__(self, host: str, port: int):
        self._host = host
        self._port = port
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._lock = asyncio.Lock()

    async def connect(self) -> None:
        self._reader, self._writer = await asyncio.open_connection(self._host, self._port)
        # drain banner
        await asyncio.wait_for(self._drain_until_prompt(), timeout=5.0)

    async def _drain_until_prompt(self) -> str:
        buf = ""
        while True:
            chunk = await asyncio.wait_for(self._reader.read(4096), timeout=3.0)
            buf += chunk.decode("utf-8", errors="replace")
            if "(qemu)" in buf:
                return buf

    async def command(self, cmd: str) -> str:
        async with self._lock:
            self._writer.write((cmd + "\n").encode())
            await self._writer.drain()
            return await self._drain_until_prompt()

    async def close(self) -> None:
        if self._writer:
            self._writer.close()
            await self._writer.wait_closed()

    @property
    def connected(self) -> bool:
        return self._writer is not None and not self._writer.is_closing()


# ── ELF symbols ──────────────────────────────────────────────────────────────
def load_symbols(elf_path: str) -> dict[str, int]:
    try:
        out = subprocess.check_output(
            ["aarch64-linux-gnu-nm", "--defined-only", "-n", elf_path],
            stderr=subprocess.DEVNULL,
            text=True,
        )
        syms: dict[str, int] = {}
        for line in out.splitlines():
            parts = line.split()
            if len(parts) >= 3:
                try:
                    syms[parts[2]] = int(parts[0], 16)
                except ValueError:
                    pass
        return syms
    except Exception as e:
        print(f"[warn] nm failed: {e}", file=sys.stderr)
        return {}


def pa_for_sym(syms: dict[str, int], name: str) -> int | None:
    va = syms.get(name)
    if va is None:
        return None
    if va >= (KERNEL_VA_OFFSET & 0xFFFFFFFFFFFFFFFF):
        return va - KERNEL_VA_OFFSET
    return va


# ── memory helpers ────────────────────────────────────────────────────────────
async def read_phys_u64(hmp: HmpClient, pa: int) -> int:
    resp = await hmp.command(f"xp /1gx 0x{pa:016x}")
    # output: "0x00000000xxxxxxxx: 0xYYYYYYYYYYYYYYYY\r\n(qemu)"
    m = re.search(r":\s+(0x[0-9a-fA-F]+)", resp)
    if not m:
        raise ValueError(f"Unexpected xp output: {resp!r}")
    return int(m.group(1), 16)


async def read_phys_bytes(hmp: HmpClient, pa: int, count: int) -> list[int]:
    """Read `count` bytes starting at `pa`."""
    resp = await hmp.command(f"xp /{count}bx 0x{pa:016x}")
    # values are hex bytes separated by spaces, possibly across lines
    values = re.findall(r"0x[0-9a-fA-F]{2}", resp)
    return [int(v, 16) for v in values[:count]]


def bytes_to_u64le(bs: list[int], offset: int) -> int:
    val = 0
    for i in range(8):
        val |= bs[offset + i] << (i * 8)
    return val


# ── MMU decode ───────────────────────────────────────────────────────────────
def decode_leaf_attrs(desc: int) -> dict[str, str]:
    attr_idx = (desc >> 2) & 0x7
    return {
        "mem":   "device" if (desc & MMU_ATTR_MASK) == MMU_ATTR_DEVICE else "normal",
        "ap":    "ro" if (desc & MMU_AP_RO) else "rw",
        "exec":  "nx" if (desc & MMU_PXN) else "x",
        "share": "inner" if (desc & MMU_SH_INNER) == MMU_SH_INNER else "outer",
        "af":    "1" if (desc & MMU_AF) else "0",
    }


async def walk_va(hmp: HmpClient, syms: dict[str, int], va_str: str) -> dict[str, Any]:
    """Walk the MMU page tables for a given VA. Returns a dict with walk steps."""
    try:
        va = int(va_str, 0)
    except ValueError:
        return {"error": f"Invalid VA: {va_str}"}

    # choose TTBR
    if va >= (KERNEL_VA_OFFSET & 0xFFFFFFFFFFFFFFFF):
        ttbr_sym = "l0_table_ttbr1"
        root_name = "ttbr1"
    else:
        ttbr_sym = "l0_table"
        root_name = "ttbr0"

    ttbr_sym_pa = pa_for_sym(syms, ttbr_sym)
    if ttbr_sym_pa is None:
        return {"error": "Could not resolve TTBR root symbol. Check ELF path."}
    # l0_table / l0_table_ttbr1 are pointer variables; dereference to get actual table PA
    root_pa = await read_phys_u64(hmp, ttbr_sym_pa)
    if root_pa == 0:
        return {"error": f"TTBR symbol {ttbr_sym} is null (MMU not yet initialised?)"}

    steps: list[dict[str, Any]] = []
    steps.append({"level": "root", "name": root_name, "pa": f"0x{root_pa:016x}"})

    # L0 index = va[47:39]
    l0_idx = (va >> 39) & 0x1FF
    l0_entry_pa = root_pa + l0_idx * 8
    l0_desc = await read_phys_u64(hmp, l0_entry_pa)
    steps.append({"level": "L0", "index": l0_idx, "entry_pa": f"0x{l0_entry_pa:016x}", "desc": f"0x{l0_desc:016x}"})
    if not (l0_desc & MMU_VALID):
        return {"va": f"0x{va:016x}", "root": root_name, "steps": steps, "result": "fault", "reason": "L0 not valid"}
    if (l0_desc & MMU_TYPE_MASK) != (MMU_VALID | 2):  # must be table
        return {"va": f"0x{va:016x}", "root": root_name, "steps": steps, "result": "fault", "reason": "L0 not table"}

    # L1
    l1_table_pa = l0_desc & MMU_ADDR_MASK
    l1_idx = (va >> 30) & 0x1FF
    l1_entry_pa = l1_table_pa + l1_idx * 8
    l1_desc = await read_phys_u64(hmp, l1_entry_pa)
    steps.append({"level": "L1", "index": l1_idx, "entry_pa": f"0x{l1_entry_pa:016x}", "desc": f"0x{l1_desc:016x}"})
    if not (l1_desc & MMU_VALID):
        return {"va": f"0x{va:016x}", "root": root_name, "steps": steps, "result": "fault", "reason": "L1 not valid"}
    if (l1_desc & MMU_TYPE_MASK) == MMU_BLOCK:
        pa = (l1_desc & MMU_L1_BLK_MASK) | (va & 0x3FFFFFFF)
        return {"va": f"0x{va:016x}", "root": root_name, "steps": steps,
                "result": "mapped", "pa": f"0x{pa:016x}", "kind": "L1 block (1 GiB)",
                "attrs": decode_leaf_attrs(l1_desc)}

    # L2
    l2_table_pa = l1_desc & MMU_ADDR_MASK
    l2_idx = (va >> 21) & 0x1FF
    l2_entry_pa = l2_table_pa + l2_idx * 8
    l2_desc = await read_phys_u64(hmp, l2_entry_pa)
    steps.append({"level": "L2", "index": l2_idx, "entry_pa": f"0x{l2_entry_pa:016x}", "desc": f"0x{l2_desc:016x}"})
    if not (l2_desc & MMU_VALID):
        return {"va": f"0x{va:016x}", "root": root_name, "steps": steps, "result": "fault", "reason": "L2 not valid"}
    if (l2_desc & MMU_TYPE_MASK) == MMU_BLOCK:
        pa = (l2_desc & MMU_L2_BLK_MASK) | (va & 0x1FFFFF)
        return {"va": f"0x{va:016x}", "root": root_name, "steps": steps,
                "result": "mapped", "pa": f"0x{pa:016x}", "kind": "L2 block (2 MiB)",
                "attrs": decode_leaf_attrs(l2_desc)}

    # L3
    l3_table_pa = l2_desc & MMU_ADDR_MASK
    l3_idx = (va >> 12) & 0x1FF
    l3_entry_pa = l3_table_pa + l3_idx * 8
    l3_desc = await read_phys_u64(hmp, l3_entry_pa)
    steps.append({"level": "L3", "index": l3_idx, "entry_pa": f"0x{l3_entry_pa:016x}", "desc": f"0x{l3_desc:016x}"})
    if not (l3_desc & MMU_VALID):
        return {"va": f"0x{va:016x}", "root": root_name, "steps": steps, "result": "fault", "reason": "L3 not valid"}
    pa = (l3_desc & MMU_L3_PG_MASK) | (va & 0xFFF)
    page_pa = l3_desc & MMU_L3_PG_MASK
    pfn = (page_pa - RAM_BASE) // PAGE_SIZE
    alloc = "unknown"
    ps_pa = pa_for_sym(syms, "page_state")
    if ps_pa is not None and 0 <= pfn < PAGE_COUNT:
        try:
            state_b = await read_phys_bytes(hmp, ps_pa + pfn, 1)
            alloc = {0: "unmanaged", 1: "free", 2: "allocated"}.get(state_b[0], f"?{state_b[0]}")
        except Exception:
            pass
    return {"va": f"0x{va:016x}", "root": root_name, "steps": steps,
            "result": "mapped", "pa": f"0x{pa:016x}", "kind": "L3 page (4 KiB)",
            "attrs": decode_leaf_attrs(l3_desc), "alloc": alloc}


# ── page allocator decode ─────────────────────────────────────────────────────
# page_state[] values (from src/kernel/page_alloc.c)
PAGE_UNUSED    = 0  # unmanaged (kernel itself, below managed_start)
PAGE_FREE      = 1  # in free list
PAGE_ALLOCATED = 2  # allocated

async def read_page_states(hmp: HmpClient, syms: dict[str, int]) -> list[int]:
    """Read the page state array from the kernel page allocator."""
    # Try common symbol names for the page state array
    for sym in ["page_state", "g_page_states", "page_states", "_page_states",
                "g_pages", "phys_page_states"]:
        pa = pa_for_sym(syms, sym)
        if pa is not None:
            break
    else:
        return []

    byte_count = PAGE_COUNT  # 1 byte per page state
    raw = await read_phys_bytes(hmp, pa, byte_count)
    return raw[:PAGE_COUNT]


async def collect_table_pages(hmp: HmpClient, syms: dict[str, int]) -> list[dict[str, Any]]:
    """Find all MMU table pages by walking the TTBR1 root."""
    results: list[dict[str, Any]] = []

    root_sym_pa = None
    for sym in ["l0_table_ttbr1", "_kernel_ttbr1_root", "mmu_ttbr1_root", "g_ttbr1_root",
                "kernel_l0_table", "ttbr1_root"]:
        root_sym_pa = pa_for_sym(syms, sym)
        if root_sym_pa is not None:
            break
    if root_sym_pa is None:
        return results
    # l0_table_ttbr1 is a pointer variable; dereference to get actual table PA
    root_pa = await read_phys_u64(hmp, root_sym_pa)
    if root_pa == 0:
        return results

    visited: set[int] = set()

    async def scan_table(pa: int, level: int, label: str) -> None:
        if pa in visited or level > 3:
            return
        visited.add(pa)
        results.append({"pa": f"0x{pa:016x}", "level": level, "label": label})

        if level >= 3:
            return

        # read 512 entries
        try:
            resp = await hmp.command(f"xp /512gx 0x{pa:016x}")
        except Exception:
            return
        entries = re.findall(r"0x[0-9a-fA-F]{16}", resp)
        for i, entry_str in enumerate(entries[:512]):
            desc = int(entry_str, 16)
            if (desc & MMU_TYPE_MASK) == (MMU_VALID | 2):  # table
                child_pa = desc & MMU_ADDR_MASK
                if RAM_BASE <= child_pa < RAM_END:
                    child_label = f"L{level+1}[{i}]←{label}"
                    await scan_table(child_pa, level + 1, child_label)

    await scan_table(root_pa, 0, "L0-root")
    return results


# ── FastAPI app ───────────────────────────────────────────────────────────────
app = FastAPI(title="QEMU Page Inspector")
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)


@app.middleware("http")
async def connection_close(request, call_next):
    """Force Connection: close so Chromium treats every response like HTTP/1.0."""
    response = await call_next(request)
    response.headers["Connection"] = "close"
    return response

# global state
_hmp: HmpClient | None = None
_syms: dict[str, int] = {}
_elf_path = "build/kernel8.elf"
_monitor_host = "127.0.0.1"
_monitor_port = 4444


class WalkRequest(BaseModel):
    va: str


async def get_hmp() -> HmpClient:
    global _hmp
    if _hmp is None or not _hmp.connected:
        _hmp = HmpClient(_monitor_host, _monitor_port)
        try:
            await _hmp.connect()
        except Exception as e:
            _hmp = None
            raise HTTPException(status_code=503, detail=f"Cannot connect to QEMU HMP monitor at {_monitor_host}:{_monitor_port} — {e}")
    return _hmp


@app.get("/api/status")
async def api_status() -> dict:
    """Check QEMU HMP connectivity and kernel ELF."""
    try:
        hmp = await get_hmp()
        resp = await hmp.command("info status")
        vm_status = "running" if "running" in resp.lower() else "paused"
    except HTTPException as e:
        return {"qemu": "disconnected", "error": e.detail, "symbols": len(_syms), "elf": _elf_path}
    return {"qemu": "connected", "vm": vm_status, "symbols": len(_syms), "elf": _elf_path}


@app.post("/api/pause")
async def api_pause() -> dict:
    hmp = await get_hmp()
    await hmp.command("stop")
    return {"ok": True}


@app.post("/api/continue")
async def api_continue() -> dict:
    hmp = await get_hmp()
    await hmp.command("cont")
    return {"ok": True}


@app.get("/api/snapshot")
async def api_snapshot() -> dict:
    """Pause QEMU, read full page state + table inventory, resume."""
    hmp = await get_hmp()
    await hmp.command("stop")
    try:
        page_states = await read_page_states(hmp, _syms)
        tables = await collect_table_pages(hmp, _syms)
    finally:
        await hmp.command("cont")

    # summarize
    free     = page_states.count(PAGE_FREE)
    used     = page_states.count(PAGE_ALLOCATED)
    reserved = page_states.count(PAGE_UNUSED)
    table    = 0

    return {
        "total_pages": PAGE_COUNT,
        "free": free,
        "used": used,
        "table": table,
        "reserved": reserved,
        "page_states": page_states,  # list[int], one per page
        "tables": tables,
        "ram_base": f"0x{RAM_BASE:08x}",
        "page_size": PAGE_SIZE,
    }


@app.post("/api/walk")
async def api_walk(req: WalkRequest) -> dict:
    hmp = await get_hmp()
    await hmp.command("stop")
    try:
        result = await walk_va(hmp, _syms, req.va)
    finally:
        await hmp.command("cont")
    return result


@app.get("/api/symbols")
async def api_symbols() -> dict:
    """Return a subset of kernel symbols useful for inspection."""
    interesting = [
        "l0_table_ttbr1", "l0_table",
        "_kernel_ttbr1_root", "mmu_ttbr1_root",
        "page_state", "g_page_states",
        "managed_start", "managed_end", "free_pages",
        "kernel_main", "_start", "kernel_main_early",
    ]
    found = {k: f"0x{v:016x}" for k, v in _syms.items() if k in interesting}
    return {"count": len(_syms), "interesting": found, "symbols": {k: f"0x{v:016x}" for k, v in _syms.items()}}


@app.get("/api/debug/pagestate")
async def api_debug_pagestate() -> dict:
    """Debug: read 32 bytes from page_state array."""
    pa = pa_for_sym(_syms, "page_state")
    if pa is None:
        return {"error": "page_state symbol not found", "syms_count": len(_syms)}
    hmp = await get_hmp()
    try:
        raw = await read_phys_bytes(hmp, pa, 64)
    except Exception as e:
        return {"error": str(e), "pa": hex(pa)}
    return {"pa": hex(pa), "raw64": raw, "page_state_in_syms": ("page_state" in _syms)}


# serve frontend
_THIS_DIR = Path(__file__).parent

@app.get("/", response_class=HTMLResponse)
@app.head("/")
async def frontend() -> HTMLResponse:
    return HTMLResponse(content=(_THIS_DIR / "index.html").read_text(encoding="utf-8"))


# ── entry point ───────────────────────────────────────────────────────────────
def main() -> None:
    global _syms, _elf_path, _monitor_host, _monitor_port

    parser = argparse.ArgumentParser(description="QEMU Page Inspector server")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7777)
    parser.add_argument("--elf", default="build/kernel8.elf")
    parser.add_argument("--monitor-host", default="127.0.0.1")
    parser.add_argument("--monitor-port", type=int, default=4444)
    args = parser.parse_args()

    _elf_path = args.elf
    _monitor_host = args.monitor_host
    _monitor_port = args.monitor_port

    elf = Path(args.elf)
    if elf.exists():
        _syms = load_symbols(str(elf))
        print(f"[info] Loaded {len(_syms)} symbols from {elf}")
    else:
        print(f"[warn] ELF not found: {elf}  (run 'make' first)")

    print(f"[info] Inspector UI → http://{args.host}:{args.port}")
    print(f"[info] QEMU HMP     → {_monitor_host}:{_monitor_port}")
    uvicorn.run(app, host=args.host, port=args.port, log_level="warning",
                http="h11", timeout_keep_alive=5)


if __name__ == "__main__":
    main()
