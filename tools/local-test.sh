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
#
# It is a ceiling on a *wall*, and the wall has to hold both firmwares: OVMF
# spends seconds of its own in post before the kernel's first instruction, and
# the two boot checks then run the whole demo sequence inside what is left. The
# v0.1.23 tag build failed there — "UEFI log missing: \[exit\] code 9 pid", a
# marker that had appeared on the same test on the same runner a commit before —
# because the image is a few kilobytes bigger and the runner was a few seconds
# slower than the day the number was picked. 25 seconds was never a fact about
# WindClone; it was a fact about one machine. So the ceiling is now a minute and
# a half, which no honest boot of this image reaches: when it does expire, the
# log will be short of a marker for a reason worth reading, not because the
# runner was having a day.
TIMEOUT=${TIMEOUT:-90}
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
    -device e1000,netdev=n0 -netdev user,id=n0 \
    -serial file:/tmp/wc-bios.log \
    -monitor unix:/tmp/wc-biosmon,server,nowait &
BIOPID=$!
sleep 6
# a/b prove the decoder; the shell then really runs `hello` in ring 3,
# `ls` enumerates the archive through SYS_GETDIRENT, `echo zzz` proves the
# command line reached _start (the word comes back verbatim), and `cat
# /etc/issue` prints bytes that exist nowhere else in this log. `spc` is
# what this QEMU's monitor calls the space bar. The last four words are the
# process table seen from a keyboard: `reaper` runs a real child, kills it and
# reaps it, `whoami` reports its own pid and tries to kill the shell, `vanish`
# kills *itself* so the prompt has to report a -9 that it did not invent, and
# `orphan` is the one that leaves somebody behind: it
# spawns /bin/nest and exits without waiting, so the prompt is the place where a
# child outlives its parent and the process table has to have an answer for that.
send_keys /tmp/wc-biosmon a b ret h e l l o ret \
    l s ret \
    e c h o spc z z z ret \
    c a t spc slash e t c slash i s s u e ret \
    r e a p e r ret \
    w h o a m i ret \
    v a n i s h ret \
    o r p h a n ret \
    h o l d ret
wait $BIOPID 2>/dev/null || true
for want in "step: pmm ok" "paging: self-checks passed" "step: paging ok" \
            "vga: text console live" "kmalloc selftest ok" "step: heap ok" \
            "boot complete" "step: interrupts armed" \
            "irq0: timer interrupt firing" "uptime: 1s" \
            "step: log ring armed" "pci: enumeration done" \
            "pci: host bridge" \
            "e1000: found 82540EM" \
            "e1000: link up, mac 52:54:00:12:34:56" \
            "e1000: rx ring 8 desc" \
            "e1000: tx probe 60 bytes, descriptor done" \
            "step: keyboard live" \
            "keyboard: key decoded end-to-end" "shell: running 'hello'" \
            "log ring (most recent):" \
            "step: initrd ok" "initrd: /etc/motd" \
            "initrd: missing path lookup returns NULL" \
            "gdt: our own table live" "syscall: int 0x80 gate installed" \
            "vfs: descriptor table live" "step: vfs ok" \
            "vfs: read-back, offset, ebadf and enoent checks passed" \
            "vfs: exists + dirent enumeration matches the archive" \
            "tmpfs: writable store live" \
            "vfs: merged enumeration lists the store and the archive" \
            "tmpfs: write, read-back, eexist, eperm and stale-fd checks passed" \
            "elf64, entry" "hello from ring 3" \
            "step: userland ok (elf64 + ring 3 round trip)" \
            "\\[motd\\] segments: .data marker=0x5a5a .bss counter=0" \
            "\\[motd\\] closed. bytes read: " \
            "\\[motd\\] read after close returned -9" \
            "gfx: framebuffer" "gfx: back buffer" "gfx: desktop composited" \
            "mouse: 8042 aux channel live" "mouse: first packet decoded" \
            "shell: hello ran and returned" \
            "shell: running 'ls'" "\\[ls\\] " "entries, " \
            "shell: ls ran and returned" \
            "shell: running 'echo'" "zzz" \
            "shell: echo ran and returned" \
            "shell: running 'cat'" "0.0.4 - initrd live" \
            "shell: cat ran and returned" "shell: cat exited with 0" \
            "shell: running 'reaper'" \
            "shell: reaper ran and returned" \
            "proc: a parent that dies before its child does" \
            "step: adoption ok (an orphan is re-parented and its exit code comes back)" \
            "adopted by pid" \
            "shell: running 'orphan'" \
            "shell: orphan ran and returned" \
            "\\[orphan\\] leaving now, I will not wait" \
            "\\[nest\\] exiting pid"; do
    grep -aq "$want" /tmp/wc-bios.log || fail "BIOS log missing: $want"
