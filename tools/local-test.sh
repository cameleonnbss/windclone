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
# A CI runner is slower than a laptop and the boot path is fixed work, so
# the timeout is generous; override with e.g. TIMEOUT=10 locally.
TIMEOUT=${TIMEOUT:-25}
fail() { echo "FAIL: $1" >&2; exit 1; }

# Inject keystrokes through a QEMU monitor unix socket once the kernel
# is up: `sendkey a` makes the emulated 8042 deliver a real IRQ1, so the
# keyboard decode path is proven end-to-end, headless.
#
# The reply is read and checked because a name this QEMU does not know is
# silently dropped: `sendkey sp` (space) used to be typed as nothing at all,
# and the shell then received "echozzz" as one word and answered "command
# unknown" - a wrong-looking but passing-looking failure. Now an unaccepted
# key name stops the run instead of quietly changing the command line.
send_keys() {
    local sock=$1; shift
    python3 - "$sock" "$@" <<'EOF'
import socket, time, sys
s = socket.socket(socket.AF_UNIX); s.settimeout(5); s.connect(sys.argv[1])
time.sleep(0.2); s.recv(4096)
for k in sys.argv[2:]:
    s.sendall(('sendkey %s\n' % k).encode())
    reply = b''
    while b'(qemu)' not in reply:
        chunk = s.recv(4096)
        if not chunk:
            break
        reply += chunk
    if b'invalid' in reply.lower():
        sys.exit('    sendkey: this QEMU rejected the key name %r' % k)
    time.sleep(0.15)
# a real PS/2 packet through the emulated 8042, same idea as sendkey
s.sendall(b'mouse_move 80 40\n'); time.sleep(0.3)
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
# a/b prove the decoder; the shell then really runs `hello` in ring 3,
# `ls` enumerates the archive through SYS_GETDIRENT, `echo zzz` proves the
# command line reached _start (the word comes back verbatim), and `cat
# /etc/issue` prints bytes that exist nowhere else in this log. `spc` is
# what this QEMU's monitor calls the space bar.
send_keys /tmp/wc-biosmon a b ret h e l l o ret \
    l s ret \
    e c h o spc z z z ret \
    c a t spc slash e t c slash i s s u e ret
wait $BIOPID 2>/dev/null || true
for want in "step: pmm ok" "paging: self-checks passed" "step: paging ok" \
            "vga: text console live" "kmalloc selftest ok" "step: heap ok" \
            "boot complete" "step: interrupts armed" \
            "irq0: timer interrupt firing" "uptime: 1s" \
            "step: log ring armed" "pci: enumeration done" \
            "pci: host bridge" "step: keyboard live" \
            "keyboard: key decoded end-to-end" "shell: running 'hello'" \
            "log ring (most recent):" \
            "step: initrd ok" "initrd: /etc/motd" \
            "initrd: missing path lookup returns NULL" \
            "gdt: our own table live" "syscall: int 0x80 gate installed" \
            "vfs: descriptor table live" "step: vfs ok" \
            "vfs: read-back, offset, ebadf and enoent checks passed" \
            "vfs: exists + dirent enumeration matches the archive" \
            "elf64, entry" "hello from ring 3" \
            "step: userland ok (elf64 + ring 3 round trip)" \
            "\\[motd\\] segments: .data marker=0x5a5a .bss counter=0" \
            "\\[motd\\] closed. bytes read: " \
            "\\[motd\\] read after close returned -9" \
            "gfx: framebuffer" "gfx: desktop composited" \
            "mouse: 8042 aux channel live" "mouse: first packet decoded" \
            "shell: hello ran and returned" \
            "shell: running 'ls'" "\\[ls\\] " "entries, " \
            "shell: ls ran and returned" \
            "shell: running 'echo'" "zzz" \
            "shell: echo ran and returned" \
            "shell: running 'cat'" "0.0.4 - initrd live" \
            "shell: cat ran and returned" "shell: cat exited with 0"; do
    grep -aq "$want" /tmp/wc-bios.log || fail "BIOS log missing: $want"
done
echo '    ok: BIOS boot — memory, interrupts, PCI, keyboard (sendkey), log ring'

# 3) UEFI boot (OVMF), same contract minus the PIC-specific line.
#    The firmware files are packaged differently per distro — and a rootless
#    toolchain extract keeps them outside /usr — so look for them rather than
#    assume a path. CI installs ovmf, so this branch always runs there.
OVMF_CODE=
OVMF_VARS=
for d in /usr/share/OVMF /usr/share/ovmf /usr/share/qemu \
         "${OVMF_DIR:-}" "$HOME/os-tools/root/usr/share/OVMF"; do
    [ -d "$d" ] || continue
    for c in "$d/OVMF_CODE_4M.fd" "$d/OVMF_CODE.fd"; do
        v="${c/CODE/VARS}"
        [ -f "$c" ] && [ -f "$v" ] || continue
        OVMF_CODE=$c
        OVMF_VARS=$v
        break 2
    done
done
if [ -z "$OVMF_CODE" ]; then
    echo '    skip: UEFI boot — no OVMF firmware found (install ovmf or set OVMF_DIR)'
    echo '          CI still runs this check on ubuntu-latest'
else
    rm -f /tmp/wc-uefi.log /tmp/wc-vars.fd
    cp "$OVMF_VARS" /tmp/wc-vars.fd
    timeout "$TIMEOUT" $Q -cdrom windclone.iso -m 512 -display none -no-reboot \
        -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
        -drive if=pflash,format=raw,file=/tmp/wc-vars.fd \
        -serial file:/tmp/wc-uefi.log || true
    for want in "step: pmm ok" "step: paging ok" "vga: text console live" \
                "kmalloc selftest ok" "step: heap ok" "boot complete" "uptime: 1s" \
                "pci: enumeration done" "step: keyboard live" \
                "log ring (most recent):" "step: initrd ok" \
                "initrd: /etc/motd" "step: vfs ok" \
                "vfs: exists + dirent enumeration matches the archive" \
                "step: userland ok (elf64 + ring 3 round trip)" \
                "gfx: framebuffer" "gfx: desktop composited"; do
        grep -aq "$want" /tmp/wc-uefi.log || fail "UEFI log missing: $want"
    done
    echo '    ok: UEFI boot — same memory + device stack comes up identically'
fi

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

# 5) window manager: real PS/2 clicks through the QEMU monitor socket. Each
#    discrete window action is a kernel log line, so a headless run proves
#    that minimise / restore / close / reopen really happened — no display,
#    no screenshots, no eyeballs. tools/qemu-desktop.py does the driving.
echo '==> window manager (mouse clicks)'
python3 tools/qemu-desktop.py windclone.iso minimize
python3 tools/qemu-desktop.py windclone.iso maxmax
python3 tools/qemu-desktop.py windclone.iso close
python3 tools/qemu-desktop.py windclone.iso start

# 6) kernel text console: the "vga: text console live" marker (checked in
#    the BIOS + UEFI logs above) proves the kernel mapped 0xB8000 through
#    its OWN page tables and a text cell written there reads back intact.
#    A composited GRAPHICAL terminal (Limine framebuffer + font) is tracked
#    in docs/ROADMAP.md under Phase 8 (framebuffer graphics).

echo '==> all local smoke tests passed'
