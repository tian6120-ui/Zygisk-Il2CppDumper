#!/usr/bin/env bash
set -euo pipefail

SO="$1"
BUILD="$2"
PROP="$3"

OUT="$BUILD/inkvasion_pkg"
ZIP="$BUILD/Inkvasion_Standalone_AutoUnlock_V1.zip"

rm -rf "$OUT" "$ZIP"
mkdir -p "$OUT/zygisk"

cp "$SO" "$OUT/zygisk/arm64-v8a.so"
cp "$PROP" "$OUT/module.prop"
: > "$OUT/skip_mount"

(
  cd "$OUT"
  zip -qr "$ZIP" .
)

echo "INKVASION_ZIP_PATH=$ZIP"
echo "INKVASION_ZIP_SHA256=$(sha256sum "$ZIP" | awk '{print $1}')"
echo "INKVASION_ZIP_BASE64_BEGIN"
base64 -w 0 "$ZIP"
echo
echo "INKVASION_ZIP_BASE64_END"