done

# v0.1.12: the compositor draws into a RAM back buffer and pushes a frame in one
# flip. The "gfx: back buffer" line above already proves the buffer exists (a
# machine that could not allocate one says so and never ships silently); this
# proves the *first* frame really flipped the whole screen, because there is no
# prior content to update. The size is read from the kernel's own banner and the
# area from its own flip line - neither number is written down in this test.
GBOX=$(grep -aoE 'gfx: desktop composited [0-9]+x[0-9]+' /tmp/wc-bios.log | head -1)
GW=$(printf '%s' "$GBOX" | sed -n 's/.*composited \([0-9]*\)x.*/\1/p')
GH=$(printf '%s' "$GBOX" | sed -n 's/.*composited [0-9]*x\([0-9]*\).*/\1/p')
GFIRST=$(grep -aoE 'gfx: flip [0-9]+ rects, [0-9]+ px' /tmp/wc-bios.log | head -1)
GN=$(printf '%s' "$GFIRST" | sed -n 's/gfx: flip \([0-9]*\) rects.*/\1/p')
GA=$(printf '%s' "$GFIRST" | sed -n 's/.*, \([0-9]*\) px/\1/p')
[ -n "$GW" ] && [ -n "$GH" ] \
    || fail 'BIOS boot gave no composited size to check the flip against'
[ "$GN" = "1" ] && [ "$GA" = "$((GW * GH))" ] \
    || fail "the boot flip was not the whole screen: ${GN:-?} rects, ${GA:-?} px (want 1, $((GW * GH)))"
echo '    ok: back buffer + a whole-screen boot flip'

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

# A child outlives its parent, and four numbers have to agree about what the
# process table did about that. `orphan` spawns /bin/nest and exits without
# waiting: before this release the survivor kept the pid of a dead process in its
# `parent` field, so no wait() could ever name it and its exit code 7 was written
# into a slot nobody would read — the sweep freed the memory and the relationship
# was forgotten. Now the adopter (the shell, pid 1) is handed the child, the
# kernel says so, the child reads the new parent back through the twelfth
# syscall, and the code comes out of the shell.
#
# Nothing here is hardcoded: the shell's pid comes from the line that claims it,
# the two program pids from the spawn lines inside this run, and every check reads
# one of those numbers back out of a *different* witness. The order check is the
# one that makes it an adoption rather than an accident — the re-parenting has to
# land before the child's own ending, or nothing ever outlived anything.
OSEG=$(awk "/shell: running .orphan./{f=1} f" /tmp/wc-bios.log)
SHELL_PID=$(grep -aF 'proc: shell is pid' /tmp/wc-bios.log | sed -n '1s/.*pid \([0-9]*\).*/\1/p')
OPID=$(printf '%s\n' "$OSEG" | grep -aF 'proc: spawned /bin/orphan as pid' | sed -n 's/.*pid \([0-9-]*\).*/\1/p')
NPID=$(printf '%s\n' "$OSEG" | grep -aF 'proc: spawned /bin/nest as pid' | sed -n 's/.*pid \([0-9-]*\).*/\1/p')
[ -n "$SHELL_PID" ] || fail 'the shell never said which pid it claimed'
[ -n "$OPID" ] && [ -n "$NPID" ] || fail 'the typed orphan produced no spawn lines'
[ "$OPID" != "$NPID" ] || fail 'orphan spawned itself as its own child'
[ "$OPID" != "$SHELL_PID" ] || fail 'the shell started a program under its own pid'
for want in "[orphan] pid $OPID ppid $SHELL_PID child $NPID" \
            "proc: pid $NPID adopted by pid $SHELL_PID (parent $OPID is gone)" \
            "shell: reaped pid $OPID code 0" \
            "shell: reaped pid $NPID code 7" \
            "shell: orphan exited with 0"; do
    printf '%s\n' "$OSEG" | grep -aqF "$want" \
        || fail "a child that outlived its parent was not accounted for: $want"
