#!/bin/bash
# WindClone — local smoke tests (what CI runs, on your machine).
#
#   ./tools/local-test.sh          # build + BIOS + UEFI boot checks
#
# Requires: make, qemu-system-x86_64, ovmf, nasm, xorriso.
# SPDX-License-Identifier: MIT
set -e
cd "$(git rev-parse --show-toplevel 2>/dev/null || dirname "$(dirname "$0")")"

Q="qemu-system-x86_64"
TIMEOUT=${TIMEOUT:-10}
fail() { echo "FAIL: $1" >&2; exit 1; }

echo '==> build kernel + ISO + MBR image'
make -j"$(nproc)" >/dev/null
make build/mbr.img >/dev/null

# 1) no SSE in the kernel binary (would triple-fault before the IDT exists)
if objdump -d build/kernel.elf | grep -qE 'movdqa|shufps|punpck'; then
    fail "SSE instructions found in kernel — check CFLAGS (-mno-sse)"
fi
echo '    ok: kernel is scalar (no SSE)'

# 2) BIOS boot: serial must show the interrupt + uptime markers
rm -f /tmp/wc-bios.log
timeout "$TIMEOUT" $Q -cdrom windclone.iso -m 512 -display none \
    -no-reboot -serial file:/tmp/wc-bios.log || true
for want in "boot complete" "step: interrupts armed" \
            "irq0: timer interrupt firing" "uptime: 1s"; do
    grep -aq "$want" /tmp/wc-bios.log || fail "BIOS log missing: $want"
done
echo '    ok: BIOS boot + timer interrupts'

# 3) UEFI boot (OVMF), same contract minus the PIC-specific line
rm -f /tmp/wc-uefi.log /tmp/wc-vars.fd
cp /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/wc-vars.fd
timeout "$TIMEOUT" $Q -cdrom windclone.iso -m 512 -display none -no-reboot \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive if=pflash,format=raw,file=/tmp/wc-vars.fd \
    -serial file:/tmp/wc-uefi.log || true
for want in "boot complete" "uptime: 1s"; do
    grep -aq "$want" /tmp/wc-uefi.log || fail "UEFI log missing: $want"
done
echo '    ok: UEFI boot'

# 4) two-stage MBR: decode the VGA text buffer through the QEMU monitor
rm -f /tmp/wc-mbrmon /tmp/wc-mbr.out
$Q -drive format=raw,file=build/mbr.img -m 512 -display none \
   -monitor unix:/tmp/wc-mbrmon,server,nowait >/dev/null 2>&1 &
MBRPID=$!
sleep 3
python3 - <<'EOF'
import socket, time, re
s = socket.socket(socket.AF_UNIX); s.settimeout(5); s.connect('/tmp/wc-mbrmon')
time.sleep(0.2); s.recv(4096)
s.sendall(b'xp/4000xb 0xb8000\n')
buf = b''
while b'(qemu)' not in buf[-20:]:
    buf += s.recv(65536)
s.sendall(b'quit\n')
clean = re.sub(rb'\x1b\[[KD]', b'', buf).decode('latin1')
hb = []
for line in clean.split('\n'):
    m = re.match(r'^\s*0{6,}[0-9a-f]{2,16}:\s+(.*)$', line)
    if m: hb += re.findall(r'0x([0-9a-f]{2})(?![0-9a-f])', m.group(1))
text = ''.join(chr(int(h,16)) if 32 <= int(h,16) < 127 else ' ' for h in hb[0::2])
ok = 'stage-2 alive' in text
print('    MBR screen says:', [l for l in (text[i*80:(i+1)*80] for i in range(25)) if l.strip()][-1:])
exit(0 if ok else 1)
EOF
kill $MBRPID 2>/dev/null || true
wait $MBRPID 2>/dev/null || true
echo '    ok: two-stage MBR loaded stage-2'

echo '==> all local smoke tests passed'
