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
# what this QEMU's monitor calls the space bar. The last three words are the
# process table seen from a keyboard: `reaper` runs a real child, kills it and
# reaps it, `whoami` reports its own pid and tries to kill the shell, and
# `vanish` kills *itself* so the prompt has to report a -9 that it did not
# invent.
send_keys /tmp/wc-biosmon a b ret h e l l o ret \
    l s ret \
    e c h o spc z z z ret \
    c a t spc slash e t c slash i s s u e ret \
    r e a p e r ret \
    w h o a m i ret \
    v a n i s h ret
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
            "shell: reaper ran and returned"; do
    grep -aq "$want" /tmp/wc-bios.log || fail "BIOS log missing: $want"
done

# The shell is a process, and that changes what a typed program can do. Until
# the shell had a slot in the table, `reaper` at the prompt could only be
# refused: spawn answered -1 and wait answered -10, because nothing would ever
# have scheduled the child. Now the same word, typed the same way through a
# real IRQ1 keystroke, runs the same program the boot demo runs — as a CHILD OF
# THE SHELL — and the whole kill chain happens again from the keyboard.
#
# This block is scoped on purpose: `[reaper] ...` lines exist twice by now (the
# boot demo and this run), so everything is read out of the part of the log that
# begins with the shell's own "running 'reaper'" line. Pids are not written
# down anywhere: the shell took pid 1, the demos moved, and a number hardcoded
# here would rot the next time the boot sequence changed.
SEG=$(awk '/shell: running .reaper./{f=1} f' /tmp/wc-bios.log)
RPID=$(printf '%s\n' "$SEG" | grep -aF 'proc: spawned /bin/reaper as pid' | sed 's/.*pid \([0-9-]*\).*/\1/')
KID2=$(printf '%s\n' "$SEG" | grep -aF '[reaper] spawned pid' | sed 's/.*pid \([0-9-]*\).*/\1/')
[ -n "$RPID" ] || fail "the shell never gave the typed reaper a pid of its own"
[ -n "$KID2" ] || fail "the typed reaper reported no child pid"
[ "$KID2" -gt 0 ] || fail "spawn refused the typed reaper a child (pid $KID2)"
[ "$KID2" != "$RPID" ] || fail "the typed reaper spawned itself?"
for want in "proc: spawned /bin/reaper as pid $RPID" \
            "[reaper] spawned pid $KID2" \
            "[bell] ringing pid $KID2" \
            "[reaper] kill $KID2 -> 0" \
            "proc: killed pid $KID2 (SIGKILL) by pid $RPID" \
            "[reaper] reaped pid $KID2 code -9" \
            "shell: reaped pid $RPID code 0"; do
    printf '%s\n' "$SEG" | grep -aqF "$want" \
        || fail "a program typed at the prompt could not run the kill chain: $want"
done
{ printf '%s\n' "$SEG" | grep -aqF '[bell] done' \
  && fail "the bell the prompt started finished on its own — kill() missed it"; } || true
echo '    ok: the prompt is a process — a typed program spawns, kills and is reaped'

# The shell's pid, and what that means for everybody else. The slot is claimed
# in shell_init(), before any program can be started, so the shell is pid 1 and
# every launch is its child. /bin/whoami is the witness from the other side of
# the boundary: getpid() used to answer 0 for a prompt-started program (there
# was no table entry to answer with) and now answers the very pid the kernel
# printed when it spawned it. The same program then tries to kill pid 1 and
# must be refused: EPERM, not ESRCH, because the shell really is in the table.
grep -aqF 'proc: shell is pid 1' /tmp/wc-bios.log \
    || fail 'the shell did not claim pid 1'
