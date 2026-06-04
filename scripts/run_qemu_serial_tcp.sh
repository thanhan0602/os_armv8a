#!/usr/bin/env bash
set -euo pipefail

echo "Starting QEMU (serial TCP)"

QEMU_BIN="${QEMU_BIN:-/home/a/qemu/build/qemu-system-aarch64}"
KERNEL_IMG="${KERNEL_IMG:-build/kernel8.img}"
SERIAL_HOST="${SERIAL_HOST:-127.0.0.1}"
SERIAL_PORT="${SERIAL_PORT:-5555}"

if [[ ! -x "$QEMU_BIN" ]]; then
  echo "QEMU binary not found or not executable: $QEMU_BIN" >&2
  exit 1
fi

if ss -tlnH "sport = :$SERIAL_PORT" 2>/dev/null | grep -q .; then
  echo "ERROR: Serial TCP port $SERIAL_PORT is already in use." >&2
  echo "Use a different port, for example:" >&2
  echo "  SERIAL_PORT=5560 make run-serial-tcp" >&2
  exit 1
fi

exec "$QEMU_BIN" \
  -machine virt,gic-version=2 \
  -cpu max \
  -display none \
  -monitor none \
  -serial "tcp:${SERIAL_HOST}:${SERIAL_PORT},server,nowait" \
  -kernel "$KERNEL_IMG"