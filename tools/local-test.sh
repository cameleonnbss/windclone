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
    c a t spc slash e t c slash i s s u e ret \
    r e a p e r ret
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
            "shell: cat ran and returned" "shell: cat exited with 0" \
            "shell: running 'reaper'" \
            "\\[reaper\\] spawned pid -1" \
            "\\[reaper\\] wait again -> -10" \
            "shell: reaper ran and returned"; do
    grep -aq "$want" /tmp/wc-bios.log || fail "BIOS log missing: $want"
done

# The scheduler demo runs on its own once the timer is up: two copies of
# /bin/tick, each with its own address space and pid, printing a line and
# yielding in turn. Every marker below is a fixed line; the interesting one
# is the ORDER, proven separately under this — a run-to-completion loop with
# extra steps would still print all eight lines, just as A-1 A-2 A-3 A-done
# B-1 ..., so the interleaving check is the one that actually fails if the
# context switch is a lie.
for want in "sched: building two processes" \
            "proc: spawned /bin/tick as pid 1" \
            "proc: spawned /bin/tick as pid 2" \
            "sched: round-robin over the process table" \
            "\\[tick A\\] step 1 pid 1" "\\[tick B\\] step 1 pid 2" \
            "\\[tick A\\] step 2 pid 1" "\\[tick B\\] step 2 pid 2" \
            "\\[tick A\\] done" "\\[tick B\\] done" \
            "sched: all processes exited" \
            "step: scheduler ok (two processes, round-robin)"; do
    grep -aq "$want" /tmp/wc-bios.log || fail "BIOS log missing: $want"
done
# Round-robin proven order-free: each process must print at least once while
# the other still has lines left, so neither ran to the end before the other
# began. Line adjacency ("B-1 right after A-1") used to be the check, but with
# a preemptive tick the first timer switch may land before B's very first
# write, legitimately putting two A lines first — what stays impossible for a
# run-to-completion loop is the crossing tested here.
TA1=$(grep -anF '[tick A]' /tmp/wc-bios.log | head -1 | cut -d: -f1)
TAL=$(grep -anF '[tick A]' /tmp/wc-bios.log | tail -1 | cut -d: -f1)
TB1=$(grep -anF '[tick B]' /tmp/wc-bios.log | head -1 | cut -d: -f1)
TBL=$(grep -anF '[tick B]' /tmp/wc-bios.log | tail -1 | cut -d: -f1)
[ -n "$TA1" ] && [ -n "$TAL" ] && [ -n "$TB1" ] && [ -n "$TBL" ] \
    || fail "tick markers vanished from the BIOS log"
{ [ "$TA1" -lt "$TBL" ] && [ "$TB1" -lt "$TAL" ]; } \
    || fail "scheduler did not interleave the two processes"
echo '    ok: scheduler — two processes interleaved (round-robin)'

# Preemption is a strictly stronger claim than the cooperative test above, so
# it gets its own program and its own check. Two /bin/spin processes burn CPU
# in a hot loop and NEVER call sched_yield — cooperative round-robin could not
# interleave them (one would spin all the way to done before the other printed
# a line). pids are 3 and 4: next_pid keeps counting across the tick demo.
for want in "sched: two compute-heavy procs, no yields" \
            "proc: spawned /bin/spin as pid 3" \
            "proc: spawned /bin/spin as pid 4" \
            "\\[spin A\\] start pid 3" "\\[spin B\\] start pid 4" \
            "\\[spin A\\] round 3" "\\[spin B\\] round 3" \
            "\\[spin A\\] done" "\\[spin B\\] done" \
            "step: preemption ok (timer switched a busy process)"; do
    grep -aq "$want" /tmp/wc-bios.log || fail "BIOS log missing: $want"
done
# Interleaving, stated order-free: the first tick after usermode_enter() may
# land before A's first write or after it, so which process prints first is
# timing luck — but run-to-completion is not. If each process prints at least
# once while the other still has lines left, neither ran to the end before the
# other began, which is exactly what only a preemptive switch can produce:
# A's first line must come before B's last, and B's first before A's last.
A1=$(grep -anF '[spin A]' /tmp/wc-bios.log | head -1 | cut -d: -f1)
AL=$(grep -anF '[spin A]' /tmp/wc-bios.log | tail -1 | cut -d: -f1)
B1=$(grep -anF '[spin B]' /tmp/wc-bios.log | head -1 | cut -d: -f1)
BL=$(grep -anF '[spin B]' /tmp/wc-bios.log | tail -1 | cut -d: -f1)
[ -n "$A1" ] && [ -n "$AL" ] && [ -n "$B1" ] && [ -n "$BL" ] \
    || fail "spin markers vanished from the BIOS log"
{ [ "$A1" -lt "$BL" ] && [ "$B1" -lt "$AL" ]; } \
    || fail "spin processes did not interleave (no preemption?)"