WSEG=$(awk "/shell: running .whoami./{f=1} f" /tmp/wc-bios.log)
WPID=$(printf '%s\n' "$WSEG" | grep -aF 'proc: spawned /bin/whoami as pid' | sed 's/.*pid \([0-9-]*\).*/\1/')
[ -n "$WPID" ] || fail 'the shell never spawned /bin/whoami'
for want in "[whoami] pid $WPID" \
            "[whoami] kill 1 -> -1" \
            "proc: refused to kill pid 1 (the shell owns it)" \
            "shell: reaped pid $WPID code 0"; do
    printf '%s\n' "$WSEG" | grep -aqF "$want" \
        || fail "whoami did not report what a real process reports: $want"
done
[ "$WPID" != "1" ] || fail "the shell's own pid was handed to a program it started"
echo '    ok: getpid — a prompt-started program is a process, and pid 1 is unkillable'

# Giving the shell a slot makes a new number reachable at the prompt. A program
# it starts can now be killed, so its exit code can be -SIGKILL, and an unsigned
# printer turns -9 into a twenty-digit nothing: the screen would report an
# ending as a clean exit. `vanish` kills itself, so everything below is the
# shell having to say what the kernel actually witnessed — including the code
# the sweep handed back, which is the one number a kill leaves behind.
VSEG=$(awk "/shell: running .vanish./{f=1} f" /tmp/wc-bios.log)
VPID2=$(printf '%s\n' "$VSEG" | grep -aF 'proc: spawned /bin/vanish as pid' | sed -n 's/.*pid \([0-9-]*\).*/\1/p')
[ -n "$VPID2" ] || fail 'the shell never spawned /bin/vanish'
for want in "[vanish] bye pid $VPID2" \
            "proc: killed pid $VPID2 (SIGKILL) by pid $VPID2" \
            "shell: reaped pid $VPID2 code -9" \
            "shell: vanish exited with -9"; do
    printf '%s\n' "$VSEG" | grep -aqF "$want" \
        || fail "the prompt misreported a process it had seen killed: $want"
done
{ printf '%s\n' "$VSEG" | grep -aqF '[vanish] STILL HERE' \
  && fail "a prompt-started vanish survived its own kill()"; } || true
echo '    ok: -9 arrives whole — a killed program is not reported as an exit 0'

# The scheduler demo runs on its own once the timer is up: two copies of
# /bin/tick, each with its own address space and pid, printing a line and
# yielding in turn. Every marker below is a fixed line; the interesting one
# is the ORDER, proven separately under this — a run-to-completion loop with
# extra steps would still print all eight lines, just as A-1 A-2 A-3 A-done
# B-1 ..., so the interleaving check is the one that actually fails if the
# context switch is a lie.
#
# The pids are read out of the spawn lines instead of written down. The shell
# claims pid 1 now, so every demo process moved, and a literal number in a test
# is a claim about the order of the boot sequence rather than about the
# scheduler — which is precisely the kind of check that rots.
TA_PID=$(grep -aF 'proc: spawned /bin/tick as pid' /tmp/wc-bios.log | sed -n '1s/.*pid \([0-9]*\).*/\1/p')
TB_PID=$(grep -aF 'proc: spawned /bin/tick as pid' /tmp/wc-bios.log | sed -n '2s/.*pid \([0-9]*\).*/\1/p')
[ -n "$TA_PID" ] && [ -n "$TB_PID" ] && [ "$TA_PID" != "$TB_PID" ] \
    || fail "the two tick processes reported no distinct pids"
for want in "sched: building two processes" \
            "proc: spawned /bin/tick as pid $TA_PID" \
            "proc: spawned /bin/tick as pid $TB_PID" \
            "sched: round-robin over the process table" \
            "\\[tick A\\] step 1 pid $TA_PID" "\\[tick B\\] step 1 pid $TB_PID" \
            "\\[tick A\\] step 2 pid $TA_PID" "\\[tick B\\] step 2 pid $TB_PID" \
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
# a line). Their pids come from the spawn lines, exactly as in the tick demo
# above: next_pid keeps counting, and the shell is one of those numbers now.
SA_PID=$(grep -aF 'proc: spawned /bin/spin as pid' /tmp/wc-bios.log | sed -n '1s/.*pid \([0-9]*\).*/\1/p')
SB_PID=$(grep -aF 'proc: spawned /bin/spin as pid' /tmp/wc-bios.log | sed -n '2s/.*pid \([0-9]*\).*/\1/p')
[ -n "$SA_PID" ] && [ -n "$SB_PID" ] && [ "$SA_PID" != "$SB_PID" ] \
    || fail "the two spin processes reported no distinct pids"
