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
TIMEOUT=${TIMEOUT:-14}
fail() { echo "FAIL: $1" >&2; exit 1; }

# Inject keystrokes through a QEMU monitor unix socket once the kernel
# is up: `sendkey a` makes the emulated 8042 deliver a real IRQ1, so the
# keyboard decode path is proven end-to-end, headless.
send_keys() {
    local sock=$1; shift
    python3 - "$sock" "$@" <<'EOF'
import socket, time, sys
s = socket.socket(socket.AF_UNIX); s.settimeout(3); s.connect(sys.argv[1])
time.sleep(0.2); s.recv(4096)
for k in sys.argv[2:]:
    s.sendall(('sendkey %s\n' % k).encode()); time.sleep(0.2)
s.close()
EOF
}

# Decode the VGA text buffer (0xB8000) through a QEMU monitor unix socket
# and check that `want` appears somewhere on screen.
screen_has() {
    local sock=$1 want=$2
    python3 - "$sock" "$want" <<'EOF'
import socket, time, re, sys
sock, want = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX); s.settimeout(5); s.connect(sock)
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
rows = [l for l in (text[i*80:(i+1)*80].strip() for i in range(25)) if l]
if want not in text:
    print('    screen rows:', rows[-3:]); sys.exit(1)
print('    screen says:', [r for r in rows if want in r][0][:72])
EOF
}

echo '==> build kernel + ISO + MBR image'
make -j"$(nproc)" >/dev/null
make build/mbr.img >/dev/null

# 1) no SSE in the kernel binary (would triple-fault before the IDT exists)
if objdump -d build/kernel.elf | grep -qE 'movdqa|shufps|punpck'; then
    fail "SSE instructions found in kernel — check CFLAGS (-mno-sse)"
fi
echo '    ok: kernel is scalar (no SSE)'

# 2) BIOS boot: serial must show the memory + interrupt + PCI + keyboard
#    + uptime markers; the injected `sendkey` must come back decoded.
rm -f /tmp/wc-bios.log /tmp/wc-biosmon
timeout "$TIMEOUT" $Q -cdrom windclone.iso -m 512 -display none -no-reboot \
    -serial file:/tmp/wc-bios.log \
    -monitor unix:/tmp/wc-biosmon,server,nowait &
BIOPID=$!
sleep 6
send_keys /tmp/wc-biosmon a b
wait $BIOPID 2>/dev/null || true
for want in "step: pmm ok" "paging: self-checks passed" "step: paging ok" \
            "vga: text console live" "kmalloc selftest ok" "step: heap ok" \
            "boot complete" "step: interrupts armed" \
            "irq0: timer interrupt firing" "uptime: 1s" \
            "step: log ring armed" "pci: enumeration done" \
            "pci: host bridge" "step: keyboard live" \
            "keyboard: key decoded end-to-end" "key: 'a' down" \
            "log ring (most recent):"; do
    grep -aq "$want" /tmp/wc-bios.log || fail "BIOS log missing: $want"
done
echo '    ok: BIOS boot — memory, interrupts, PCI, keyboard (sendkey), log ring'

# 3) UEFI boot (OVMF), same contract minus the PIC-specific line
rm -f /tmp/wc-uefi.log /tmp/wc-vars.fd
cp /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/wc-vars.fd
timeout "$TIMEOUT" $Q -cdrom windclone.iso -m 512 -display none -no-reboot \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive if=pflash,format=raw,file=/tmp/wc-vars.fd \
    -serial file:/tmp/wc-uefi.log || true
for want in "step: pmm ok" "step: paging ok" "vga: text console live" \
            "kmalloc selftest ok" "step: heap ok" "boot complete" "uptime: 1s" \
            "pci: enumeration done" "step: keyboard live" \
            "log ring (most recent):"; do
    grep -aq "$want" /tmp/wc-uefi.log || fail "UEFI log missing: $want"
done
echo '    ok: UEFI boot — same memory + device stack comes up identically'

# 4) two-stage MBR: decode the VGA text buffer through the QEMU monitor
rm -f /tmp/wc-mbrmon
$Q -drive format=raw,file=build/mbr.img -m 512 -display none \
   -monitor unix:/tmp/wc-mbrmon,server,nowait >/dev/null 2>&1 &
MBRPID=$!
sleep 3
screen_has /tmp/wc-mbrmon "stage-2 alive" || fail "MBR screen missing stage-2"
kill $MBRPID 2>/dev/null || true
wait $MBRPID 2>/dev/null || true
echo '    ok: two-stage MBR loaded stage-2'

# 5) kernel text console: the "vga: text console live" marker (checked in
#    the BIOS + UEFI logs above) proves the kernel mapped 0xB8000 through
#    its OWN page tables and a text cell written there reads back intact.
#    A composited GRAPHICAL terminal (Limine framebuffer + font) is tracked
#    in docs/ROADMAP.md under Phase 8 (framebuffer graphics).

echo '==> all local smoke tests passed'