echo '    ok: preemption — timer switched a CPU-bound process'

# The trilogy's last act, its own program and its own proof. /bin/waiter
# spawns /bin/exit 7 and /bin/exit 9 with real argv (a first: userland asked
# for a spawn WITH a command line, shuttled through kernel bounce buffers),
# then waits. The children's pids are NOT hardcoded on purpose: the waiter
# itself takes a pid when the scheduler's exit path reaps it, and which pid
# that is depends on how many demo blocks run before it — a static number here
# would rot the next time the boot sequence moved. The spawn lines name the
# pids, so the cross-checks READ them back and verify every hop of the trip:
# spawned pid/code == child's own exit line == what wait() handed back.
# Which child dies first is timing luck, so every assertion stays order-free.
for want in "proc: one parent, two children, a blocking wait" \
            "proc: spawned /bin/exit as pid" \
            "\\[waiter\\] spawned pid" \
            "\\[exit\\] code 7 pid" "\\[exit\\] code 9 pid" \
            "\\[waiter\\] reaped pid" \
            "\\[waiter\\] all children reaped" \
            "step: wait ok (spawn with argv + blocking wait + reaping)"; do
    grep -aq "$want" /tmp/wc-bios.log || fail "BIOS log missing: $want"
done
grep -acF '[waiter] spawned pid' /tmp/wc-bios.log | grep -qx 2 \
    || fail "waiter did not spawn exactly two children"
grep -acF '[waiter] reaped pid' /tmp/wc-bios.log | grep -qx 2 \
    || fail "waiter did not reap exactly two children"
# pid/code pairings straight off the waiter's own spawn lines:
P7=$(grep -aF '[waiter] spawned pid' /tmp/wc-bios.log | grep -F 'code 7' | sed 's/.*pid \([0-9]*\).*/\1/')
P9=$(grep -aF '[waiter] spawned pid' /tmp/wc-bios.log | grep -F 'code 9' | sed 's/.*pid \([0-9]*\).*/\1/')
[ -n "$P7" ] && [ -n "$P9" ] || fail "waiter spawn lines lost their pid/code pairing"
[ "$P7" != "$P9" ] || fail "both children report the same pid"
grep -Fq "[exit] code 7 pid $P7" /tmp/wc-bios.log \
    || fail "child $P7 did not print the exit code its argv carried"
grep -Fq "[exit] code 9 pid $P9" /tmp/wc-bios.log \
    || fail "child $P9 did not print the exit code its argv carried"
grep -Fq "[waiter] reaped pid $P7 code 7" /tmp/wc-bios.log \
    || fail "wait() did not return child $P7 with its exit code"
grep -Fq "[waiter] reaped pid $P9 code 9" /tmp/wc-bios.log \
    || fail "wait() did not return child $P9 with its exit code"
{ grep -aq 'sched: deadlock' /tmp/wc-bios.log \
  && fail "scheduler deadlocked — a waiter was never woken"; } || true
echo '    ok: wait — spawn with argv, blocking wait, wake-up, zombie reaping'

# The fourth verb. /bin/reaper spawns /bin/bell (a child that runs for seconds
# and never quits on its own), yields so it demonstrably lives, kills it, and
# reaps it. /bin/vanish covers self-kill, the one case that cannot be done as a
# table edit. As in the wait demo, pids are READ back from the log instead of
# hardcoded — and here the cross-checks are the whole point: the pid the kernel
# handed the parent on spawn must be the pid the child printed for itself, the
# pid the kill names, and the pid wait() returns, with the code the kernel
# marked (-SIGKILL). Two claims are absences, and they are the ones that prove
# the kill actually stopped something: the bell never reached its final line,
# and vanish never came back from a call that is not supposed to return.
for want in "proc: a parent that kills, and a process that ends itself" \
            "proc: spawned /bin/bell as pid" \
            "\\[reaper\\] spawned pid" \
            "\\[bell\\] ringing pid" "\\[bell\\] round 1" \
            "proc: killed pid" "\\[reaper\\] kill" \
            "\\[reaper\\] reaped pid" \
            "\\[reaper\\] kill again -> -3" \
            "\\[reaper\\] kill sig 1 -> -22" \
            "\\[reaper\\] wait again -> -10" \
            "\\[reaper\\] child killed and reaped" \
            "\\[vanish\\] bye pid" \
            "step: kill ok (SIGKILL ends a process, its parent reaps it)"; do
    grep -aq "$want" /tmp/wc-bios.log || fail "BIOS log missing: $want"
