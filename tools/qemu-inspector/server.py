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
from contextlib import asynccontextmanager
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


async def read_phys_u64_bulk(hmp: HmpClient, pa: int, count: int) -> list[int]:
    """Read count u64 values starting at pa, batching 64 at a time.

    QEMU xp /Ngx puts 2 u64 values per output line, so we must parse all
    values on each line (not just the first after the colon).
    """
    result: list[int] = []
    for off in range(0, count, 64):
        n = min(64, count - off)
        resp = await hmp.command(f"xp /{n}gx 0x{pa + off * 8:016x}")
        for line in resp.split("\n"):
            if ":" not in line:
                continue
            data_part = line.split(":", 1)[1]
            result.extend(int(v, 16) for v in re.findall(r"0x[0-9a-fA-F]+", data_part))
    return result[:count]


async def read_phys_bytes(hmp: HmpClient, pa: int, count: int) -> list[int]:
    """Read `count` bytes starting at `pa`."""
    resp = await hmp.command(f"xp /{count}bx 0x{pa:016x}")
    # Each HMP output line: "0xADDRESS: 0xVAL1 0xVAL2 ..."
    # Parse only the data part AFTER the colon to avoid matching address bytes.
    values: list[int] = []
    for line in resp.split("\n"):
        if ":" not in line:
            continue
        data_part = line.split(":", 1)[1]
        values.extend(int(v, 16) for v in re.findall(r"0x[0-9a-fA-F]{2}", data_part))
    return values[:count]


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


async def walk_va(
    hmp: HmpClient,
    syms: dict[str, int],
    va_str: str,
    root_pa_override: int | None = None,
) -> dict[str, Any]:
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

    steps: list[dict[str, Any]] = []

    if root_pa_override is not None and root_name == "ttbr0":
        root_pa = root_pa_override
        steps.append({"level": "root", "name": root_name,
                      "pa": f"0x{root_pa:016x}", "note": "task root_pa override"})
    else:
        ttbr_sym_pa = pa_for_sym(syms, ttbr_sym)
        if ttbr_sym_pa is None:
            return {"error": "Could not resolve TTBR root symbol. Check ELF path."}
        # l0_table / l0_table_ttbr1 are pointer variables; dereference to get actual table PA
        root_pa = await read_phys_u64(hmp, ttbr_sym_pa)
        if root_pa == 0:
            return {"error": f"TTBR symbol {ttbr_sym} is null (MMU not yet initialised?)"}
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


# ── background HMP reconnect ─────────────────────────────────────────────────
async def _hmp_reconnect_loop() -> None:
    """Keep HMP alive: retry every 3 s so the web UI is ready before QEMU."""
    global _hmp
    while True:
        try:
            if _hmp is None or not _hmp.connected:
                client = HmpClient(_monitor_host, _monitor_port)
                await asyncio.wait_for(client.connect(), timeout=2.0)
                _hmp = client
        except Exception:
            _hmp = None
        await asyncio.sleep(3)


@asynccontextmanager
async def lifespan(_app: FastAPI):
    hmp_task = asyncio.create_task(_hmp_reconnect_loop())
    gdb_task = asyncio.create_task(_gdb_reconnect_loop())
    yield
    hmp_task.cancel()
    gdb_task.cancel()
    for t in (hmp_task, gdb_task):
        try:
            await t
        except asyncio.CancelledError:
            pass


# ── FastAPI app ───────────────────────────────────────────────────────────────
app = FastAPI(title="QEMU Page Inspector", lifespan=lifespan)
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
_gdb_host = "127.0.0.1"
_gdb_port = 4446


