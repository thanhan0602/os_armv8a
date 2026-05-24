#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CODE_BIN="${CODE_BIN:-/home/a/.vscode-server/cli/servers/Stable-0958016b2af9f09bb4257e0df4a95e2f90590f9f/server/bin/remote-cli/code}"

VSIX_PATH="$($ROOT_DIR/scripts/build_vsix.sh)"
"$CODE_BIN" --install-extension "$VSIX_PATH" --force

echo "Installed $VSIX_PATH"
echo "Reload Window is still recommended so contributed commands/UI definitely refresh."