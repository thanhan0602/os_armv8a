#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "usage: $0 <remote-path> <local-elf> [task-name]" >&2
  exit 1
fi

host="${HOST:-127.0.0.1}"
port="${PORT:-5555}"
read_timeout="${READ_TIMEOUT:-8}"
remote_path="$1"
local_elf="$2"
task_name="${3:-}"

if [[ ! -f "$local_elf" ]]; then
  echo "local ELF not found: $local_elf" >&2
  exit 1
fi

if ! command -v xxd >/dev/null 2>&1; then
  echo "xxd is required" >&2
  exit 1
fi

size="$(stat -c '%s' "$local_elf")"

exec 3<>"/dev/tcp/$host/$port"

printf '\nreceive %s %s\n' "$remote_path" "$size" >&3
xxd -p -c 512 "$local_elf" >&3
printf '\n' >&3

if [[ -n "$task_name" ]]; then
  printf 'load %s %s\n' "$remote_path" "$task_name" >&3
else
  printf 'load %s\n' "$remote_path" >&3
fi

timeout "$read_timeout" cat <&3 || true
