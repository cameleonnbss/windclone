#!/bin/sh
# WindClone — assemble a BIOS + UEFI hybrid ISO, following the official
# Limine "BIOS/UEFI hybrid ISO creation" recipe (USAGE.md).
#
# Result boots on: legacy BIOS (CD), UEFI (CD), and — thanks to the
# bios-install pass — as a dd-able raw image on USB sticks.
#
# Run from the repository root. SPDX-License-Identifier: MIT
set -eu

ISO="windclone.iso"
KERNEL="build/kernel.elf"
INITRD="build/initrd.tar"
LIMINE_DIR="limine"

for f in "$KERNEL" "$INITRD" limine.conf \
         "$LIMINE_DIR/limine-bios-cd.bin" "$LIMINE_DIR/limine-uefi-cd.bin" \
         "$LIMINE_DIR/limine-bios.sys" "$LIMINE_DIR/BOOTX64.EFI" "$LIMINE_DIR/BOOTIA32.EFI"; do
    [ -e "$f" ] || { echo "[build-iso] missing $f - run ./tools/fetch-limine.sh first"; exit 1; }
done

rm -rf isodir
mkdir -p isodir/boot isodir/EFI/BOOT

# Kernel + boot config (limine.conf / limine-bios.sys must be in one of
# root, limine/, boot/, boot/limine/ — we use boot/)
cp "$KERNEL"                         isodir/boot/kernel.elf
cp "$INITRD"                         isodir/boot/initrd.tar
cp limine.conf                       isodir/boot/limine.conf
cp "$LIMINE_DIR/limine-bios.sys"     isodir/boot/limine-bios.sys

# UEFI fallback loaders (also what the UEFI firmware loads from ESP)
cp "$LIMINE_DIR/BOOTX64.EFI"         isodir/EFI/BOOT/BOOTX64.EFI
cp "$LIMINE_DIR/BOOTIA32.EFI"        isodir/EFI/BOOT/BOOTIA32.EFI

# El Torito boot catalog pieces (can live anywhere in the tree)
cp "$LIMINE_DIR/limine-bios-cd.bin"  isodir/limine-bios-cd.bin
cp "$LIMINE_DIR/limine-uefi-cd.bin"  isodir/limine-uefi-cd.bin

xorriso -as mkisofs -R -r -J \
    -V WINDCLONE \
    -b limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    --efi-boot limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    isodir -o "$ISO"

# Make the hybrid MBR bootable when the image is dd'd to a USB stick.
# Use the prebuilt host tool; fall back to compiling limine.c.
if [ -x "$LIMINE_DIR/limine.exe" ] && "$LIMINE_DIR/limine.exe" --version >/dev/null 2>&1; then
    "$LIMINE_DIR/limine.exe" bios-install "$ISO"
elif [ -f "$LIMINE_DIR/limine.c" ]; then
    ${CC:-cc} -O2 -std=c11 "$LIMINE_DIR/limine.c" -o build/limine-host \
        && ./build/limine-host bios-install "$ISO" \
        || echo "[build-iso] WARNING: could not build limine host tool; ISO still boots from CD/UEFI."
else
    echo "[build-iso] NOTE: skipped bios-install (no host tool); ISO still boots from CD/UEFI."
fi

echo "[build-iso] wrote $ISO ($(wc -c < "$ISO") bytes)"
