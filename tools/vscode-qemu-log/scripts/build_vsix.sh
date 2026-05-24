#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DIST_DIR="$ROOT_DIR/dist"
TMP_DIR="$ROOT_DIR/.vsix-tmp"

NAME="$(python3 - <<'PY' "$ROOT_DIR/package.json"
import json, sys
with open(sys.argv[1], 'r', encoding='utf-8') as f:
    pkg = json.load(f)
print(pkg['name'])
PY
)"
PUBLISHER="$(python3 - <<'PY' "$ROOT_DIR/package.json"
import json, sys
with open(sys.argv[1], 'r', encoding='utf-8') as f:
    pkg = json.load(f)
print(pkg['publisher'])
PY
)"
VERSION="$(python3 - <<'PY' "$ROOT_DIR/package.json"
import json, sys
with open(sys.argv[1], 'r', encoding='utf-8') as f:
    pkg = json.load(f)
print(pkg['version'])
PY
)"
DISPLAY_NAME="$(python3 - <<'PY' "$ROOT_DIR/package.json"
import json, sys
with open(sys.argv[1], 'r', encoding='utf-8') as f:
    pkg = json.load(f)
print(pkg['displayName'])
PY
)"
DESCRIPTION="$(python3 - <<'PY' "$ROOT_DIR/package.json"
import json, sys
with open(sys.argv[1], 'r', encoding='utf-8') as f:
    pkg = json.load(f)
print(pkg['description'])
PY
)"

VSIX_PATH="$DIST_DIR/${PUBLISHER}.${NAME}-${VERSION}.vsix"

rm -rf "$TMP_DIR"
mkdir -p "$TMP_DIR/extension" "$DIST_DIR"

cp "$ROOT_DIR/package.json" "$TMP_DIR/extension/package.json"
cp "$ROOT_DIR/tsconfig.json" "$TMP_DIR/extension/tsconfig.json"
cp -R "$ROOT_DIR/out" "$TMP_DIR/extension/out"
cp -R "$ROOT_DIR/src" "$TMP_DIR/extension/src"

cat > "$TMP_DIR/extension.vsixmanifest" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<PackageManifest Version="2.0.0" xmlns="http://schemas.microsoft.com/developer/vsx-schema/2011">
  <Metadata>
    <Identity Language="en-US" Id="${PUBLISHER}.${NAME}" Version="${VERSION}" Publisher="${PUBLISHER}" />
    <DisplayName>${DISPLAY_NAME}</DisplayName>
    <Description xml:space="preserve">${DESCRIPTION}</Description>
    <Categories>Other</Categories>
  </Metadata>
  <Installation>
    <InstallationTarget Id="Microsoft.VisualStudio.Code" />
  </Installation>
  <Dependencies />
  <Assets>
    <Asset Type="Microsoft.VisualStudio.Code.Manifest" Path="extension/package.json" />
  </Assets>
</PackageManifest>
EOF

cat > "$TMP_DIR/[Content_Types].xml" <<'EOF'
<?xml version="1.0" encoding="utf-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="json" ContentType="application/json" />
  <Default Extension="js" ContentType="application/javascript" />
  <Default Extension="ts" ContentType="text/plain" />
  <Default Extension="map" ContentType="application/json" />
  <Default Extension="xml" ContentType="text/xml" />
  <Default Extension="md" ContentType="text/markdown" />
  <Default Extension="txt" ContentType="text/plain" />
</Types>
EOF

rm -f "$VSIX_PATH"
cd "$TMP_DIR"
zip -qr "$VSIX_PATH" .

echo "$VSIX_PATH"