# ── GDB RSP client ────────────────────────────────────────────────────────────
class GdbRspClient:
    """Minimal async GDB Remote Serial Protocol (RSP) client.

    Supports the subset needed for breakpoints and memory reads on QEMU:
      - connect / disconnect
      - insert / remove software breakpoints (Z0/z0)
      - read memory (m packets)
      - continue execution (c)
      - interrupt (Ctrl-C, i.e. 0x03 byte)
      - query stop reason (?)
    """
    def __init__(self, host: str, port: int) -> None:
        self._host = host
        self._port = port
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._lock = asyncio.Lock()

    async def connect(self) -> None:
        self._reader, self._writer = await asyncio.open_connection(self._host, self._port)
        # QEMU sends a '+' ACK when it accepts the connection? No — just connect.
        # Enable no-ack mode for simplicity (QEMU >= 2.5 supports QStartNoAckMode)
        resp = await self._send_packet("QStartNoAckMode")
        # If ack mode disabled succeeds, QEMU replies OK; if not, we'll get ""
        # Either way proceed.

    async def close(self) -> None:
        if self._writer:
            self._writer.close()
            try:
                await self._writer.wait_closed()
            except Exception:
                pass

    @property
    def connected(self) -> bool:
        return self._writer is not None and not self._writer.is_closing()

    @staticmethod
    def _checksum(data: str) -> str:
        return f"{sum(ord(c) for c in data) & 0xFF:02x}"

    @staticmethod
    def _encode(data: str) -> bytes:
        cs = GdbRspClient._checksum(data)
        return f"${data}#{cs}".encode()

    async def _send_raw(self, raw: bytes) -> None:
        self._writer.write(raw)
        await self._writer.drain()

    async def _recv_packet(self, read_timeout: float = 5.0) -> str:
        """Read until we see $..#xx and return the payload."""
        buf = b""
        while True:
            chunk = await asyncio.wait_for(self._reader.read(4096), timeout=read_timeout)
            if not chunk:
                raise ConnectionError("GDB RSP: connection closed")
            buf += chunk
            # Look for $payload#xx pattern
            start = buf.find(b"$")
            if start == -1:
                continue
            end = buf.find(b"#", start + 1)
            if end == -1 or len(buf) < end + 3:
                continue
            payload = buf[start + 1:end].decode("latin-1")
            print(f"[gdb <<] {payload[:100]}", flush=True)
            # Send '+' ACK (may be ignored in no-ack mode)
            self._writer.write(b"+")
            await self._writer.drain()
            return payload

    async def wait_for_stop(self, timeout: float = 60.0) -> str:
        """Wait for a stop-reply packet (T05, T02, …) without sending any command.
        Used when execution is controlled by HMP and we just observe the GDB stop.
        """
        async with self._lock:
            return await self._recv_packet(read_timeout=timeout)

    async def _send_packet(self, data: str) -> str:
        async with self._lock:
            # Encode and send
            packet = self._encode(data)
            print(f"[gdb >>] {data[:100]}", flush=True)
            self._writer.write(packet)
            await self._writer.drain()
            # Read response packet (no-ack mode: QEMU won't send '+')
            return await self._recv_packet()

    async def interrupt(self) -> str:
        """Send Ctrl-C (0x03) to pause the target."""
        async with self._lock:
            self._writer.write(b"\x03")
            await self._writer.drain()
            return await self._recv_packet()

    async def flush_and_sync(self) -> None:
        """Send Ctrl-C to stop any in-flight execution, then drain all pending
        packets from the TCP receive buffer so the connection is in a known
        clean 'stopped' state ready for the next command.

        This avoids the force-reconnect pattern, which causes QEMU to
        auto-resume the VM (gdbstub calls vm_start() on client disconnect).
        """
        async with self._lock:
            self._writer.write(b"\x03")
            await self._writer.drain()
            # Drain everything in the receive buffer
            while True:
                try:
                    data = await asyncio.wait_for(
                        self._reader.read(4096), timeout=0.3
                    )
                    if not data:
                        break
                except asyncio.TimeoutError:
                    break

    async def query_stop(self) -> str:
        return await self._send_packet("?")

    async def read_mem(self, addr: int, count: int) -> bytes:
        """Read `count` bytes from target address `addr`."""
        resp = await self._send_packet(f"m{addr:x},{count:x}")
        if resp.startswith("E"):
            raise ValueError(f"GDB mem read error: {resp} at 0x{addr:x}")
        return bytes.fromhex(resp)

    async def write_mem(self, addr: int, data: bytes) -> None:
        hex_data = data.hex()
        resp = await self._send_packet(f"M{addr:x},{len(data):x}:{hex_data}")
        if resp != "OK":
            raise ValueError(f"GDB mem write error: {resp}")

    async def insert_breakpoint(self, addr: int, kind: int = 4, hw: bool = False) -> None:
        """Insert a breakpoint at `addr`.
        hw=False: software breakpoint (Z0) — patches memory.
        hw=True:  hardware breakpoint (Z1) — uses CPU debug registers, no memory patch.
        """
        btype = 1 if hw else 0
        resp = await self._send_packet(f"Z{btype},{addr:x},{kind}")
        if resp not in ("OK", ""):
            raise ValueError(f"GDB Z{btype} error: {resp} at 0x{addr:x}")

    async def remove_breakpoint(self, addr: int, kind: int = 4, hw: bool = False) -> None:
        btype = 1 if hw else 0
        resp = await self._send_packet(f"z{btype},{addr:x},{kind}")
        if resp not in ("OK", ""):
            raise ValueError(f"GDB z{btype} error: {resp} at 0x{addr:x}")

    async def cont(self, timeout: float = 30.0) -> str:
        """Continue execution via GDB 'c'; returns stop-reply packet.

        Unlike _send_packet, 'c' does not get an immediate ACK — the
        stop-reply (T05, S05, etc.) arrives only when the target halts.
        `timeout` controls how long to wait for that stop-reply.
        """
        async with self._lock:
            print("[gdb >>] c", flush=True)
            self._writer.write(self._encode("c"))
            await self._writer.drain()
            return await self._recv_packet(read_timeout=timeout)

    async def read_reg(self, reg_num: int) -> int:
        """Read a single register by GDB register number."""
        resp = await self._send_packet(f"p{reg_num:x}")
        if resp.startswith("E"):
            raise ValueError(f"GDB read reg error: {resp}")
        raw = bytes.fromhex(resp)
        return int.from_bytes(raw, "little")