done
# The adopted process's side of the same fact. Which round the adoption landed in
# is scheduling luck — a tick may hand the CPU to nest before or after the parent's
# exit — so the claim stays "some round reported the adopter's pid", not "round 2
# did". A getppid() that never changed would mean the kernel wrote a line about a
# re-parenting the table did not actually perform.
printf '%s\n' "$OSEG" | grep -aF '[nest] round' \
    | grep -aqF "pid $NPID ppid $SHELL_PID" \
    || fail 'the adopted process never read its new parent back through getppid()'
ADOPT_LN=$(printf '%s\n' "$OSEG" | grep -anF "proc: pid $NPID adopted by pid $SHELL_PID" | head -1 | cut -d: -f1)
EXIT_LN=$(printf '%s\n' "$OSEG" | grep -anF "[nest] exiting pid $NPID" | head -1 | cut -d: -f1)
[ -n "$ADOPT_LN" ] && [ -n "$EXIT_LN" ] \
    || fail 'adoption left no order to check — the re-parenting line or the child ending is missing'
[ "$ADOPT_LN" -lt "$EXIT_LN" ] \
    || fail 'the child ended before its parent was placed — nobody outlived anybody'
{ printf '%s\n' "$OSEG" | grep -aqF 'sched: deadlock' \
  && fail "the adoption run deadlocked — an orphan was never scheduled"; } || true
echo '    ok: adoption — a child outlives its parent and the adopter collects its exit code'

# A launch that is *live*, not an excursion — the whole point of v0.1.10. The
# programs above are short: they finish inside one visit, so the old synchronous
# markers still describe them (proc_reap_live prints the very same "ran and
# returned" / "exited with" lines, late instead of at launch). `hold` is built to
# run long — twenty rounds, each a yield and a busy stretch — so it stays READY
# across several idle-loop visits while its output streams into the terminal. The
# claim that separates this from the old run-to-completion launch is not that a
# paint happened (the boot paints for free) but that a paint landed *strictly
# between two of hold's own rounds*. Nothing is hardcoded: the pid comes from the
# spawn line, the check is order-free, and the exit code 5 arrives later than the
# "started, pid P (running)" line on the reap.
HSEG=$(awk "/shell: running .hold./{f=1} f" /tmp/wc-bios.log)
HPID=$(printf '%s\n' "$HSEG" | grep -aF 'proc: spawned /bin/hold as pid' | sed -n 's/.*pid \([0-9-]*\).*/\1/p')
[ -n "$HPID" ] || fail 'the shell never gave the live /bin/hold a pid'
# the honest async launch line, before any round has run
printf '%s\n' "$HSEG" | grep -aqF "shell: hold started, pid $HPID (running)" \
    || fail 'the async launch did not report a started-but-still-running pid'
R1=$(printf '%s\n' "$HSEG" | grep -anF "[hold] round" | head -1 | cut -d: -f1)
RN=$(printf '%s\n' "$HSEG" | grep -anF "[hold] round" | tail -1 | cut -d: -f1)
[ -n "$R1" ] && [ -n "$RN" ] && [ "$R1" -lt "$RN" ] \
    || fail '/bin/hold left no run of rounds to interleave a paint into'
