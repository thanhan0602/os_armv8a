#!/usr/bin/env bash
# tools/qemu-inspector/start.sh
#
# Quick-start script for QEMU + Inspector server.
# The server starts first and serves the Web UI immediately (even while QEMU
# is booting).  It reconnects to QEMU automatically in the background.
#
# Usage:
#   ./tools/qemu-inspector/start.sh              # start both if not running
#   ./tools/qemu-inspector/start.sh --stop       # kill both
#   ./tools/qemu-inspector/start.sh --status     # show PIDs + URLs
#   ./tools/qemu-inspector/start.sh --vscode     # use port 1234 (VSCode GDB compat) + print launch.json
#   ./tools/qemu-inspector/start.sh --server-only  # server only (QEMU via VSCode launch.json)
#
# Environment overrides:
#   QEMU_BIN  — path to qemu-system-aarch64  (default: /home/a/qemu/build/...)
#   KERNEL    — path to kernel image          (default: build/kernel8.img)
#   ELF       — path to kernel ELF            (default: build/kernel8.elf)
#   GDB_PORT  — GDB stub port                 (default: 4446; set to 1234 to match VSCode defaults)

set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
QEMU_BIN="${QEMU_BIN:-/home/a/qemu/build/qemu-system-aarch64}"
KERNEL="${KERNEL:-$REPO/build/kernel8.img}"
ELF="${ELF:-$REPO/build/kernel8.elf}"
VENV="$REPO/.venv"
MONITOR_PORT=4445
GDB_PORT="${GDB_PORT:-4446}"
SERVER_PORT=8888
SERIAL_LOG=/tmp/qemu-serial.log
SERVER_LOG=/tmp/inspector-server.log

# ── --vscode ─────────────────────────────────────────────────────────────────
if [[ "${1:-}" == "--vscode" ]]; then
  GDB_PORT=1234
  shift
  echo "[vscode] GDB port set to 1234"
  echo ""
  echo "  Add to .vscode/launch.json:"
  echo '  {'
  echo '    "name": "Attach QEMU (inspector)",'
  echo '    "type": "cppdbg",'
  echo '    "request": "launch",'
  echo '    "program": "${workspaceFolder}/build/kernel8.elf",'
  echo '    "miDebuggerServerAddress": "127.0.0.1:1234",'
  echo '    "miDebuggerPath": "gdb-multiarch",'
  echo '    "stopAtEntry": false,'
  echo '    "cwd": "${workspaceFolder}",'
  echo '    "externalConsole": false,'
  echo '    "MIMode": "gdb"'
  echo '  }'
  echo ""
  echo "  NOTE: The web inspector Break & Snapshot uses the same port."
  echo "  Disconnect VSCode GDB before using Break & Snapshot."
  echo "  All HMP features (Walk VA, Tasks, Page Owners) work alongside VSCode GDB."
  echo ""
fi

# ── helpers ───────────────────────────────────────────────────────────────────
qemu_running()  { pgrep -f "qemu-system-aarch64" > /dev/null 2>&1; }
server_running() { pgrep -f "server.py.*$SERVER_PORT" > /dev/null 2>&1; }

# ── --stop ────────────────────────────────────────────────────────────────────
if [[ "${1:-}" == "--stop" ]]; then
  if server_running; then
    pkill -f "server.py.*$SERVER_PORT"
    echo "[stop] inspector server stopped"
  else
    echo "[stop] server not running"
  fi
  if qemu_running; then
    pkill -f "qemu-system-aarch64"
    echo "[stop] QEMU stopped"
  else
    echo "[stop] QEMU not running"
  fi
  exit 0
fi

# ── --status ──────────────────────────────────────────────────────────────────
if [[ "${1:-}" == "--status" ]]; then
  echo "=== QEMU Inspector ==="
  if qemu_running; then
    echo "QEMU  : running  (pid $(pgrep -f qemu-system-aarch64 | head -1))"
  else
    echo "QEMU  : stopped"
  fi
  if server_running; then
    echo "Server: running  (pid $(pgrep -f "server.py.*$SERVER_PORT" | head -1))"
  else
    echo "Server: stopped"
  fi
  echo "Web UI: http://127.0.0.1:$SERVER_PORT/"
  echo "Serial: $SERIAL_LOG"
  echo "Log  : $SERVER_LOG"
  exit 0
fi

cd "$REPO"

# ── --server-only (for use with VSCode GDB — QEMU already started by launch.json) ──
if [[ "${1:-}" == "--server-only" ]]; then
  if server_running; then
    echo "[start] Server already running on :$SERVER_PORT"
  else
    if [[ ! -f "$VENV/bin/activate" ]]; then
      echo "[error] venv not found at $VENV" >&2; exit 1
    fi
    source "$VENV/bin/activate"
    python3 "$REPO/tools/qemu-inspector/server.py" \
      --elf "$ELF" \
      --port "$SERVER_PORT" \
      --host 0.0.0.0 \
      --monitor-port "$MONITOR_PORT" \
      --gdb-port "$GDB_PORT" \
      >> "$SERVER_LOG" 2>&1 &
    echo "[start] Inspector server pid=$!  log=$SERVER_LOG"
  fi
  echo ""
  echo "  Web UI → http://127.0.0.1:$SERVER_PORT/"
  echo "  QEMU HMP expected on port $MONITOR_PORT (added by run_qemu_debug.sh)"
  echo "  HMP features: Tasks, Walk VA, Page Owners ⟳, Memory Dump — all available"
  echo "  GDB port $GDB_PORT is used by VSCode — Break & Snapshot unavailable while VSCode GDB is connected"
  echo ""
  exit 0
fi

# ── Inspector server (start first — serves UI while QEMU boots) ───────────────
if server_running; then
  echo "[start] Server already running on :$SERVER_PORT"
else
  if [[ ! -f "$VENV/bin/activate" ]]; then
    echo "[error] venv not found at $VENV  (run: python3 -m venv .venv && pip install -e tools/mcp)" >&2
    exit 1
  fi
  # shellcheck source=/dev/null
  source "$VENV/bin/activate"
  python3 "$REPO/tools/qemu-inspector/server.py" \
    --elf "$ELF" \
    --port "$SERVER_PORT" \
    --host 0.0.0.0 \
    --monitor-port "$MONITOR_PORT" \
    --gdb-port "$GDB_PORT" \
    >> "$SERVER_LOG" 2>&1 &
  echo "[start] Server pid=$!  log=$SERVER_LOG"
fi

# ── QEMU ──────────────────────────────────────────────────────────────────────
if qemu_running; then
  echo "[start] QEMU already running (pid $(pgrep -f qemu-system-aarch64 | head -1))"
else
  if [[ ! -f "$KERNEL" ]]; then
    echo "[warn]  Kernel not found: $KERNEL  (run: make)"
  else
    "$QEMU_BIN" \
      -machine virt,gic-version=2 \
      -cpu max \
      -display none \
      -serial file:"$SERIAL_LOG" \
      -monitor tcp:127.0.0.1:"$MONITOR_PORT",server,nowait \
      -gdb tcp:127.0.0.1:"$GDB_PORT" \
      -S \
      -kernel "$KERNEL" \
      > /dev/null 2>&1 &
    echo "[start] QEMU  pid=$!  serial=$SERIAL_LOG"
  fi
fi

echo ""
echo "  Web UI  → http://127.0.0.1:$SERVER_PORT/"
echo "  The status dot will pulse yellow until QEMU connects, then turn green."
echo "  Run './tools/qemu-inspector/start.sh --stop' to kill everything."
