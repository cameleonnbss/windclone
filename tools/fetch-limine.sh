#!/bin/sh
# WindClone — download the prebuilt Limine boot files needed for the ISO.
# Source of truth: the limine "binary" branch of the upstream project.
# SPDX-License-Identifier: MIT
set -eu

BRANCH="${LIMINE_BRANCH:-v9.x-binary}"
OUT_DIR="limine"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

echo "[fetch-limine] branch: $BRANCH"
mkdir -p "$OUT_DIR"

URL="https://github.com/limine-bootloader/limine/archive/refs/heads/${BRANCH}.tar.gz"
curl -sSL "$URL" -o "$TMP_DIR/limine.tar.gz"
tar -xzf "$TMP_DIR/limine.tar.gz" -C "$TMP_DIR"

SRC="$(find "$TMP_DIR" -maxdepth 1 -type d -iname 'limine*' | head -n1)"
if [ -z "$SRC" ]; then
    echo "[fetch-limine] ERROR: could not locate extracted archive directory" >&2
    exit 1
fi

for f in limine-bios-cd.bin limine-uefi-cd.bin limine-bios.sys \
         BOOTX64.EFI BOOTIA32.EFI limine.h limine.c limine.exe limine-bios-hdd.h; do
    cp "$SRC/$f" "$OUT_DIR/$f"
    echo "[fetch-limine]   -> $OUT_DIR/$f"
done
chmod +x "$OUT_DIR/limine.exe" 2>/dev/null || true

echo "[fetch-limine] done."