for want in "sched: two compute-heavy procs, no yields" \
            "proc: spawned /bin/spin as pid $SA_PID" \
            "proc: spawned /bin/spin as pid $SB_PID" \
            "\\[spin A\\] start pid $SA_PID" "\\[spin B\\] start pid $SB_PID" \
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

# The version the kernel announces is the version the source says. The status
# window, the taskbar and `about` all draw that same macro, so a number a
# release left behind stops being possible: it fails the run instead of
# shipping a screen that describes an older system.
WC_V=$(sed -n 's/.*WC_VERSION *"\([^"]*\)".*/\1/p' src/kernel/version.h)
[ -n "$WC_V" ] || fail 'version.h declares no WC_VERSION to check the boot against'
grep -aqF "boot: windclone $WC_V" /tmp/wc-bios.log \
    || fail 'BIOS boot did not announce the version in src/kernel/version.h'
echo '    ok: version — the kernel says what the source declares'
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
                "boot: windclone $WC_V" \
                "pci: enumeration done" "step: keyboard live" \
                "log ring (most recent):" "step: initrd ok" \
                "initrd: /etc/motd" "step: vfs ok" \
                "vfs: exists + dirent enumeration matches the archive" \
                "step: userland ok (elf64 + ring 3 round trip)" \
                "proc: shell is pid 1" \
                "proc: spawned /bin/tick as pid" \
                "\\[tick A\\] step 1 pid" "\\[tick B\\] step 1 pid" \
                "step: scheduler ok (two processes, round-robin)" \
                "\\[spin A\\] start pid" "\\[spin B\\] start pid" \
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

# 6) the start menu is the archive, not a picture of it. Every program the
#    build puts into /bin must appear as a row (the kernel writes one
#    "wm: menu row <n> <name>" line per entry), and clicking a row must run
#    that program. The expected set comes from the Makefile, so a new program
#    that forgot to register itself in the menu fails the run.
echo '==> start menu (launcher)'
python3 tools/qemu-desktop.py windclone.iso menu || fail 'start menu launch'
MENU_LOG=${WM_LOG:-/tmp/wc-wm.log}
for prog in $(sed -n 's/^USER_BINS := \(.*\)$/\1/p' Makefile | tr ' ' '\n' | sed 's|build/||'); do
    grep -aqE "wm: menu row [0-9]+ $prog\$" "$MENU_LOG" \
        || fail "start menu has no row for /bin/$prog"
done
# nothing may be left out on a screen this size: hidden must be zero, or the
# rows above are only part of /bin and the loop proved nothing.
grep -aqE 'wm: menu rows [0-9]+, hidden 0$' "$MENU_LOG" \
    || fail 'start menu hid programs that fit nowhere'
# A click must start a *process*, not merely a program. menuproc clicks
# /bin/whoami and ties three lines together: the pid the kernel handed out at
# spawn, the pid the program read back through getpid(), and the reap the shell
# owed it afterwards. The keyboard proves the same thing further up, but the
# mouse is the path the start menu actually takes — and a scenario nobody
# invokes is a screenshot of a test, not a test.
python3 tools/qemu-desktop.py windclone.iso menuproc \
    || fail 'the start menu did not launch a real process'

# 7) kernel text console: the "vga: text console live" marker (checked in
#    the BIOS + UEFI logs above) proves the kernel mapped 0xB8000 through
#    its OWN page tables and a text cell written there reads back intact.
#    A composited GRAPHICAL terminal (Limine framebuffer + font) is tracked
#    in docs/ROADMAP.md under Phase 8 (framebuffer graphics).

echo '==> all local smoke tests passed'
