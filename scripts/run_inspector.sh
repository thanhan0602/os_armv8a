#!/usr/bin/env bash
# Launch the QEMU Page Inspector web app.
# Usage: bash scripts/run_inspector.sh [--port 7777]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VENV="$REPO_ROOT/.venv"

if [[ ! -f "$VENV/bin/python3" ]]; then
    echo "[error] .venv not found at $REPO_ROOT/.venv"
    echo "        Run: python3 -m venv .venv && source .venv/bin/activate && pip install fastapi uvicorn"
    exit 1
fi

ELF="${ELF:-$REPO_ROOT/build/kernel8.elf}"
MONITOR_HOST="${MONITOR_HOST:-127.0.0.1}"
MONITOR_PORT="${MONITOR_PORT:-4445}"
HOST="${INSPECTOR_HOST:-127.0.0.1}"
PORT="${INSPECTOR_PORT:-7777}"

echo "[qemu-inspector] ELF:     $ELF"
echo "[qemu-inspector] Monitor: $MONITOR_HOST:$MONITOR_PORT"
echo "[qemu-inspector] UI:      http://$HOST:$PORT"
echo ""
echo "  Make sure QEMU is running with:  -display none -serial file:/tmp/qemu-serial.log -monitor tcp:127.0.0.1:4445,server,nowait"
echo ""

cd "$REPO_ROOT"
exec "$VENV/bin/python3" tools/qemu-inspector/server.py \
    --host "$HOST" \
    --port "$PORT" \
    --elf "$ELF" \
    --monitor-host "$MONITOR_HOST" \
    --monitor-port "$MONITOR_PORT" \
    "$@"