_gdb: GdbRspClient | None = None


async def _gdb_reconnect_loop() -> None:
    global _gdb
    while True:
        try:
            if _gdb is None or not _gdb.connected:
                client = GdbRspClient(_gdb_host, _gdb_port)
                await asyncio.wait_for(client.connect(), timeout=2.0)
                _gdb = client
        except Exception:
            _gdb = None
        await asyncio.sleep(3)


async def get_gdb() -> GdbRspClient:
    global _gdb
    if _gdb is None or not _gdb.connected:
        raise HTTPException(status_code=503, detail="GDB not connected — is QEMU running with -gdb?")
    return _gdb


# ── struct task layout (CONFIG_KERNEL_VIRTUAL, 64-bit host, VMA=LMA=PA) ─────
_TASK_CONTEXT_SIZE  = 13 * 8   # x19..x30 + sp
_TASK_ID_OFF        = _TASK_CONTEXT_SIZE        # 104
_TASK_STATE_OFF     = _TASK_CONTEXT_SIZE + 8    # 112
_TASK_NAME_OFF      = _TASK_CONTEXT_SIZE + 32   # 136 (kernel VA pointer)
_TASK_MM_OFF        = _TASK_CONTEXT_SIZE + 40   # 144 (kernel VA pointer)
_TASK_SIZE          = _TASK_CONTEXT_SIZE + 56   # 160
_MAX_TASKS          = 16
_MM_ROOTPA_OFF      = 0   # mm_context.root_pa is the first field (raw PA)
_TASK_STATE_NAMES   = {0: "running", 1: "ready", 2: "dead"}


async def _enumerate_pt_pages(hmp: HmpClient, root_pa: int, max_pages: int = 128) -> list[dict]:
    """Walk a user-space page table from root_pa, returning all mapped pages."""
    pages: list[dict] = []

    async def walk(table_pa: int, level: int, va_prefix: int) -> None:
        if len(pages) >= max_pages:
            return
        entries = await read_phys_u64_bulk(hmp, table_pa, 512)
        shift = 39 - level * 9  # L0:39 L1:30 L2:21 L3:12
        for i, desc in enumerate(entries):
            if not (desc & 1):
                continue
            va_base = va_prefix | (i << shift)
            bits01 = desc & 3
            if bits01 == 3 and level < 3:  # table
                await walk(desc & MMU_ADDR_MASK, level + 1, va_base)
            elif bits01 == 3 and level == 3:  # 4KiB page
                UXN = (desc >> 54) & 1
                AP  = (desc >>  6) & 3
                pages.append({"va": hex(va_base), "pa": hex(desc & MMU_L3_PG_MASK),
                               "ap": "ro" if AP >= 2 else "rw", "exec": "nx" if UXN else "x"})
            elif bits01 == 1 and 1 <= level <= 2:  # block
                bshift = 30 if level == 1 else 21
                UXN = (desc >> 54) & 1
                AP  = (desc >>  6) & 3
                pages.append({"va": hex(va_base),
                               "pa": hex(desc & (~((1 << bshift) - 1) & 0x0000FFFFFFFFFFFF)),
                               "ap": "ro" if AP >= 2 else "rw", "exec": "nx" if UXN else "x",
                               "size": hex(1 << bshift), "kind": "block"})

    await walk(root_pa, 0, 0)
    return pages


def _kva_to_pa(ptr: int) -> int:
    """Convert a kernel VA (with KERNEL_VA_OFFSET) back to a physical address."""
    if ptr >= (KERNEL_VA_OFFSET & 0xFFFFFFFFFFFFFFFF):
        return ptr - KERNEL_VA_OFFSET
    return ptr


class WalkRequest(BaseModel):
    va: str
    root_pa: str | None = None  # optional TTBR0 root PA override (hex string)


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
    if _hmp and _hmp.connected:
        try:
            resp = await _hmp.command("info status")
            rl = resp.lower()
            if "debug" in rl:
                vm_status = "halted (gdb)"
            elif "running" in rl:
                vm_status = "running"
            else:
                vm_status = "paused"
            return {"qemu": "connected", "vm": vm_status, "symbols": len(_syms), "elf": _elf_path}
        except Exception:
            pass
    return {"qemu": "disconnected", "symbols": len(_syms), "elf": _elf_path}


