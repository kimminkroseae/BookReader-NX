#!/usr/bin/env bash
# Builds BookReader.nro. devkitPro's makefiles break on paths containing
# shell characters such as '&', so the project is mirrored into a plain path
# (~/.cache/wookreader-build), built there, and the .nro copied back.
set -euo pipefail
SRC="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BOOKREADER_BUILD_DIR:-$HOME/.cache/wookreader-build}"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"

mkdir -p "$BUILD"
rsync -a --delete \
  --exclude /build --exclude /lib --exclude /mupdf/generated --exclude '*.o' \
  --exclude '/*.nro' --exclude '/*.elf' --exclude '/*.nacp' \
  "$SRC/" "$BUILD/"
cd "$BUILD"
make -j"$(nproc)" "$@"
cp -f "$BUILD/BookReader.nro" "$SRC/BookReader.nro"
echo ">>> Built $SRC/BookReader.nro"
