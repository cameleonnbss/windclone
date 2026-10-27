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
# artifacts, not sources, so they never land in the tree). The list below is
# the same set as USER_BINS in the Makefile: a program that is missing simply
# stays out of the archive, which is why `make` builds userland first - and
# why the warning is loud rather than silent, because a /bin with fewer names
# than the Makefile promises shows up at the prompt as "command unknown".
rm -rf "$STAGE"
mkdir -p "$STAGE/bin"
cp -a "$ROOT"/. "$STAGE"/
for prog in hello motd ls cat echo tick spin exit waiter bell reaper vanish whoami orphan nest hold save; do
    if [ ! -f "build/$prog" ]; then
        echo "[mkinitrd] WARNING: build/$prog is missing, /bin/$prog will not exist" >&2
        continue
    fi
    cp "build/$prog" "$STAGE/bin/$prog"
done

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