@app.post("/api/reconnect")
async def api_reconnect() -> dict:
    """Force-drop and immediately re-establish the HMP connection."""
    global _hmp
    if _hmp:
        await _hmp.close()
        _hmp = None
    try:
        client = HmpClient(_monitor_host, _monitor_port)
        await asyncio.wait_for(client.connect(), timeout=2.0)
        _hmp = client
        return {"ok": True, "qemu": "connected"}
    except Exception as e:
        return {"ok": False, "error": str(e)}


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
    status_str = (await hmp.command("info status")).lower()
    # Don't resume if the VM was halted at a GDB breakpoint (RUN_STATE_DEBUG
    # is reported as "running (debug)" by HMP — calling cont would bypass GDB).
    was_freely_running = ("running" in status_str and "debug" not in status_str)
    await hmp.command("stop")
    try:
        page_states = await read_page_states(hmp, _syms)
        tables = await collect_table_pages(hmp, _syms)
    finally:
        if was_freely_running:
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


@app.get("/api/tasks")
async def api_tasks() -> dict:
    """Read the kernel tasks[] array; returns per-task info with TTBR0 root PA."""
    hmp = await get_hmp()
    tasks_sym_pa = pa_for_sym(_syms, "tasks")
    if tasks_sym_pa is None:
        return {"error": "tasks symbol not found"}
    # Preserve the current VM run/pause state so we don't inadvertently resume
    # a VM that was paused at startup with -S (which would let user tasks exit
    # before the user has a chance to set a GDB breakpoint).
    #
    # IMPORTANT: QEMU reports RUN_STATE_DEBUG (GDB breakpoint halt) as
    # "running (debug)".  Do NOT treat this as "freely running" — calling
    # HMP cont on a debug-halted VM would bypass the GDB stub's state
    # machine and let the VM run without delivering a proper T05.
    status_str = (await hmp.command("info status")).lower()
    was_freely_running = ("running" in status_str and "debug" not in status_str)
    await hmp.command("stop")
    try:
        result = []
        for i in range(_MAX_TASKS):
            task_pa   = tasks_sym_pa + i * _TASK_SIZE
            task_id   = await read_phys_u64(hmp, task_pa + _TASK_ID_OFF)
            task_state = await read_phys_u64(hmp, task_pa + _TASK_STATE_OFF)
            name_ptr  = await read_phys_u64(hmp, task_pa + _TASK_NAME_OFF)
            mm_ptr    = await read_phys_u64(hmp, task_pa + _TASK_MM_OFF)
            # slot is unused if id==0 and name_ptr==0 (except task[0] = idle)
            if task_id == 0 and name_ptr == 0 and i > 0:
                continue
            name = "(null)"
            if name_ptr:
                try:
                    name_bytes = await read_phys_bytes(hmp, _kva_to_pa(name_ptr), 32)
                    end = name_bytes.index(0) if 0 in name_bytes else len(name_bytes)
                    name = bytes(name_bytes[:end]).decode("ascii", errors="replace")
                except Exception:
                    name = f"?@{hex(name_ptr)}"
            root_pa_val = None
            if mm_ptr:
                mm_pa = _kva_to_pa(mm_ptr)
                root_pa_val = hex(await read_phys_u64(hmp, mm_pa + _MM_ROOTPA_OFF))
            result.append({
                "idx": i,
                "id": task_id,
                "name": name,
                "state": _TASK_STATE_NAMES.get(task_state, f"?{task_state}"),
                "mm": hex(mm_ptr) if mm_ptr else None,
                "ttbr0_root_pa": root_pa_val,
            })
    finally:
        if was_freely_running:
            await hmp.command("cont")
    return {"tasks": result}


@app.get("/api/page_owners")
async def api_page_owners() -> dict:
    """Enumerate physical pages owned by all live user tasks (tasks with mm != null).
    Halts QEMU via HMP; do NOT call while GDB has the target stopped.
    """
    hmp = await get_hmp()
    tasks_sym_pa = pa_for_sym(_syms, "tasks")
    if tasks_sym_pa is None:
        return {"error": "tasks symbol not found", "owners": []}
    status_str = (await hmp.command("info status")).lower()
    was_freely_running = ("running" in status_str and "debug" not in status_str)
    await hmp.command("stop")
    try:
        owners: list[dict] = []
        for i in range(_MAX_TASKS):
            task_pa  = tasks_sym_pa + i * _TASK_SIZE
            task_id  = await read_phys_u64(hmp, task_pa + _TASK_ID_OFF)
            name_ptr = await read_phys_u64(hmp, task_pa + _TASK_NAME_OFF)
            mm_ptr   = await read_phys_u64(hmp, task_pa + _TASK_MM_OFF)
            if task_id == 0 and name_ptr == 0 and i > 0:
                continue
            if not mm_ptr:
                continue
            name = f"task[{i}]"
            try:
                nb = await read_phys_bytes(hmp, _kva_to_pa(name_ptr), 32)
                end = nb.index(0) if 0 in nb else len(nb)
                name = bytes(nb[:end]).decode("ascii", errors="replace")
            except Exception:
                pass
            mm_pa   = _kva_to_pa(mm_ptr)
            root_pa = await read_phys_u64(hmp, mm_pa + _MM_ROOTPA_OFF)
            if not root_pa:
                continue
            try:
                pages = await _enumerate_pt_pages(hmp, root_pa)
            except Exception:
                pages = []
            for p in pages:
                p["task_idx"] = i
                p["task_name"] = name
                owners.append(p)
    finally:
        if was_freely_running:
            await hmp.command("cont")
    owners.sort(key=lambda x: int(x["pa"], 16))
    return {"owners": owners}