PAINT=$(printf '%s\n' "$HSEG" | grep -anF 'wm: frame painted' \
    | awk -F: -v a="$R1" -v b="$RN" '$1>a && $1<b {print $1; exit}')
[ -n "$PAINT" ] \
    || fail 'no repaint landed between two live hold rounds — the screen still froze'
for want in "[hold] round 1 pid $HPID" \
            "[hold] exiting pid $HPID code 5" \
            "shell: hold ran and returned" \
            "shell: hold exited with 5" \
            "shell: reaped pid $HPID code 5"; do
    printf '%s\n' "$HSEG" | grep -aqF "$want" \
        || fail "the live /bin/hold did not reach its async end: $want"
done

# v0.1.11: a launched program's output belongs to *its own* window, not the
# shared terminal, and that window outlives the process. The keyboard run types
# nine programs and, headless, nothing ever clicks a close button — so the six
# window slots fill with finished-but-still-open windows and the later launches
# (vanish, orphan, hold) correctly fall back to the terminal: the documented,
# honest edge of "a launched program gets a window", not "every process does".
# The lifecycle claim is therefore checked against the FIRST program that really
# did get a window, and every marker names the same dynamic pid and name read
# back out of the kernel's own open line — nothing here is hardcoded.
WOPEN=$(grep -aF 'wm: program window opened' /tmp/wc-bios.log | head -1)
WPID=$(printf '%s\n' "$WOPEN" | sed -n 's/.*pid \([0-9]*\).*/\1/p')
WNAME=$(printf '%s\n' "$WOPEN" | sed -n "s/.*opened '\([^']*\)'.*/\1/p")
[ -n "$WPID" ] && [ -n "$WNAME" ] \
    || fail 'not one program in the keyboard run got a window of its own'
grep -aqF "wm: program window opened '$WNAME' pid $WPID" /tmp/wc-bios.log \
    || fail "a launched program opened no window bound to pid $WPID"
grep -aqF "wm: routed output pid $WPID" /tmp/wc-bios.log \
    || fail "$WNAME's output was not routed to its own window (pid $WPID)"
grep -aqF "wm: program window pid $WPID finished code" /tmp/wc-bios.log \
    || fail "$WNAME's window did not record the exit code its process left"
# The window outlives its process: nothing was clicked closed in this run, so no
# closed marker may exist — a finished program's window is dismissed by the user.
grep -aqF "wm: program window closed '$WNAME'" /tmp/wc-bios.log \
    && fail "$WNAME's window closed itself after the process died — it must survive"
# And the overflow is real, not silent: hold is past the six slots, so its output
# falls back to the terminal — which is also why its rounds still interleave with
# repaints in the block above. The router caused the earlier windows; this proves
# the fallback is what happens when there is no window room.
grep -aqF "wm: no window room, 'hold' output to terminal" /tmp/wc-bios.log \
    || fail 'the seventh+ launch did not fall back to the terminal as documented'
echo "    ok: $WNAME ran in a window of its own that outlived the process (pid $WPID)"

{ printf '%s\n' "$HSEG" | grep -aqF 'sched: deadlock' \
  && fail 'the idle-driven visit deadlocked a live process'; } || true
