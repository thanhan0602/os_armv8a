#!/usr/bin/env bash
set -euo pipefail

echo "Starting QEMU"

QEMU_BIN="${QEMU_BIN:-/home/a/qemu/build/qemu-system-aarch64}"
KERNEL_IMG="${KERNEL_IMG:-build/kernel8.img}"

if [[ ! -x "$QEMU_BIN" ]]; then
  echo "QEMU binary not found or not executable: $QEMU_BIN" >&2
  exit 1
fi

exec "$QEMU_BIN" \
  -machine virt,gic-version=2 \
  -cpu max \
  -nographic \
  -serial mon:stdio \
  -S \
  -gdb tcp::1234 \
  -kernel "$KERNEL_IMG"