class WalkTaskRequest(BaseModel):
    va: str
    task_idx: int  # index into tasks[] array


@app.post("/api/walk_task")
async def api_walk_task(req: WalkTaskRequest) -> dict:
    """Walk a VA using the TTBR0 root of a specific task (atomic stop/cont)."""
    hmp = await get_hmp()
    tasks_sym_pa = pa_for_sym(_syms, "tasks")
    if tasks_sym_pa is None:
        return {"error": "tasks symbol not found"}
    status_str = (await hmp.command("info status")).lower()
    was_freely_running = ("running" in status_str and "debug" not in status_str)
    await hmp.command("stop")
    try:
        task_pa = tasks_sym_pa + req.task_idx * _TASK_SIZE
        mm_ptr = await read_phys_u64(hmp, task_pa + _TASK_MM_OFF)
        if not mm_ptr:
            return {"error": f"task[{req.task_idx}].mm is null — task has no user space"}
        mm_pa = _kva_to_pa(mm_ptr)
        root_pa = await read_phys_u64(hmp, mm_pa + _MM_ROOTPA_OFF)
        if not root_pa:
            return {"error": f"task[{req.task_idx}] mm_context.root_pa is 0"}
        result = await walk_va(hmp, _syms, req.va, root_pa_override=root_pa)
    finally:
        if was_freely_running:
            await hmp.command("cont")
    return result


@app.post("/api/walk")
async def api_walk(req: WalkRequest) -> dict:
    hmp = await get_hmp()
    root_pa_override = int(req.root_pa, 0) if req.root_pa else None
    status_str = (await hmp.command("info status")).lower()
    was_freely_running = ("running" in status_str and "debug" not in status_str)
    await hmp.command("stop")
    try:
        result = await walk_va(hmp, _syms, req.va, root_pa_override=root_pa_override)
    finally:
        if was_freely_running:
            await hmp.command("cont")
    return result


# ── GDB endpoints ─────────────────────────────────────────────────────────────

@app.get("/api/gdb/status")
async def api_gdb_status() -> dict:
    """Return GDB RSP connection status."""
    if _gdb and _gdb.connected:
        return {"gdb": "connected", "host": _gdb_host, "port": _gdb_port}
    return {"gdb": "disconnected", "host": _gdb_host, "port": _gdb_port}


class GdbBreakRequest(BaseModel):
    addr: str              # hex VA or symbol name, e.g. "0xffff000040082a40" or "sched_reap_dead"
    kind: int = 4          # instruction length: 4 for A64


@app.post("/api/gdb/break")
async def api_gdb_break(req: GdbBreakRequest) -> dict:
    """Insert a software breakpoint at addr (VA, must be in kernel text)."""
    gdb = await get_gdb()
    # Resolve symbol name to address if needed
    if req.addr.startswith("0x") or req.addr.isdigit():
        addr = int(req.addr, 0)
    else:
        sym_val = _syms.get(req.addr)
        if sym_val is None:
            raise HTTPException(status_code=404, detail=f"Symbol '{req.addr}' not found")
        # Symbols are PAs; kernel text runs at PA+KERNEL_VA_OFFSET after MMU
        addr = sym_val + KERNEL_VA_OFFSET
    await gdb.insert_breakpoint(addr, req.kind)
    return {"ok": True, "addr": hex(addr)}


@app.delete("/api/gdb/break")
async def api_gdb_del_break(addr: str, kind: int = 4) -> dict:
    """Remove a software breakpoint."""
    gdb = await get_gdb()
    a = int(addr, 0)
    await gdb.remove_breakpoint(a, kind)
    return {"ok": True, "addr": hex(a)}


@app.post("/api/gdb/continue")
async def api_gdb_continue(timeout: float = 10.0) -> dict:
    """Continue execution and wait up to `timeout` seconds for a breakpoint hit."""
    gdb = await get_gdb()
    try:
        stop_reply = await asyncio.wait_for(gdb.cont(), timeout=timeout)
    except asyncio.TimeoutError:
        # Interrupt the target so we can continue to use GDB
        try:
            stop_reply = await asyncio.wait_for(gdb.interrupt(), timeout=3.0)
        except Exception:
            stop_reply = "timeout"
        return {"stopped": False, "reason": "timeout", "raw": stop_reply}
    return {"stopped": True, "raw": stop_reply}