echo '    ok: a launch is live — a running program interleaves with repaints, reaps late'

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
                "tmpfs: writable store live" \
                "tmpfs: write, read-back, eexist, eperm and stale-fd checks passed" \
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
                "proc: a parent that dies before its child does" \
                "adopted by pid" \
                "\\[orphan\\] leaving now, I will not wait" \
                "\\[nest\\] exiting pid" \
                "step: adoption ok (an orphan is re-parented and its exit code comes back)" \
                "gfx: framebuffer" "gfx: desktop composited" \
                "wm: wallpaper cached" "rtc: hardware clock"; do
        grep -aq "$want" /tmp/wc-uefi.log || fail "UEFI log missing: $want"
    done
    UKID=$(grep -aF '[reaper] spawned pid' /tmp/wc-uefi.log | sed 's/.*pid \(-\?[0-9]*\).*/\1/')
    grep -Fq "[reaper] reaped pid $UKID code -9" /tmp/wc-uefi.log \
        || fail "UEFI: wait() did not return the killed child with code -SIGKILL"
    { grep -aqF '[bell] done' /tmp/wc-uefi.log \
      && fail "UEFI: the bell finished on its own — kill() did not stop it"; } || true
    { grep -aqF '[vanish] STILL HERE' /tmp/wc-uefi.log \
      && fail "UEFI: self-kill returned instead of ending the process"; } || true
    # Adoption, on the other firmware. The BIOS run above does the full
    # cross-boundary arithmetic; UEFI only has to prove the same kernel story is
    # not firmware-dependent: the survivor was re-parented by the pid the boot
    # claimed, and the code it exited with was collected by that adopter rather
    # than dropped by the sweep.
    USHELL_PID=$(grep -aF 'proc: shell is pid' /tmp/wc-uefi.log | sed -n '1s/.*pid \([0-9]*\).*/\1/p')
    UNPID=$(grep -aF 'proc: spawned /bin/nest as pid' /tmp/wc-uefi.log | sed -n '1s/.*pid \([0-9-]*\).*/\1/p')
    [ -n "$USHELL_PID" ] && [ -n "$UNPID" ] \
        || fail 'UEFI: no shell pid or no orphan child to follow'
    grep -Fq "proc: pid $UNPID adopted by pid $USHELL_PID" /tmp/wc-uefi.log \
        || fail "UEFI: the kernel never re-parented pid $UNPID"
    grep -Fq "shell: reaped pid $UNPID code 7" /tmp/wc-uefi.log \
        || fail "UEFI: the adopter never collected the orphan's exit code 7"
    printf '%s\n' "$(grep -aF '[nest] round' /tmp/wc-uefi.log)" \
        | grep -qF "pid $UNPID ppid $USHELL_PID" \
        || fail 'UEFI: the adopted process never read its new parent back'
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

# v0.1.14: dragging a border resizes a window. The scenario grows the status
# window from its bottom-right corner (the geometry read from the kernel's own
# `wm: window resized WxH` line, checked to follow the cursor and stay inside
# the desktop), then shrinks it far past the floor and requires it to stop at
# MIN_W x MIN_H — so the min-size clamp is proven, not just that it got smaller.
echo '==> window resizing (drag a border; min size; desktop clamp)'
python3 tools/qemu-desktop.py windclone.iso resize \
    || fail 'a window could not be resized by dragging its border'

# v0.1.16: the pointer changes shape over a draggable border. The scenario hovers
# the status window's left edge, its bottom-right corner, and its body, and
# requires the kernel to log the border mask it is showing (and a plain arrow
# once off the border) — the same geometry a click would resize from.
echo '==> resize cursor (the pointer changes over a border)'
python3 tools/qemu-desktop.py windclone.iso cursor \
    || fail 'the cursor did not change shape over a window border'

# v0.1.17: Aero Snap. Drag the status window's title bar to the left edge and the
# WM tiles it as a half; drag that tiled window to the top-right corner and it
# tiles as a quarter. The scenario compares the rect the kernel logged against
# the work area it computed from its own banner size - the geometry is the claim.
echo '==> window snapping (drag a title bar to an edge: halves, quarters)'
python3 tools/qemu-desktop.py windclone.iso snap \
    || fail 'a window did not snap to a screen edge when dragged by its title bar'

