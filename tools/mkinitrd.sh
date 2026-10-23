#!/bin/sh
# WindClone — pack rootfs/ into the archive the bootloader hands to the
# kernel as a module.
#
# Format is ustar on purpose: the kernel parses it in place with a few
# hundred bytes of code (no decompression, no filesystem driver, no libc).
# Keep it that way — GNU and BSD tar both spell it `--format=ustar`, and
# neither writes pax extended headers for the short paths we use.
#
#   ./tools/mkinitrd.sh [out.tar] [root-dir]
#
# SPDX-License-Identifier: MIT
set -eu

OUT=${1:-build/initrd.tar}
ROOT=${2:-rootfs}
STAGE=build/initrd-root

[ -d "$ROOT" ] || { echo "[mkinitrd] ERROR: missing $ROOT/" >&2; exit 1; }
mkdir -p "$(dirname "$OUT")"

# Stage = the checked-in rootfs plus the programs we build (they are
# artifacts, not sources, so they never land in the tree).
rm -rf "$STAGE"
mkdir -p "$STAGE/bin"
cp -a "$ROOT"/. "$STAGE"/
[ -f build/hello ] && cp build/hello "$STAGE/bin/hello"

# "." makes tar emit relative entries (./etc/motd); the kernel strips the
# leading "./" so lookups can use "/etc/motd". Directories become typeflag
# '5' entries, which the parser skips.
#
# --owner/--group/--numeric-owner: the uid of whoever runs the build can be
# outside tar's own uid_t range (containers routinely are), and the archive
# does not need real ownership anyway - the kernel ignores those fields.
tar --format=ustar --owner=0 --group=0 --numeric-owner \
    -C "$STAGE" -cf "$OUT" .

echo "[mkinitrd] $OUT ($(wc -c < "$OUT") bytes, $(find "$ROOT" -type f | wc -l) files)"
