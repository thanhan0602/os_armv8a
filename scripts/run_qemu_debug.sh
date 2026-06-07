#!/usr/bin/env bash
set -euo pipefail

echo "Starting QEMU"

QEMU_BIN="${QEMU_BIN:-/home/a/qemu/build}"
KERNEL_IMG="${KERNEL_IMG:-build/kernel8.img}"
MONITOR_PORT=4445
GDB_PORT=1234

# If QEMU_BIN is a directory, append the binary name
if [[ -d "$QEMU_BIN" ]]; then
  QEMU_BIN="$QEMU_BIN/qemu-system-aarch64"
fi

if [[ ! -x "$QEMU_BIN" ]]; then
  echo "QEMU binary not found or not executable: $QEMU_BIN" >&2
  exit 1
fi

# Check for port conflicts before starting — a common cause of "connection timeout"
# (usually the qemu-inspector is already running on the same ports)
for PORT in "$MONITOR_PORT" "$GDB_PORT"; do
  if ss -tlnH "sport = :$PORT" 2>/dev/null | grep -q .; then
    echo "" >&2
    echo "ERROR: Port $PORT is already in use." >&2
    if [[ "$PORT" == "$MONITOR_PORT" ]]; then
      echo "  The qemu-inspector may be running. Stop it first:" >&2
      echo "  ./tools/qemu-inspector/start.sh --stop" >&2
      echo "  Then retry F5. Afterwards run:" >&2
      echo "  GDB_PORT=1234 ./tools/qemu-inspector/start.sh --server-only" >&2
    fi
    echo "" >&2
    exit 1
  fi
done

exec "$QEMU_BIN" \
  -machine virt,gic-version=2 \
  -cpu max \
  -nographic \
  -serial mon:stdio \
  -S \
  -gdb tcp::1234 \
  -monitor tcp:127.0.0.1:4445,server,nowait \
  -kernel "$KERNEL_IMG"