# v0.1.18: the file layer can write. The boot self-check above proves the kernel
# half against its own numbers; this proves the userland half the way a user
# does - real IRQ1 keystrokes at the prompt. One `save` process writes a note and
# exits, a second one (different pid) reads those bytes back, and a third sees
# the name through the merged directory. The scenario compares the two byte
# counts and the two pids, and requires the note's text between the reader's own
# lines: the shell never echoes a typed command to the serial port, so nothing
# but the store can have put it there.
echo '==> writable files (write a note in one process, read it in another)'
python3 tools/qemu-desktop.py windclone.iso filewrite \
    || fail 'a program could not write a file another program could read'

# v0.1.19: the file layer can hand the shell instructions. /bin/save writes two
# command lines into /tmp/demo.bat, `bat demo` reads them back and runs them
# through the same dispatcher a typed line goes through — the log tells them
# apart by the origin word, so `shell: running 'echo' from a batch file` can only
# have come from the file. The scenario also requires the three refusals to be
# answers and not crashes: no such script, a name that looks like a traversal,
# and a script inside a script (whose following line still has to run).
echo '==> batch files (a script a program wrote, the shell obeys)'
python3 tools/qemu-desktop.py windclone.iso batchfile \
    || fail 'the shell could not run a batch file out of the writable store'

# v0.1.20: the keyboard has a second customer. The scenario types a command that
# launches /bin/keys, waits for the kernel to say the program is *asleep* in
# read(0), types a word, and requires the program - not the prompt - to print it
# back with every byte accounted for. Then the program exits and the next word
# typed is a command again, which is the half that a desktop without this would
# lose: a program that dies holding the keyboard leaves a screen you can look at
# but cannot type into.
echo '==> the keyboard reaches a program (read(0) sleeps until a key lands)'
python3 tools/qemu-desktop.py windclone.iso typing \
    || fail 'a program could not read the keyboard, or the prompt could not take it back'

# v0.1.21: those keys make an editor. The scenario writes a known file, opens it
# in /bin/note, and then edits it through real PS/2 events - Ctrl+E to the end of
# the line, a word, a Backspace that takes it away, Enter that breaks the line, a
# second line, Ctrl+S. It asserts the caret position and byte count the editor's
# own frame reports after every one of those keys, and finishes by having a third
# process read the file back and compare it, byte for byte, with the text that
# only ever entered through the keyboard.
echo '==> a text editor (open, edit, save a file in its own window)'
python3 tools/qemu-desktop.py windclone.iso editing \
    || fail 'the editor could not open, edit or save a file, or the save did not persist'

# v0.1.22: colour in a program window, and a code viewer that uses it. The
# scenario opens a source file that ships in the initrd, requires the kernel to
# report that it consumed a colour escape, and then looks for the escape on the
# wire in front of the specific words it names - a keyword before `include`, a
# type before `int`, a number before `42`, a string before a quote. Those tokens
# live on the second, third and fourth screens, so the three page-downs that have
# to deliver them are what proves the whole file was classified and not just the
# rows that happened to be visible when it opened.
echo '==> a syntax-highlighting code viewer (colour in a program window)'
python3 tools/qemu-desktop.py windclone.iso highlight \
    || fail 'the viewer could not open, colour or scroll a source file'

# v0.1.23: the viewer's classifier and the editor's buffer are one program. The
# scenario has /bin/copy move a real source file out of the boot archive into the
# writable store, opens it as `work.c` (the dot is what makes it a file and not a
# note), requires the editor to paint the same colours the viewer paints, types
# one character, saves, and then digests the file a *second* process reads back.
# A digest over the bytes is the proof a terminal cannot give: a program window is
# 62 columns wide and would silently clip a source line to fit.
echo '==> a code editor that writes back (archive -> store -> editor -> store)'
python3 tools/qemu-desktop.py windclone.iso editcode \
    || fail 'the editor could not open, colour, edit or save a real source file'