done
KID=$(grep -aF '[reaper] spawned pid' /tmp/wc-bios.log | sed 's/.*pid \(-\?[0-9]*\).*/\1/')
[ -n "$KID" ] || fail "the reaper's child has no pid to follow"
grep -Fq "[bell] ringing pid $KID" /tmp/wc-bios.log \
    || fail "the bell's own getpid() ($KID expected) does not match what spawn returned"
grep -Fq "[reaper] kill $KID -> 0" /tmp/wc-bios.log \
    || fail "kill() did not answer 0 for the pid spawn handed back"
grep -Fq "proc: killed pid $KID (SIGKILL)" /tmp/wc-bios.log \
    || fail "the kernel did not report killing $KID"
grep -Fq "[reaper] reaped pid $KID code -9" /tmp/wc-bios.log \
    || fail "wait() did not return the killed child with code -SIGKILL"
# The killed child must not have run to its own ending, and the self-killed
# process must not have returned from the call that ends it.
{ grep -aqF '[bell] done' /tmp/wc-bios.log \
  && fail "the bell finished on its own — kill() did not stop it"; } || true
{ grep -aqF '[vanish] STILL HERE' /tmp/wc-bios.log \
  && fail "self-kill returned into the process instead of ending it"; } || true
VPID=$(grep -aF '[vanish] bye pid' /tmp/wc-bios.log | sed 's/.*pid \([0-9]*\).*/\1/')
[ -n "$VPID" ] || fail "vanish printed no pid"
grep -Fq "proc: killed pid $VPID (SIGKILL)" /tmp/wc-bios.log \
    || fail "the kernel did not report vanish's self-kill of $VPID"
{ grep -aq 'sched: deadlock' /tmp/wc-bios.log \
  && fail "scheduler deadlocked — a kill woke nobody"; } || true
echo '    ok: kill — SIGKILL ends a running child, its parent reaps code -9'
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
                "proc: spawned /bin/tick as pid 1" \
                "\\[tick A\\] step 1 pid 1" "\\[tick B\\] step 1 pid 2" \
                "step: scheduler ok (two processes, round-robin)" \
                "\\[spin A\\] start pid 3" "\\[spin B\\] start pid 4" \
                "step: preemption ok (timer switched a busy process)" \
                "proc: spawned /bin/exit as pid" \
                "\\[waiter\\] spawned pid" \
                "\\[exit\\] code 7 pid" "\\[exit\\] code 9 pid" \
                "\\[waiter\\] reaped pid" \
                "\\[waiter\\] all children reaped" \
                "step: wait ok (spawn with argv + blocking wait + reaping)" \
                "\\[reaper\\] spawned pid" "\\[bell\\] ringing pid" \
                "\\[reaper\\] kill" "\\[reaper\\] reaped pid" \
                "\\[reaper\\] child killed and reaped" \
                "\\[vanish\\] bye pid" \
                "step: kill ok (SIGKILL ends a process, its parent reaps it)" \
                "gfx: framebuffer" "gfx: desktop composited"; do
        grep -aq "$want" /tmp/wc-uefi.log || fail "UEFI log missing: $want"
    done
    UKID=$(grep -aF '[reaper] spawned pid' /tmp/wc-uefi.log | sed 's/.*pid \(-\?[0-9]*\).*/\1/')
    grep -Fq "[reaper] reaped pid $UKID code -9" /tmp/wc-uefi.log \
        || fail "UEFI: wait() did not return the killed child with code -SIGKILL"
    { grep -aqF '[bell] done' /tmp/wc-uefi.log \
      && fail "UEFI: the bell finished on its own — kill() did not stop it"; } || true
    { grep -aqF '[vanish] STILL HERE' /tmp/wc-uefi.log \
      && fail "UEFI: self-kill returned instead of ending the process"; } || true
    { grep -aq 'sched: deadlock' /tmp/wc-uefi.log \
      && fail "UEFI run deadlocked — a waiter was never woken"; } || true
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