@app.post("/api/gdb/interrupt")
async def api_gdb_interrupt() -> dict:
    """Send CTRL-C to pause the target."""
    gdb = await get_gdb()
    stop_reply = await gdb.interrupt()
    return {"stopped": True, "raw": stop_reply}


# AArch64 GDB register numbers (AArch64 RSP numbering):
# 0-30: x0-x30, 31: sp, 32: pc, 33: cpsr
_AARCH64_REGS = {
    **{f"x{i}": i for i in range(31)},
    "sp": 31,
    "pc": 32,
    "cpsr": 33,
}
_COMMON_REGS = ["pc", "sp", "x0", "x1", "x2", "x29", "x30"]


@app.get("/api/gdb/registers")
async def api_gdb_registers() -> dict:
    """Read key AArch64 registers via GDB RSP (target must be stopped/halted)."""
    gdb = await get_gdb()
    result: dict[str, str] = {}
    for name in _COMMON_REGS:
        reg_num = _AARCH64_REGS[name]
        try:
            val = await gdb.read_reg(reg_num)
            result[name] = hex(val)
        except Exception as e:
            result[name] = f"error: {e}"
    return {"registers": result}


@app.get("/api/hmp/snapshot")
async def api_hmp_snapshot() -> dict:
    """Read tasks + page owners using only HMP (no GDB interaction).

    Intended for use when VS Code GDB (or any external GDB client) has already
    halted the VM at a breakpoint.  The VM must be in a stopped state
    (paused or halted-by-GDB) before calling this — it will NOT call
    ``cont`` under any circumstances, so it is safe to call while a GDB
    session is in progress.

    Returns the same structure as the tasks + page_owners fields of
    ``/api/gdb/break_and_snapshot``.
    """
    hmp = await get_hmp()
    status_str = (await hmp.command("info status")).lower()
    vm_state: str
    if "debug" in status_str:
        vm_state = "halted (gdb)"
    elif "running" in status_str:
        vm_state = "running"
    else:
        vm_state = "paused"

    # Explicitly stop so HMP reads are consistent.
    # We will NOT resume regardless of original state — this endpoint is
    # designed for external-GDB workflows where resuming is the caller's job.
    await hmp.command("stop")

    tasks_sym_pa = pa_for_sym(_syms, "tasks")
    if tasks_sym_pa is None:
        return {"error": "tasks symbol not found", "vm": vm_state}

    try:
        result: list[dict] = []
        for i in range(_MAX_TASKS):
            task_pa   = tasks_sym_pa + i * _TASK_SIZE
            task_id   = await read_phys_u64(hmp, task_pa + _TASK_ID_OFF)
            task_state = await read_phys_u64(hmp, task_pa + _TASK_STATE_OFF)
            name_ptr  = await read_phys_u64(hmp, task_pa + _TASK_NAME_OFF)
            mm_ptr    = await read_phys_u64(hmp, task_pa + _TASK_MM_OFF)
            if task_id == 0 and name_ptr == 0 and i > 0:
                continue
            task_name = "(null)"
            if name_ptr:
                try:
                    name_bytes = await read_phys_bytes(hmp, _kva_to_pa(name_ptr), 32)
                    end = name_bytes.index(0) if 0 in name_bytes else len(name_bytes)
                    task_name = bytes(name_bytes[:end]).decode("ascii", errors="replace")
                except Exception:
                    task_name = f"?@{hex(name_ptr)}"
            ttbr0_root: str | None = None
            if mm_ptr:
                mm_pa = _kva_to_pa(mm_ptr)
                ttbr0_root = hex(await read_phys_u64(hmp, mm_pa + _MM_ROOTPA_OFF))
            result.append({
                "idx": i,
                "id": task_id,
                "name": task_name,
                "state": _TASK_STATE_NAMES.get(task_state, f"?{task_state}"),
                "mm": hex(mm_ptr) if mm_ptr else None,
                "ttbr0_root_pa": ttbr0_root,
            })

        page_owners: list[dict] = []
        for task in result:
            mm_ptr = task.get("mm")
            if not mm_ptr:
                continue
            mm_pa = _kva_to_pa(int(mm_ptr, 16))
            root_pa = await read_phys_u64(hmp, mm_pa + _MM_ROOTPA_OFF)
            if not root_pa:
                continue
            try:
                pages = await _enumerate_pt_pages(hmp, root_pa)
            except Exception:
                pages = []
            for page in pages:
                page["task_idx"] = task["idx"]
                page["task_name"] = task["name"]
                page_owners.append(page)
        page_owners.sort(key=lambda x: int(x["pa"], 16))
    except Exception as exc:
        return {"error": str(exc), "vm": vm_state, "tasks": [], "page_owners": []}

    return {
        "vm": vm_state,
        "tasks": result,
        "page_owners": page_owners,
    }