# ... and the half that has to say no. /src/big.c is bigger than a document
# buffer, so the editor loads part of it, calls it truncated on the row a person
# reads first, refuses Ctrl+S, quits with unsaved bytes and exit code 7 - and the
# file it refused to write still digests to exactly what it was before it was
# opened. An editor that saved half a file would report a clean write.
echo '==> an editor that refuses to save half a file (truncation guard)'
python3 tools/qemu-desktop.py windclone.iso edittrunc \
    || fail 'the editor saved a truncated document, or would not admit it was one'

# v0.1.24: the desktop's skin, and a clock that tells the truth. The wallpaper is
# painted once into a RAM surface of exactly the composited size and cached (the
# scenario checks that size against the boot banner, so the number is the
# kernel's, not the test's), and the taskbar tray shows the CMOS chip's decoded
# time - not the "12:00" the chrome has carried since it was drawn. Both happen
# before the first mouse packet, so this scenario clicks nothing: the boot log is
# the whole proof. The rounded corners and acrylic bar it ships with are paint,
# and paint is not asserted here - only the two code paths that make them cheap
# and honest are.
echo '==> the desktop skin (cached wallpaper + hardware clock)'
python3 tools/qemu-desktop.py windclone.iso chrome \
    || fail 'the wallpaper was not cached at the desktop size, or the clock was not read from the chip'

# v0.1.26: the card listens. The boot self-check above already proved the e1000
# can send; this is the other half. From the idle loop the ARP client broadcasts
# who-has 10.0.2.2, and QEMU's user-mode network (SLIRP) answers - a reply the
# driver can only parse if it truly landed in the DMA receive ring. The scenario
# waits for the kernel to name the gateway's MAC and rejects two cheats: an
# all-zero address (parsed from an empty buffer) and our own NIC's address (an
# echo of the request, not a reply from another host). The exact SLIRP address is
# deliberately not hard-coded - the claim is that a real reply came back, and the
# emulator's own answer is the witness.
echo '==> the network receives (arp resolve against the emulator subnet)'
python3 tools/qemu-desktop.py windclone.iso net \
    || fail 'the card could not receive: the arp reply never came back through the rx ring'

# v0.1.30: the fetched page is a package. The boot-time TCP/HTTP GET now writes
# the response body into the writable store as /net/fetched, so a byte that came
# off the network becomes a real file. /bin/pkg opens it through the same VFS the
# editor and copier use and paints it in its own window. The scenario recomputes
# the served page's FNV-1a and requires the rendered digest to match: the count is
# the kernel's, the bytes are proven identical, and nothing here trusts a green
# line that a mis-parsed or truncated response could also print.
echo '==> a fetched page becomes a package a program opens (http -> store -> window)'
python3 tools/qemu-desktop.py windclone.iso pkg \
    || fail 'the fetched page was not stored, or /bin/pkg could not render it identically'

# v0.1.31: the machine navigates on request. `fetch /second.html` at the prompt
# re-arms the same TCP client on the boot's lease, the body of the *second* page
# replaces the first in the store, and /bin/browser paints it under the address
# the kernel recorded - read back over syscall 14 by a ring-3 program that has
# no other way to know it. The scenario digests the rendered page against the
# bytes it served and requires the bar to name /second.html, so a window that
# quietly kept the boot page, or a bar echoing the keyboard, both fail the run.
echo '==> a browser that navigates (fetch -> tcp -> store -> address bar + page)'
python3 tools/qemu-desktop.py windclone.iso browser \
    || fail 'the shell could not navigate, or the browser window did not show the page it fetched'

# v0.1.32: cmd's file verbs over the writable store. copy (a program) puts a
# real archive file into /tmp, then DIR counts it, REN moves its name while the
# entry count proves the bytes stayed, and DEL frees the slot - with the count
# proving the file is really gone rather than merely unlisted. Both mutations
# answer a separate refusal for an archive path, because the initrd is
# read-only by contract and a DEL that ate /bin/hello would be a program
# deleted from a store that never held it.
echo '==> file verbs on the store (copy -> dir -> ren -> del, archive refused)'
python3 tools/qemu-desktop.py windclone.iso fileops \
    || fail 'dir/ren/del did not behave over the writable store, or the archive was not read-only'