class GdbReadMemRequest(BaseModel):
    addr: str    # hex VA
    count: int = 8


@app.post("/api/gdb/read_mem")
async def api_gdb_read_mem(req: GdbReadMemRequest) -> dict:
    """Read memory via GDB RSP (target must be stopped)."""
    gdb = await get_gdb()
    addr = int(req.addr, 0)
    count = min(req.count, 256)
    data = await gdb.read_mem(addr, count)
    u64s = []
    for i in range(0, len(data), 8):
        chunk = data[i:i+8]
        if len(chunk) == 8:
            u64s.append({"off": i, "val": hex(int.from_bytes(chunk, "little"))})
    return {"addr": hex(addr), "bytes": list(data), "u64s": u64s}


@app.post("/api/gdb/break_and_snapshot")
async def api_gdb_break_and_snapshot(addr: str, timeout: float = 15.0) -> dict:
    """Set a breakpoint, wait for it to hit, then snapshot tasks + walk.

    This is the main "freeze at function X and inspect" endpoint.
    `addr` may be a hex VA or a symbol name.
    A fresh GDB connection is established each call to clear stale TCP state.

    Key design: breakpoints are always set at the PHYSICAL address (PA) so the
    Z0 patch works even before the kernel enables the MMU.  The kernel maps the
    same PA to a high VA, so the BRK instruction fires when the kernel executes
    that function.  After setting Z0 the VM is started via HMP; we then wait
    passively for the T05 stop reply without sending GDB 'c'.
    """
    global _gdb
    hmp = await get_hmp()

    # Resolve address → report VA (bp_addr) + Z0 PA (bp_addr_pa)
    if addr.startswith("0x") or addr.lstrip("-").isdigit():
        bp_addr = int(addr, 0)
        if bp_addr >= (KERNEL_VA_OFFSET & 0xFFFFFFFFFFFFFFFF):
            # Caller supplied the full kernel VA
            bp_addr_pa = bp_addr - KERNEL_VA_OFFSET
        else:
            # Caller supplied a raw PA — convert to kernel VA for Z0
            bp_addr_pa = bp_addr
            bp_addr    = bp_addr_pa + KERNEL_VA_OFFSET
    else:
        sym_val = _syms.get(addr)
        if sym_val is None:
            raise HTTPException(status_code=404, detail=f"Symbol '{addr}' not found")
        bp_addr_pa = sym_val                     # physical address from ELF
        bp_addr    = sym_val + KERNEL_VA_OFFSET  # kernel VA (PC value when running)

    # Use the existing GDB connection — do NOT close and reopen it.
    # QEMU's gdbstub calls vm_start() when the GDB client disconnects, which
    # resumes the VM and lets user tasks run (and call task_exit) before the
    # new connection can set Z0.  Instead we send Ctrl-C to cancel any
    # in-flight 'c' and drain stale bytes so the connection is clean.
    if _gdb is None or not _gdb.connected:
        try:
            fresh = GdbRspClient(_gdb_host, _gdb_port)
            await asyncio.wait_for(fresh.connect(), timeout=3.0)
            _gdb = fresh
        except Exception as exc:
            raise HTTPException(
                status_code=503,
                detail=f"Cannot connect to GDB stub at {_gdb_host}:{_gdb_port} — {exc}",
            )
    gdb = _gdb

    # Cancel any in-flight execution and drain stale stop-reply bytes so the
    # GDB stub is in a known idle/stopped state before we set Z0.
    try:
        await asyncio.wait_for(gdb.flush_and_sync(), timeout=5.0)
    except Exception:
        pass

    # 1. Insert software breakpoint (Z0) at the kernel VA.  QEMU's gdbstub
    #    registers breakpoints by PC value (virtual address), so we must use
    #    the VA that the CPU will have in its PC register when the kernel calls
    #    this function (after the MMU is active with TTBR1_EL1).
    await gdb.insert_breakpoint(bp_addr, 4, hw=False)

    # 2. Resume via GDB 'c' — this tells QEMU's GDB stub that the CPU is
    #    running and that a T05 stop-reply should be sent when the breakpoint
    #    fires.  Using HMP 'cont' bypasses the stub state machine so T05 is
    #    never delivered over the GDB socket.
    try:
        stop_reply = await gdb.cont(timeout=timeout)
    except asyncio.TimeoutError:
        try:
            await hmp.command("stop")
        except Exception:
            pass
        try:
            await gdb.remove_breakpoint(bp_addr, 4, hw=False)
        except Exception:
            pass
        return {"error": "timeout waiting for breakpoint hit"}

    # 3. Remove breakpoint immediately so it doesn't fire again
    try:
        await gdb.remove_breakpoint(bp_addr, 4, hw=False)
    except Exception:
        pass

    # 4. Use HMP to read tasks (target is now halted; HMP works even when GDB halts)
    tasks_sym_pa = pa_for_sym(_syms, "tasks")
    if tasks_sym_pa is None:
        return {"error": "tasks symbol not found", "stop_reply": stop_reply}

    # HMP also needs to be stopped; when GDB halts the CPU QEMU pauses automatically
    result_tasks = []
    try:
        for i in range(_MAX_TASKS):
            task_pa = tasks_sym_pa + i * _TASK_SIZE
            task_id = await read_phys_u64(hmp, task_pa + _TASK_ID_OFF)
            task_state = await read_phys_u64(hmp, task_pa + _TASK_STATE_OFF)
            name_ptr = await read_phys_u64(hmp, task_pa + _TASK_NAME_OFF)
            mm_ptr = await read_phys_u64(hmp, task_pa + _TASK_MM_OFF)
            if task_id == 0 and name_ptr == 0 and i > 0:
                continue
            name = "(null)"
            if name_ptr:
                try:
                    nb = await read_phys_bytes(hmp, _kva_to_pa(name_ptr), 32)
                    end = nb.index(0) if 0 in nb else len(nb)
                    name = bytes(nb[:end]).decode("ascii", errors="replace")
                except Exception:
                    name = f"?@{hex(name_ptr)}"
            root_pa_val = None
            if mm_ptr:
                mm_pa = _kva_to_pa(mm_ptr)
                root_pa_val = hex(await read_phys_u64(hmp, mm_pa + _MM_ROOTPA_OFF))
            result_tasks.append({
                "idx": i,
                "id": task_id,
                "name": name,
                "state": _TASK_STATE_NAMES.get(task_state, f"?{task_state}"),
                "mm": hex(mm_ptr) if mm_ptr else None,
                "ttbr0_root_pa": root_pa_val,
            })
    except Exception as e:
        return {"error": str(e), "stop_reply": stop_reply}

    # 5. Enumerate user-space pages while still halted
    page_owners: list[dict] = []
    for t in result_tasks:
        if t.get("ttbr0_root_pa"):
            try:
                root = int(t["ttbr0_root_pa"], 16)
                pages = await _enumerate_pt_pages(hmp, root)
                for p in pages:
                    p["task_idx"] = t["idx"]
                    p["task_name"] = t["name"]
                    page_owners.append(p)
            except Exception:
                pass
    page_owners.sort(key=lambda x: int(x["pa"], 16))

    return {
        "stopped_at": hex(bp_addr),
        "stop_reply": stop_reply,
        "tasks": result_tasks,
        "page_owners": page_owners,
    }


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