# v0.1.33: a real ZIP archive unpacked. /bin/unzip reads the committed specimen
# whole, extracts every STORED member byte for byte (its own digest compared to
# fnv1a over the bytes the archive holds), refuses the DEFLATED member instead
# of writing corrupt bytes, and the result is an ordinary store file - copied
# back out by a second program and deleted through the v0.1.32 verbs, which only
# work because the bytes landed in the writable store, not the read-only archive
# they came from.
echo '==> a zip archive unpacked into the store (stored extracted, deflated refused)'
python3 tools/qemu-desktop.py windclone.iso unzip \
    || fail 'the archive did not unpack into the store, or a deflated member was not refused'

# v0.1.34: a real bitmap painted as pixels. /bin/imgview decodes the committed
# 24-bit BMP (little-endian header, four-byte row stride, bottom-up rows, BGR to
# RGB) and hands the exact top-down byte stream to the kernel's new raster channel;
# the test decodes the same file itself and requires the viewer's digest to equal
# fnv1a over those bytes, so "an image showed up" means every pixel landed in the
# right order and not that a count appeared.
echo '==> a bitmap decoded and blitted into a window (real pixels, verified bytes)'
python3 tools/qemu-desktop.py windclone.iso img \
    || fail 'the bitmap did not decode to the pixels the file holds, or the window never took them'

# v0.1.12: the double buffer + damage claim, measured. The scenario reads the
# kernel's own "gfx: flip <n> rects, <area> px" lines and requires three things:
# the boot frame flipped the whole screen, a pure mouse move flipped only a
# cursor-sized band (area under an eighth of the pixels), and a click on a window
# body flipped everything again - so the smaller blit is real damage tracking and
# not a broken repaint.
echo '==> double buffer + damage (mouse move repaints a band, not the screen)'
python3 tools/qemu-desktop.py windclone.iso damage \
    || fail 'a mouse move did not repaint only a small region of the screen'

# v0.1.13: the same damage idea applied to a *running program*. Launch /bin/hold
# from the menu and require its output to repaint only its own window: a
# sub-screen flip landing strictly between two of hold's rounds. On v0.1.12 every
# such frame was the whole desktop, so this is the exact claim of the release.
echo '==> a running program repaints its own window, not the screen'
python3 tools/qemu-desktop.py windclone.iso progdamage \
    || fail 'a running program still repainted the whole screen'

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

# The mouse path to the same live claim: click /bin/hold, and the desktop keeps
# painting while it runs. The scenario reads the hold row's index and the pid
# from the kernel's own lines and requires a repaint strictly between two of
# hold's rounds — the proof the screen no longer freezes behind a running program.
echo '==> live process in a window (async launch, mouse)'
python3 tools/qemu-desktop.py windclone.iso live \
    || fail 'a menu-launched program did not keep the desktop painting'

# v0.1.11: the mouse path to a window that is *the program's own*. Click /bin/hold,
# read its pid, and require the four claims to name that same pid — a window opened
# for it, its output routed there (not the terminal), the window finalised with its
# exit code after the process died, and it still open until the close click. A
# program whose output never reaches the terminal is the point of the release.
echo '==> a program window of its own (routed output, outlives the process)'
python3 tools/qemu-desktop.py windclone.iso progwin \
    || fail 'a launched program did not run in a window that outlived its process'

# 7) kernel text console: the "vga: text console live" marker (checked in
#    the BIOS + UEFI logs above) proves the kernel mapped 0xB8000 through
#    its OWN page tables and a text cell written there reads back intact.
#    A composited GRAPHICAL terminal (Limine framebuffer + font) is tracked
#    in docs/ROADMAP.md under Phase 8 (framebuffer graphics).

echo '==> all local smoke tests passed'