@app.get("/api/debug/rawmem")
async def api_debug_rawmem(pa: str, count: int = 64) -> dict:
    """Debug: read raw bytes at a given physical address.
    pa: hex address string (e.g. 0x40089148), count: number of bytes (max 256).
    Returns bytes as list[int] and u64 words.
    """
    count = min(count, 256)
    pa_int = int(pa, 0)
    hmp = await get_hmp()
    status_str = (await hmp.command("info status")).lower()
    was_freely_running = ("running" in status_str and "debug" not in status_str)
    await hmp.command("stop")
    try:
        raw = await read_phys_bytes(hmp, pa_int, count)
    finally:
        if was_freely_running:
            await hmp.command("cont")
    words = []
    for i in range(0, len(raw), 8):
        chunk = raw[i:i+8]
        if len(chunk) == 8:
            val = int.from_bytes(chunk, "little")
            words.append({"off": i, "val": hex(val)})
    return {"pa": hex(pa_int), "bytes": raw, "u64s": words}


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
    parser.add_argument("--gdb-host", default="127.0.0.1")
    parser.add_argument("--gdb-port", type=int, default=4446)
    args = parser.parse_args()

    _elf_path = args.elf
    _monitor_host = args.monitor_host
    _monitor_port = args.monitor_port
    _gdb_host = args.gdb_host
    _gdb_port = args.gdb_port

    elf = Path(args.elf)
    if elf.exists():
        _syms = load_symbols(str(elf))
        print(f"[info] Loaded {len(_syms)} symbols from {elf}")
    else:
        print(f"[warn] ELF not found: {elf}  (run 'make' first)")

    print(f"[info] Inspector UI → http://{args.host}:{args.port}")
    print(f"[info] QEMU HMP     → {_monitor_host}:{_monitor_port}")
    print(f"[info] GDB RSP      → {_gdb_host}:{_gdb_port}")
    uvicorn.run(app, host=args.host, port=args.port, log_level="warning",
                http="h11", timeout_keep_alive=5)


if __name__ == "__main__":
    main()
