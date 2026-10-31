#!/usr/bin/env python3
"""Drive the WindClone desktop inside QEMU and check what the kernel said.

Everything here is a real PS/2 event through the emulated 8042: `mouse_move`
becomes a movement packet on IRQ12, `mouse_button` a button packet on the
same line. There is no in-kernel shortcut and no test hook — the window
manager is exercised exactly the way a mouse exercises it.

    qemu-desktop.py <iso> <scenario> [screendump.png]

Scenarios (each one asserts the serial markers the kernel prints):
    focus      click a window body, then drag it by its title bar
    minimize   minimise the terminal, restore it from the taskbar
    maxmax     maximise the terminal, then restore it
    close      close the status window, reopen it from a desktop icon
    start      DEMARRER opens the menu, its action row shows the desktop,
               and a click outside the panel dismisses it
    menu       open the menu, find the row the kernel listed for /bin/hello,
               click it, and check the program really ran
    menushot   open the menu and leave it open — this one exists to be
               screenshotted (docs/screenshots/start-menu.png)
    menuproc   click /bin/whoami and follow its pid through the log: the pid
               the kernel gave at spawn, the one the program read back with
               getpid(), and the reap the shell owed it
    progwin    click /bin/hold and prove its output belongs to *its own*
               window, the window outlives the process (it finalises with the
               exit code but stays open), and closing it is the click that
               finally dismisses it
    damage     prove the double buffer + damage claim: the boot composite flips
               the whole screen, a pure mouse move on a desktop that has gone
               quiet flips only a cursor-sized band (a fraction of the pixels),
               and a click on a window body is structural and flips the whole
               screen again
    progdamage prove a *running program* repaints only its own window: launch
               /bin/hold from the menu, then require a whole-screen-sized
               program's output to land strictly between two of its own rounds
               as a sub-screen flip - on v0.1.12 every such frame was the whole
               desktop
    resize     prove a window resizes by dragging its border: raise the status
               window, grow it from its bottom-right corner (width and height
               follow the cursor and stay inside the desktop), shrink it far
               past the floor and require it to stop at the minimum size, then
               grab the top edge (above the title-bar buttons) and grow it back
               taller without touching its width
    cursor     prove the resize cursor: hover the status window's left edge, its
               bottom-right corner, and its body, and require the kernel to log
               the border mask it is showing (a plain arrow off the border)
    snap       prove Aero Snap: drag a window's title bar to the left edge of the
               screen and it tiles as a half, then drag the tiled window to the
               top-right corner and it tiles as a quarter. The kernel logs the
               zone and the rect it applied, and the test asserts that geometry
    filewrite  prove the writable file layer end-to-end from the keyboard: type
               `save write demo <note>` so one process puts bytes in /tmp, then
               `save read demo` so a *different* pid opens the same path and
               prints the note back. The test checks the writer's byte count,
               the reader's, that the two pids differ, and that the note's own
               text sits between the reader's two lines (the shell never echoes
               a typed command to the serial port, so nothing else put it there),
               then `save list` so a third process sees the name and size through
               SYS_GETDIRENT's merged enumeration
    batchfile  prove the shell obeys a script that a *program* wrote: type
               `save script demo about nl echo hi` so /bin/save puts two command
               lines in /tmp/demo.bat, then `bat demo` and require the kernel to
               log the same byte count, run both lines, and name the batch file as
               the origin of the program it launched (`shell: running 'echo' from
               a batch file` - a typed line says "from the prompt"). Then the
               three refusals: a missing script, a name that looks like a path
               traversal, and a script that calls another script.
    batch2     prove the cmd.exe semantics a copied script leans on: `@` hides
               one line's echo, `ECHO OFF`/`ECHO ON` flip the script's echo and
               a bare `ECHO` reports the state, `set x=hi` defines a session
               variable a later `echo %x%` expands to (the resolved exec row must
               read `echo hi` and never a literal `echo %x%`), a bare `SET` lists the
               environment, and `rem` is a comment like `;`. Every proof line is
               `shell: batch exec '<resolved line>'` - the serial transcript the
               echo setting cannot hide.
    promptset  prove the cmd built-ins a person types at the keyboard, not in a
               script: `set pz=hi` defines a session variable the kernel folds to
               `PZ`, a bare `set` lists the environment back, `set wrong` (no
               `=`) is refused with a line rather than guessed, and `type
               /tmp/hi.bat` prints a file /bin/save wrote to the store - the
               exact bytes landing on the transcript, and a missing name saying
               so. Each claim is a kernel-written line the screen echo cannot
               fake (`shell: prompt set ...`, `shell: type ... (N bytes)`).
    promptvar  prove %VAR% expansion on a *typed* line, the way cmd expands a
               command before it runs: after `set pz=bonjour`, a typed
               `echo %pz%` resolves to `echo bonjour` - the kernel writes
               `shell: prompt exec 'echo bonjour'` (the resolved form, only
               because %pz% substituted) and then launches the program with
               `shell: running 'echo' from the prompt`. An undefined name stays
               LITERAL, so a typed `echo %nope%` writes NO exec row (a count
               check proves it) - the exact choice that keeps batch2's typed
               `save ... echo %x%` intact.
    caret      prove cmd's caret escape: `^` consumes itself and emits the next
               character literally, and a `^%` cannot start a %VAR% reference.
               After `set pz=bonjour`, a typed `echo ^%pz%` resolves to
               `echo %pz%` (caret gone, percent literal, no expansion) and
               `echo a^b` resolves to `echo ab`; each is the kernel's own
               `shell: prompt exec '<resolved>'` row, then the program launch.
    typing     prove the keyboard reaches a *program*: type `keys` so /bin/keys
               launches and blocks in read(0) (the kernel says it is asleep until
               a keystroke wakes it), then type a word and require the program to
               print it back exactly, with every byte it delivered summing to the
               keys typed. The same typing must NOT reach the prompt, and when the
               program exits the console has to take the keyboard back and run the
               next word it is given — no click in between.
    editing    prove a text editor: type `save write demo hello world` so one
               process puts a known file in /tmp, then `note demo` so /bin/note
               opens it in its own window, repaints that window as a *screen*
               (a form feed, not another line of scrollback), and takes the
               keyboard. Then edit it with real PS/2 events - Ctrl+E to the end
               of the line, a word typed, a Backspace that removes it again,
               Enter that breaks the line, a second line - and save with Ctrl+S.
               The byte count the store reports is computed from what the test
               typed, and a *different* process (`save read demo`) has to read
               exactly that many bytes back, with the words typed only into the
               editor present between its two lines.
    highlight  prove a syntax-highlighting code viewer: type `view /src/hello.c`
               so /bin/view opens a source file that ships in the initrd, decides
               what each token is, and paints the answer with the in-band colour
               escape (FS + a palette digit). The kernel says it consumed one -
               `wm: colour pid N` comes out of the parser and not out of a byte
               count - and the escape has to be found on the wire in front of the
               words it names: the comment ink on the file's own first row, a
               keyword in front of `include`, a number in front of `42`, a type
               in front of `int`, the string ink in front of a quote. Three
               Ctrl+F page-downs have to deliver them, because they are on the
               second, third and fourth screens and not the first, and the status
               row at the last page has to agree with the line count the program
               reported when it opened.
    editcode   prove one program does both halves: /bin/copy puts a real source
               file from the boot archive into the writable store, /bin/note
               opens it under the name typed (`work.c`, with the dot), paints it
               with the same classifier the viewer paints, edits it with a real
               PS/2 keystroke, and writes it back. A digest over the bytes
               (FNV-1a, printed by both programs) is compared three ways: the
               file as the test reads it off the disk, the document as the editor
               loaded it, and the file as a *second* copy process reads it out of
               the store after the save. Every byte that round-tripped is a byte
               the kernel moved, and the offset the editor's own ruler reports is
               what says where the typed character landed.
    edittrunc  prove the refusal. /src/big.c is a specimen deliberately bigger
               than a document buffer and smaller than the store, so the editor
               loads part of it and says so. One Ctrl+D makes the document dirty;
               Ctrl+S then has to be *refused* rather than quietly writing half a
               file back over a whole one, Ctrl+Q has to end with unsaved bytes
               and exit 7, and a third process copying the file afterwards has to
               report the digest it had before the editor ever opened it.
    chrome     prove the desktop's skin, from the boot log rather than the
               pixels: the wallpaper is painted once into a surface of exactly
               the composited size and cached (`wm: wallpaper cached WxH`, the
               size checked against the banner the caller parsed), and the
               taskbar clock is the CMOS chip's own decoded time (`rtc: hardware
               clock HH:MM:SS` with a real, in-range hour/minute/second, plus a
               plausible date) and not the "12:00" string that shipped for the
               six releases before it. A chip that answers 0xFF to every port is
               refused by the driver, so the very presence of a decoded time is
               the proof a live clock was read. No clicks: both are painted
               before the first mouse packet, which is the whole claim.

Exit status 0 = every step did what the serial log says it did.
SPDX-License-Identifier: MIT
"""
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

MON = os.environ.get("WM_MON", "/tmp/wc-wmmon")
LOG = os.environ.get("WM_LOG", "/tmp/wc-wm.log")
# A second control channel, used only for key chords; see Qmp below.
QMP = os.environ.get("WM_QMP", "/tmp/wc-wmqmp")
# The tree the initrd was built out of, so a test can hold the specimen in one
# hand and the guest's word about it in the other. Read from disk rather than
# transcribed: a file that gained a byte moves the expectation with it, and the
# spelling the checkout happens to use (CRLF on Windows, LF on Linux) is the
# spelling the initrd carries, because mkinitrd.sh tars rootfs/ as it finds it.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Geometry as layout() computes it for the Limine mode QEMU gives us; the
# real size is read back from the serial banner and everything recomputed.
STATUS_W, STATUS_H = 400, 300
TERM_W, TERM_H = 600, 380
TASKBAR_H, TITLE_H = 44, 34
BTN_W, BTN_DY, BTN_H = 26, 8, 18
TASK_X, TASK_W, TASK_GAP = 132, 168, 6
START_X, START_W = 12, 110
ICON_X, ICON_W = 28, 52
MENU_X, MENU_W = START_X, 236
MENU_HEADER_H, MENU_ITEM_H, MENU_FOOTER_H, MENU_GAP = 30, 22, 26, 6
# A program window as wm_open_program() cascades it: the first one lands at
# (150,56) and is PROG_W wide (clamped to W-40), so its close button is where
# title_button() says for a window at that rect.
PROG_W, PROG_H = 500, 240
PROG_X0, PROG_Y0, PROG_STEP_X, PROG_STEP_Y = 150, 56, 24, 22
# v0.1.14 window resizing. A border grab counts within RESIZE_EDGE pixels of
# an edge, and a shrink stops at MIN_W x MIN_H. These mirror the kernel's
# RESIZE_EDGE / MIN_W / MIN_H so the scenario can drive and assert against them.
RESIZE_EDGE, MIN_W, MIN_H = 5, 200, 120
# Characters a scenario may type that are not letters or digits. Most map to a
# single QEMU monitor key name the kernel's scancode table (keyboard.c map_plain)
# has to agree with: 0x33 ',', 0x34 '.', 0x35 '/' are mapped there, so a
# `sendkey slash` really does land as a '/' in the shell's input buffer.
#
# An underscore is not one of those. There is no bare make code for it: on the
# PS/2 set it is Shift + Minus, and keyboard.c only answers map_shift[0x0C] = '_'
# while shift is held. `sendkey underscore` is a name QEMU accepts but delivers no
# usable scancode for, so a wheel member named demo/__init__.py typed back at the
# prompt arrived as demo/init.py - a silently dropped key, the exact failure the
# docstring on type_text warns about. So '_' is spelled as the chord the kernel
# actually decodes, and (having a dash) it goes through QMP like every other
# chord: press shift, press minus, release both.
#
# '=' and '%' follow the same rule, because the batch-semantics scenario types
# SET X=hi and echo %X% at the prompt. The kernel already decodes both:
# map_plain[0x0D] = '=' and map_shift[0x06] = '%'. QEMU's own names ("equal" and
# the shift-5 chord) are proven QMP qcodes, so both bypass HMP: "equal" is not
# in HMP_OK, and "shift-5" carries a dash. '@' (map_shift[0x03], the `@echo off`
# of the same scenario) joins them, and ';' maps to QEMU's own qcode name: the
# raw character is not a qcode ('QEMU refused the semicolon as ";"'), "semicolon"
# is, and it is not in HMP_OK either way, so it goes through the channel that
# answers. map_plain[0x27] = ';' decodes it on the kernel side.
KEYMAP = {" ": "spc", "\n": "ret", "/": "slash", ".": "dot", "-": "minus",
          "_": "shift-minus", "=": "equal", "%": "shift-5", "^": "shift-6",
          "@": "shift-2", ";": "semicolon"}
# Key names the *human* monitor has been proved to deliver: the letters and
# digits every scenario has always typed, plus the unshifted spellings KEYMAP
# maps to. Anything else goes through QMP, because `sendkey` accepts an unknown
# name, replies nothing, and sends no scancode - a Backspace spelled "bspace", or
# an underscore, looked exactly like a guest that had lost the key. QMP answers,
# so a name QEMU does not know fails the run at the keystroke instead of at the
# assertion 25 seconds later.
HMP_OK = set("abcdefghijklmnopqrstuvwxyz0123456789") | {
    "spc", "ret", "slash", "dot", "minus"}


# The page the harness serves for the network scenarios. It is a fixed, known
# body so the pkg test can recompute its FNV-1a here and require the kernel's
# whole fetch path (TCP -> the store -> the VFS -> /bin/pkg's window) to hand
# back the same bytes it served, not merely a plausible count.
PAGE_BODY = "<html><body>windclone-first-fetch</body></html>\n"

# The page the browser scenario navigates to *after* boot. The boot fetch is
# fixed ("/"), so the second file is what proves the address bar and the stored
# body followed the request rather than the first page: different bytes, a
# different digest, and an attribute for the lexer to colour.
SECOND_BODY = ('<html><body><p class="second">windclone-second-page</p>'
               '</body></html>\n')


def count_serial(pattern):
    """How many times the serial log already says `pattern`. A marker that a
    later step also produces cannot be proven with `wait_serial` alone - re.search
    is happy with the *first* hit and would pass on a stale line - so scenarios
    that expect an event to happen *again* count the hits instead."""
    return len(re.findall(pattern, read_log()))


def wait_serial_after(mark, pattern, timeout=25.0):
    """Wait for `pattern` to match somewhere past byte offset `mark`, and return
    (new_mark, match).

    An editor repaints the *same* status row over and over, so the plain
    wait_serial would answer with a frame from before the keystroke and prove
    nothing at all. Offsetting by how far the log had already got makes the
    assertion about the frame that came after the key, which is the only one
    that says anything about the edit.

    The new mark is the end of the match, not the end of the log. A frame goes
    out in a single write(), so two rows of the same frame can sit one after the
    other; skipping to the end of the file would put the next assertion past the
    very frame it is meant to read and it would time out waiting for a repaint
    that already happened."""
    rx = re.compile(pattern)
    deadline = time.time() + timeout
    while time.time() < deadline:
        text = read_log()
        m = rx.search(text[mark:])
        if m:
            return mark + m.end(), m
        time.sleep(0.25)
    raise SystemExit("serial never said %s after byte %d (%s)"
                     % (pattern, mark, LOG))


def last_resized(marker="wm: window resized "):
    """Return (w, h) from the LAST `wm: window resized WxH` line, or None. The
    resize scenario produces two of them (a grow, then a shrink to the floor),
    so the test reads the most recent one rather than matching the first."""
    try:
        with open(LOG, errors="replace") as fh:
            text = fh.read()
    except OSError:
        return None
    hits = re.findall(r"wm: window resized (\d+)x(\d+)", text)
    if not hits:
        return None
    w, h = hits[-1]
    return (int(w), int(h))


def last_cursor():
    """Return the mask from the LAST `wm: cursor resize <n>` line, or None. The
    kernel logs a border mask only when the hover changes, so reading the last
    one gives the cursor the pointer is showing right now. The numbers are the
    kernel's RS_* bits: LEFT=1, RIGHT=2, TOP=4, BOTTOM=8, 0 = plain arrow."""
    try:
        with open(LOG, errors="replace") as fh:
            text = fh.read()
    except OSError:
        return None
    hits = re.findall(r"wm: cursor resize (\d+)", text)
    if not hits:
        return None
    return int(hits[-1])


def read_log():
    """The whole serial log as text, or "" before the first byte lands. Every
    `last_*` reader above and every cross-line assertion below works from this,
    and none of them invents a number: the kernel's own lines are the input."""
    try:
        with open(LOG, errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def read_log_raw():
    """The serial log with no newline translation, for the one test that counts
    bytes rather than matching a line.

    The kernel's serial driver follows every line feed with a carriage return so
    a real terminal does not staircase down the page, and Python's universal
    newlines read that pair as *two* line feeds. A pattern that fits on one row
    cannot tell the difference - the editor's ruler, a window title, an exit
    status all match either way - but a test that says "the file the reader
    dumped is these exact bytes" would be comparing against a log with an extra
    newline after every one the program wrote."""
    try:
        with open(LOG, newline="", errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def spec(rel):
    """The specimen's own bytes, from the tree the initrd was built out of.
    `rel` is relative to the repo, so "/src/hello.c" in the guest is
    "rootfs/src/hello.c" here - the same file, the same bytes, and the length
    and content the copy program has to report."""
    with open(os.path.join(ROOT, rel), "rb") as fh:
        return fh.read()


def fnv1a(data):
    """FNV-1a over 32 bits, the same one src/user/wcstr.h's wc_fnv1a() runs.

    This is the test keeping its own copy of an algorithm rather than trusting
    the program's, and that is the point: a copier, an editor and a viewer that
    all shared a broken hash would agree with each other and with nothing else.
    The programs print eight hex digits because a 64-bit `unsigned long` still
    has only 32 bits worth of FNV-1a in its low half - every step here is
    truncated the same way, so the two spellings land on the same number."""
    h = 2166136261
    for byte in data:
        h ^= byte
        h = (h * 16777619) & 0xFFFFFFFF
    return "%08x" % h


def last_snap():
    """Return (code, w, h, x, y) from the LAST `wm: snapped <c> <W>x<H> at
    <x>,<y>` line, or None. The kernel logs it only when a drag ends on a screen
    edge, so this reads the tile it actually applied - the geometry, not a
    guess. code is the kernel's SNAP_* enum (1 left, 2 right, 3..6 corners)."""
    try:
        with open(LOG, errors="replace") as fh:
            text = fh.read()
    except OSError:
        return None
    hits = re.findall(r"wm: snapped (\d+) (\d+)x(\d+) at (-?\d+),(-?\d+)", text)
    if not hits:
        return None
    return tuple(int(v) for v in hits[-1])


class Qmp:
    """QEMU's machine-readable monitor, used here for key chords only.

    The human monitor *accepts* `sendkey ctrl-e`, replies nothing, and the guest
    never sees a scancode: the chord is parsed and dropped, and a test that types
    Ctrl+S then waits for the save is waiting for a keystroke that never left the
    emulator. QMP's `send-key` takes the same trip through the 8042 — press the
    modifier, press the key, release both — and answers with a JSON object, so a
    key QEMU refuses becomes a failed test instead of a silent pause.
    """

    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.settimeout(5)
        self.s.connect(path)
        time.sleep(0.2)
        self.reply()                       # the greeting, not a command result
        self.cmd("qmp_capabilities")

    def reply(self):
        """One JSON line, or whatever arrived before the socket went quiet."""
        buf = b""
        while b"\n" not in buf:
            try:
                chunk = self.s.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            buf += chunk
        return buf.decode(errors="replace").strip()

    def cmd(self, name, **args):
        body = {"execute": name}
        if args:
            body["arguments"] = args
        self.s.sendall((json.dumps(body) + "\n").encode())
        time.sleep(0.1)
        out = self.reply()
        if '"error"' in out:
            raise SystemExit("QEMU refused %s: %s" % (name, out[:200]))
        return out

    def chord(self, spec):
        """`ctrl-e` -> [ctrl, e]: the modifier is held for the key, both released."""
        keys = [{"type": "qcode", "data": part} for part in spec.split("-")]
        self.cmd("send-key", keys=keys)

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


class Monitor:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.settimeout(5)
        self.s.connect(path)
        time.sleep(0.2)
        self.s.recv(4096)
        self.qmp = None            # set by main(); chords need it, see key()

    def cmd(self, text):
        self.s.sendall(text.encode() + b"\n")
        time.sleep(0.12)

    def move_rel(self, dx, dy):
        """QEMU clamps one packet to a signed byte, so walk there in steps."""
        while dx or dy:
            sx = max(-100, min(100, dx))
            sy = max(-100, min(100, dy))
            self.cmd("mouse_move %d %d" % (sx, sy))
            dx -= sx
            dy -= sy

    def move_to(self, x, y):
        # Anchor at the top-left corner: QEMU's monitor counts dy positive
        # DOWNWARD (it negates it when it builds the PS/2 packet), so both
        # deltas here are negative to reach (0,0) and clamp there.
        self.move_rel(-1500, -1500)
        self.move_rel(int(x), int(y))

    def click(self, x, y):
        self.move_to(x, y)
        self.cmd("mouse_button 1")
        time.sleep(0.25)
        self.cmd("mouse_button 0")
        time.sleep(0.25)

    def drag(self, x0, y0, dx, dy):
        self.move_to(x0, y0)
        self.cmd("mouse_button 1")
        time.sleep(0.25)
        self.move_rel(dx, dy)
        time.sleep(0.25)
        self.cmd("mouse_button 0")
        time.sleep(0.25)

    def key(self, name):
        """One keystroke on the emulated 8042.

        A name with a dash is a chord (`ctrl-s`), and the human monitor cannot
        send one: it accepts the command and the guest sees no scancode at all.
        Those go through QMP instead, which answers, so a key QEMU will not send
        fails the run rather than hanging it. The names in HMP_OK stay on the
        socket every scenario has always driven, because that is the path they
        proved; an unproven name goes to QMP for the same reason a chord does."""
        if "-" in name or name not in HMP_OK:
            if self.qmp is None:
                raise SystemExit("key %r needs the QMP channel" % name)
            self.qmp.chord(name)
            time.sleep(0.1)
            return
        self.cmd("sendkey %s" % name)

    def type_text(self, text):
        """Type through the emulated 8042: every key is a real IRQ1.

        QEMU's monitor wants key *names*, not characters, and the two only agree
        for letters and digits. The map below covers the punctuation a path or a
        script line needs; a character missing from it would be sent as itself
        and silently dropped, which is why the batch scenario asserts the whole
        name back out of the serial log instead of trusting the typing."""
        for ch in text:
            self.key(KEYMAP.get(ch, ch))
            time.sleep(0.12)

    def shot(self, path):
        self.cmd("screendump %s -f png" % path)
        time.sleep(1.5)

    def close(self):
        try:
            self.s.sendall(b"quit\n")
            self.s.close()
        except OSError:
            pass


def title_button(rect, which):
    """Centre of a title-bar button: 0=minimise, 1=maximise, 2=close.

    The kernel packs them right to left (close against the corner), so the
    button index runs backwards from the right edge.
    """
    x, y, w, _ = rect
    bx = x + w - 4 - BTN_W * (3 - which)
    return bx + BTN_W // 2, y + BTN_DY + BTN_H // 2


def task_button_x(slot, count):
    """Centre x of the `slot`-th taskbar button; buttons follow wins[] order
    for every window that is open, so the terminal is not the first one."""
    return TASK_X + slot * (TASK_W + TASK_GAP) + TASK_W // 2


def wait_serial(pattern, timeout=25.0):
    """Poll the serial file until `pattern` matches. A click that opens the
    menu makes the kernel enumerate /bin before it repaints, and the row a
    test wants to click is only known once those lines exist — so this waits
    for the log, not for a guessed delay."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        text = ""
        try:
            with open(LOG, errors="replace") as fh:
                text = fh.read()
        except OSError:
            pass
        m = re.search(pattern, text)
        if m:
            return m
        time.sleep(0.25)
    raise SystemExit("serial never said %s (%s)" % (pattern, LOG))


FLIP_RE = r"gfx: flip (\d+) rects, (\d+) px"


def wait_flip_after(mark, pred, timeout=15.0):
    """Poll the serial log for a `gfx: flip <n> rects, <area> px` line that
    appears *after* byte offset `mark` and whose (rects, area) satisfies `pred`.
    Damage is a measured claim, not a fixed string, so the test reads the number
    out of the log and compares it - offsetting by `mark` keeps the boot flip and
    earlier frames from satisfying a later step's predicate."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        for n, a in re.findall(FLIP_RE, read_log()[mark:]):
            if pred(int(n), int(a)):
                return (int(n), int(a))
        time.sleep(0.2)
    return None


def wait_quiet(timeout=60.0, still=2.0):
    """The log offset of the moment the compositor stopped flipping, or None if
    it never does. The kernel paints a frame only when something is dirty, so
    "no flip line for `still` seconds" is the definition of a quiet desktop -
    and it is measured on the flip lines, not on the byte count, because the
    idle loop prints a one-hertz `uptime:` heartbeat whatever happens.

    This is a *precondition*, and the damage scenarios need it: the boot's own
    demo programs keep writing into the terminal for seconds after the desktop
    marker the preamble waits for, and every one of those frames repaints the
    window body - the largest rectangle on the screen. A mouse move issued in
    the middle of that is not evidence about mouse moves."""
    deadline = time.time() + timeout
    prev = None
    since = time.time()
    while time.time() < deadline:
        text = read_log()
        n = len(re.findall(FLIP_RE, text))
        if n != prev:
            prev = n
            since = time.time()
        elif time.time() - since >= still:
            return len(text)
        time.sleep(0.2)
    return None


def first_flip_after(mark, timeout=15.0):
    """The first `gfx: flip` line after offset `mark`, or None if none arrives.
    Knowing *which* frame a measurement belongs to is the difference between
    'some flip in the neighbourhood was small' and 'this event caused a small
    flip', so a scenario that quiesced the desktop asks for the first one."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        m = re.search(FLIP_RE, read_log()[mark:])
        if m:
            return (int(m.group(1)), int(m.group(2)))
        time.sleep(0.2)
    return None


def menu_layout(H):
    """(top, rows) of the start menu. The panel is anchored above the taskbar
    and grows upward, and its height depends on how many programs the kernel
    listed — so the geometry comes from the kernel's own count line instead of
    being assumed by the test."""
    rows = int(wait_serial(r"wm: menu rows (\d+), hidden (\d+)").group(1))
    height = MENU_HEADER_H + (rows + 1) * MENU_ITEM_H + MENU_FOOTER_H
    return H - TASKBAR_H - height - MENU_GAP, rows


def menu_row_y(H, index):
    """Centre y of row `index`; `index == rows` is the action row under the
    programs, which is what `index=rows` means when this is called."""
    top, rows = menu_layout(H)
    return top + MENU_HEADER_H + index * MENU_ITEM_H + MENU_ITEM_H // 2


def scenario_actions(sc, mon, W, H):
    tx, ty = W - min(TERM_W, W - 40) - 48, H - TASKBAR_H - TERM_H - 60
    tw = min(TERM_W, W - 40)
    term = (tx, ty, tw, TERM_H)
    sx, sy = 116, 88                              # status window
    status = (sx, sy, STATUS_W, STATUS_H)

    if sc == "chrome":
        # v0.1.24: the desktop's skin, proved from the boot log because these two
        # things are painted before any click and a screenshot is not how this
        # suite proves anything. The wallpaper must have been cached into a
        # surface of exactly the composited size; the tray must be showing the
        # CMOS chip's own decoded time. W and H are the banner the caller parsed,
        # so the size check compares the kernel to itself, not to a number the
        # test wrote down.
        text = read_log()
        m = re.search(r"wm: wallpaper cached (\d+)x(\d+)", text)
        if not m:
            raise SystemExit("the wallpaper was never cached into a surface")
        if (int(m.group(1)), int(m.group(2))) != (W, H):
            raise SystemExit("wallpaper cached %sx%s, not the desktop's %dx%d"
                             % (m.group(1), m.group(2), W, H))
        # A decoded time out of 0x70/0x71. An absent chip answers 0xFF to every
        # register, which the driver turns down (no "hardware clock" line); so a
        # real HH:MM:SS here can only have come from a chip that settled on one
        # value across two reads. This run seeds the CMOS with a known instant
        # (vm clock, base 2026-08-21T05:37:13), so the claim is exact, not merely
        # plausible: the hour the driver decoded must be the hour it was given,
        # and the date must be the date it was given. Only the seconds tick, so
        # the minute may roll once if boot is slow - hence minute is range-checked
        # but the hour and the date are pinned to the seed.
        cm = re.search(r"rtc: hardware clock (\d\d):(\d\d):(\d\d)", text)
        if not cm:
            raise SystemExit("the hardware clock never reported a decoded time")
        hh, mm, ss = (int(cm.group(i)) for i in (1, 2, 3))
        if hh > 23 or mm > 59 or ss > 59:
            raise SystemExit("rtc reported an impossible %02d:%02d:%02d"
                             % (hh, mm, ss))
        if hh != 5:
            raise SystemExit("rtc read hour %d, but the CMOS was seeded with hour 05"
                             % hh)
        dm = re.search(r"rtc: date day (\d+) month (\d+) year (\d+)", text)
        if not dm:
            raise SystemExit("rtc never reported a date")
        if (int(dm.group(1)), int(dm.group(2)), int(dm.group(3))) != (21, 8, 26):
            raise SystemExit("rtc read date %s, not the seeded 21/08/26"
                             % dm.group(0))

    elif sc == "focus":
        # clicking a window body raises it, then the title bar drags it
        mon.click(tx + 60, ty + TITLE_H + 60)
        mon.drag(tx + tw // 2, ty + TITLE_H // 2, 120, 90)
    elif sc == "minimize":
        mon.click(*title_button(term, 0))                       # minimise
        mon.click(task_button_x(1, 2), H - TASKBAR_H + 22)      # terminal button
    elif sc == "maxmax":
        mon.click(*title_button(term, 1))                       # maximise
        mon.click(*title_button((0, 0, W, H - TASKBAR_H), 1))   # restore
    elif sc == "close":
        mon.click(*title_button(status, 2))                     # close status
        mon.click(ICON_X + ICON_W // 2, 28 + 30)                # ORDINATEUR icon
    elif sc == "start":
        btn = (START_X + START_W // 2, H - TASKBAR_H + 22)
        mon.click(*btn)                                  # open the menu
        action = menu_row_y(H, menu_layout(H)[1])        # the action row
        mon.click(MENU_X + MENU_W // 2, action)          # AFFICHER LE BUREAU
        mon.click(*btn)                                  # open it again
        mon.click(MENU_X + MENU_W // 2, action)          # ...and back
        mon.click(*btn)                                  # open a third time
        mon.click(W - 60, 30)                            # click the empty
                                                         # desktop to dismiss
    elif sc == "menu":
        mon.click(START_X + START_W // 2, H - TASKBAR_H + 22)
        # Which row is /bin/hello? The kernel says, one line per program:
        #     wm: menu row 3 hello
        # Reading the index out of the log is what makes this a test of the
        # launcher and not a test of a coordinate the test guessed.
        idx = int(wait_serial(r"wm: menu row (\d+) hello\n").group(1))
        mon.click(MENU_X + MENU_W // 2, menu_row_y(H, idx))
        time.sleep(1.0)
    elif sc == "menushot":
        mon.click(START_X + START_W // 2, H - TASKBAR_H + 22)
        wait_serial(r"wm: start menu opened")   # the panel is up before the
        time.sleep(0.5)                          # screenshot is taken
    elif sc == "menuproc":
        # A click must start a PROCESS, not merely a program: /bin/whoami
        # prints the pid the kernel gave it, so the two lines can be tied
        # together from the log. This is the mouse's version of the claim the
        # keyboard test makes, and it is the one thing a screenshot cannot show.
        mon.click(START_X + START_W // 2, H - TASKBAR_H + 22)
        idx = int(wait_serial(r"wm: menu row (\d+) whoami\n").group(1))
        mon.click(MENU_X + MENU_W // 2, menu_row_y(H, idx))
        m = wait_serial(r"proc: spawned /bin/whoami as pid (\d+)")
        pid = m.group(1)
        wait_serial(r"\[whoami\] pid %s\n" % pid)
        wait_serial(r"shell: reaped pid %s code 0\n" % pid)
    elif sc == "live":
        # A click must start a process that is *still running* when the launch
        # returns, and the desktop must keep painting underneath it. /bin/hold
        # prints a round per yield across several idle-loop visits, so the claim
        # is not "a paint happened" (the boot paints for free) but "a paint
        # landed strictly between two of hold's own rounds" — the screen turned
        # over while the program worked. Row index from the menu's own log line,
        # pid from the spawn line: nothing here is hardcoded, and the ordering
        # check stays order-free like the keyboard one further up the suite.
        mon.click(START_X + START_W // 2, H - TASKBAR_H + 22)
        idx = int(wait_serial(r"wm: menu row (\d+) hold\n").group(1))
        mon.click(MENU_X + MENU_W // 2, menu_row_y(H, idx))
        pid = wait_serial(r"proc: spawned /bin/hold as pid (\d+)").group(1)
        # honest async launch: started, pid P, still running — no exit code yet
        wait_serial(r"shell: hold started, pid %s \(running\)\n" % pid)
        # a real PS/2 move while hold is mid-run gives the loop a fresh reason
        # to repaint; the interleave itself is read out of the accumulated log
        time.sleep(0.4)
        mon.move_rel(40, 30)
        deadline = time.time() + 20
        interleaved = False
        while time.time() < deadline:
            with open(LOG, errors="replace") as fh:
                text = fh.read()
            rounds = [mm.start() for mm in
                      re.finditer(r"\[hold\] round \d+ pid %s\n" % pid, text)]
            paints = [mm.start() for mm in
                      re.finditer(r"wm: frame painted", text)]
            if any(any(r < p for r in rounds) and any(r > p for r in rounds)
                   for p in paints):
                interleaved = True
                break
            time.sleep(0.2)
        if not interleaved:
            raise SystemExit("no repaint landed between two live hold rounds")
        # and it reaches its async end later, reaped with the code the launch
        # could not have known at click time
        wait_serial(r"\[hold\] exiting pid %s code 5\n" % pid)
        wait_serial(r"shell: reaped pid %s code 5\n" % pid)
    elif sc == "progwin":
        # v0.1.11: a launched program's output belongs to *its own* window, and
        # the window outlives the process. Click /bin/hold; read its pid; then
        # prove the three claims the release makes, all naming that same pid:
        # a window was opened for it, the console router sent its bytes there
        # (not to the terminal), and when the process died the window finalised
        # with its exit code and the byte count it received — but is still open.
        # Only the close click may dismiss it, so the "closed" marker must be
        # absent until we click it, and present after. Geometry comes from the
        # kernel's own cascade, not a guessed coordinate: the first program
        # window sits at (150,56), PROG_W wide, close packed against its corner.
        mon.click(START_X + START_W // 2, H - TASKBAR_H + 22)
        idx = int(wait_serial(r"wm: menu row (\d+) hold\n").group(1))
        mon.click(MENU_X + MENU_W // 2, menu_row_y(H, idx))
        pid = wait_serial(r"proc: spawned /bin/hold as pid (\d+)").group(1)
        wait_serial(r"wm: program window opened 'hold' pid %s\n" % pid)
        wait_serial(r"wm: routed output pid %s\n" % pid)
        # the finish line carries the exit code and the routed byte count; a
        # running program's output never reaches the terminal, so bytes > 0 here
        m = wait_serial(r"wm: program window pid %s finished code 5, (\d+) bytes\n" % pid)
        if int(m.group(1)) <= 0:
            raise SystemExit("hold's window recorded no routed bytes")
        if "wm: program window closed 'hold'" in open(LOG, errors="replace").read():
            raise SystemExit("hold's window closed itself before it was clicked")
        pw = min(PROG_W, W - 40)
        cx, cy = title_button((PROG_X0, PROG_Y0, pw, PROG_H), 2)   # BTN_CLOSE
        mon.click(cx, cy)
        wait_serial(r"wm: program window closed 'hold' pid %s\n" % pid)
    elif sc == "damage":
        # v0.1.12: the compositor draws into a RAM back buffer and pushes only
        # what changed. The claim has three parts, all read from the kernel's own
        # "gfx: flip <n> rects, <area> px" lines - nothing about the pixel counts
        # is guessed, W and H come from the composited banner the caller parsed.
        text = read_log()
        if "gfx: back buffer" not in text:
            raise SystemExit("no back buffer was allocated - direct-to-framebuffer fallback")
        flips = [(int(n), int(a)) for n, a in re.findall(FLIP_RE, text)]
        if not flips:
            raise SystemExit("the compositor never reported a flip")
        # the boot frame has no back buffer to update, so it is whole-screen:
        # exactly one rectangle covering every pixel
        n0, area0 = flips[0]
        if n0 != 1 or area0 != W * H:
            raise SystemExit("boot flip was not the whole screen: %d rects, %d px"
                             % (n0, area0))
        # a pure mouse move (no button) is the case the release is about: it must
        # repaint only the band the cursor swept, a rounding error against W*H.
        # It has to start from a quiet desktop, and that is not a nicety but the
        # measurement itself. The preamble waits for the desktop marker, while
        # the boot's own demo programs keep streaming into the terminal for
        # seconds after it; every one of those frames damages the window body -
        # the largest rectangle on the screen - so a move issued in the middle
        # of them is being judged against flips that are not its own. This is
        # how the tag build of v0.1.22 failed on a loaded CI runner and passed
        # twice on a laptop: same kernel, same pixels, a mouse move that landed
        # in a busy second. So wait for the compositor to stop flipping (a frame
        # only happens when something is dirty, so an absence of flips is what a
        # quiet desktop means), then move, then take the *first* frame after the
        # silence - it can belong to the cursor and to nothing else, which is a
        # stronger claim than "some flip in the neighbourhood was small".
        mark = wait_quiet()
        if mark is None:
            raise SystemExit("the desktop never went quiet - something repaints with no input")
        mon.move_rel(40, 30)
        small = first_flip_after(mark)
        if small is None:
            raise SystemExit("a mouse move on a quiet desktop caused no frame at all")
        if small[1] * 8 >= W * H:
            raise SystemExit("a mouse move repainted %d rects, %d px - not a band"
                             % (small[0], small[1]))
        # a click on a window body is a structural change (z-order/raise), so the
        # honesty of the smaller blit depends on this still flipping everything.
        # Same precondition, same reason: quiet first, then click.
        mark = wait_quiet()
        if mark is None:
            raise SystemExit("the desktop never went quiet after the mouse move")
        mon.click(tx + 60, ty + TITLE_H + 60)
        full = wait_flip_after(mark, lambda n, a: n == 1 and a >= W * H)
        if full is None:
            raise SystemExit("a click on a window did not repaint the whole screen")
    elif sc == "progdamage":
        # v0.1.13: a *running program's* output must repaint only its own window,
        # not the whole desktop. The distinguishing witness is not merely "a
        # sub-screen flip happened" (the terminal body was already sub-screen in
        # v0.1.12) but "a sub-screen flip landed strictly between two of hold's
        # own rounds" - those are exactly the frames hold's output caused, and on
        # v0.1.12 every one of them was a full W*H flip. Same interleave idea as
        # the live scenario, now measuring the flip's area. Nothing is hardcoded:
        # pid from the spawn line, W*H from the composited banner.
        text = open(LOG, errors="replace").read()
        if "gfx: back buffer" not in text:
            raise SystemExit("no back buffer was allocated - direct-to-framebuffer fallback")
        flips = [(int(n), int(a)) for n, a in
                 re.findall(r"gfx: flip (\d+) rects, (\d+) px", text)]
        if not flips:
            raise SystemExit("the compositor never reported a flip")
        n0, area0 = flips[0]
        if n0 != 1 or area0 != W * H:
            raise SystemExit("boot flip was not the whole screen: %d rects, %d px"
                             % (n0, area0))
        # The same precondition `damage` learned the hard way: the boot's own
        # demo programs keep streaming into the terminal for seconds after the
        # desktop marker, and a terminal repaint is a sub-screen flip as well.
        # Quiescing before the launch is what makes a sub-screen flip found
        # between two of hold's rounds belong to *hold* rather than to a boot
        # program that happened to be printing at the same moment.
        if wait_quiet() is None:
            raise SystemExit("the desktop never went quiet - something repaints with no input")
        # launch /bin/hold from the menu (the launch click is a full flip - fine)
        mon.click(START_X + START_W // 2, H - TASKBAR_H + 22)
        idx = int(wait_serial(r"wm: menu row (\d+) hold\n").group(1))
        mon.click(MENU_X + MENU_W // 2, menu_row_y(H, idx))
        pid = wait_serial(r"proc: spawned /bin/hold as pid (\d+)").group(1)
        wait_serial(r"\[hold\] round 2 pid %s\n" % pid)   # a between-rounds gap exists
        deadline = time.time() + 20
        between = False
        while time.time() < deadline:
            t = open(LOG, errors="replace").read()
            rounds = [m.start() for m in
                      re.finditer(r"\[hold\] round \d+ pid %s\n" % pid, t)]
            subs = [m.start() for m in
                    re.finditer(r"gfx: flip \d+ rects, (\d+) px", t)
                    if 0 < int(m.group(1)) < W * H]
            if any(r0 < s < r1 for s in subs for r0, r1 in zip(rounds, rounds[1:])):
                between = True
                break
            time.sleep(0.2)
        if not between:
            raise SystemExit("a running program still repainted the whole screen")
        # let it reach its honest end so the log is clean for the reap marker
        wait_serial(r"shell: reaped pid %s code 5\n" % pid)
    elif sc == "resize":
        # v0.1.14: drag a border to resize a window. The status window (116,88,
        # 400x300) has room to grow down-right, but the terminal overlaps its
        # bottom-right corner, so raise the status window first by clicking a
        # body point clear of the terminal — that is the front-to-back rule a
        # user hits too. Everything the asserts check is read back from the
        # kernel's own `wm: window resized WxH` line, never a guessed geometry.
        sx, sy = 116, 88
        mon.click(sx + 40, sy + TITLE_H + 40)          # raise the status window
        mon.drag(sx + STATUS_W - 2, sy + STATUS_H - 2, 60, 50)  # grow the corner
        m = last_resized()
        if m is None:
            raise SystemExit("dragging a border logged no resize")
        w1, h1 = m
        if not (w1 > STATUS_W and h1 > STATUS_H and w1 <= W - sx):
            raise SystemExit("grow resize wrong: %dx%d from %dx%d (W=%d)"
                             % (w1, h1, STATUS_W, STATUS_H, W))
        # Shrink far past the floor: grab the now-larger corner and drag inward.
        # The window must stop at MIN_W x MIN_H — proof of the min-size floor,
        # not merely that it got smaller. Corner read from the size just measured.
        mon.drag(sx + w1 - 2, sy + h1 - 2, -2000, -2000)
        m = last_resized()
        if m is None:
            raise SystemExit("shrinking a border logged no resize")
        if m != (MIN_W, MIN_H):
            raise SystemExit("min size not enforced: got %dx%d, want %dx%d"
                             % (m[0], m[1], MIN_W, MIN_H))
        # v0.1.15: the top edge too. Grab the thin strip above the title-bar
        # buttons (the window is now min-sized at (116,88,200,120), still front)
        # and drag it upward; the top edge follows the cursor while the bottom
        # stays put, so the window grows taller. Reading only the height back
        # proves the vertical half of a top grab works and did not clash with
        # the drag or the buttons.
        mon.drag(sx + MIN_W // 2, sy + 2, 0, -30)
        m = last_resized()
        if m is None:
            raise SystemExit("dragging the top edge logged no resize")
        w3, h3 = m
        if not (h3 > MIN_H and w3 == MIN_W):
            raise SystemExit("top-edge resize wrong: %dx%d, want height > %d, "
                             "width unchanged at %d" % (w3, h3, MIN_H, MIN_W))
    elif sc == "cursor":
        # v0.1.16: the pointer changes shape over a draggable border. Raise the
        # status window, hover its left edge, then its bottom-right corner, then
        # its body. The kernel logs the border mask it is showing on every change
        # — the same geometry a click would resize from, read from the log rather
        # than a screenshot, so this proves the hit-test and not just the art.
        sx, sy = 116, 88
        LEFT, RIGHT, BOTTOM = 1, 2, 8
        mon.click(sx + 40, sy + TITLE_H + 40)              # raise it; arrow inside
        mon.move_to(sx + 2, sy + STATUS_H // 2)            # left edge
        time.sleep(0.5)
        m = last_cursor()
        if m != LEFT:
            raise SystemExit("left edge showed cursor %r, want %d (RS_LEFT)" % (m, LEFT))
        mon.move_to(sx + STATUS_W - 2, sy + STATUS_H - 2)  # bottom-right corner
        time.sleep(0.5)
        m = last_cursor()
        if m != (RIGHT | BOTTOM):
            raise SystemExit("corner showed cursor %r, want %d (RIGHT|BOTTOM)"
                             % (m, RIGHT | BOTTOM))
        mon.move_to(sx + STATUS_W // 2, sy + STATUS_H // 2)  # body -> plain arrow
        time.sleep(0.5)
        m = last_cursor()
        if m != 0:
            raise SystemExit("body showed cursor %r, want 0 (arrow)" % (m,))
    elif sc == "snap":
        # v0.1.17 Aero Snap: drop a dragged title bar on a screen edge and the
        # window tiles to a half or a quarter. Raise the status window, drag it
        # to the left edge, then drag the tiled window to the top-right corner.
        # The kernel logs the zone code and the rect it chose, so the assertion
        # reads geometry the WM computed - not pixels the test guessed.
        sx, sy = 116, 88
        work = H - TASKBAR_H
        SNAP_LEFT, SNAP_TR = 1, 4              # the kernel's SNAP_* enum
        mon.click(sx + 40, sy + TITLE_H + 40)  # raise it, clear of every border
        # grab the title bar - below the 5 px top resize band, far from the
        # buttons - and release 6 px inside the left edge at mid height
        gx, gy = sx + 40, sy + TITLE_H // 2
        mon.drag(gx, gy, 6 - gx, H // 2 - gy)
        time.sleep(0.5)
        s = last_snap()
        if s is None:
            raise SystemExit("dragging to the left edge logged no snap")
        want = (SNAP_LEFT, W // 2, work, 0, 0)
        if s != want:
            raise SystemExit("left snap %r, want %r" % (s, want))
        # the window is now the left half at (0,0). Grab its title and release in
        # the top-right corner: the quarter must be sized from the work area
        # (work // 2 tall), not from the half it was living in - which is what
        # proves the drag-out kept the pre-snap rect instead of reusing the tile.
        gx, gy = 40, TITLE_H // 2
        mon.drag(gx, gy, (W - 6) - gx, 6 - gy)
        time.sleep(0.5)
        s = last_snap()
        want = (SNAP_TR, W // 2, work // 2, W // 2, 0)
        if s != want:
            raise SystemExit("top-right snap %r, want %r" % (s, want))
    elif sc == "filewrite":
        # v0.1.18: the writable half of the file layer, driven from a real
        # keyboard. One process writes a note and dies; a *second* process,
        # with a different pid and no memory of the first, opens the same path
        # and prints those bytes back. That is what makes it a file rather than
        # a variable the kernel remembers.
        #
        # The claim this scenario reads is not "the kernel logged a number": the
        # note's own text has to appear in the log with nothing left that put it
        # there. The shell never echoes a typed line to the serial port (its
        # put_line() writes to the terminal surface only) and launch() logs just
        # argv[0], so the bytes between the reader's two lines came out of the
        # store and nowhere else.
        note = "hello from the mouse"
        tw = min(TERM_W, W - 40)
        tx, ty = W - tw - 48, H - TASKBAR_H - TERM_H - 60
        mon.click(tx + tw // 2, ty + TITLE_H // 2)   # raise the terminal, the
                                                     # way `shot` does
        mon.type_text("save write demo " + note + "\n")
        m = wait_serial(r"save: wrote (\d+) bytes /tmp/demo\.txt pid (\d+)")
        wrote, wpid = int(m.group(1)), int(m.group(2))
        if wrote != len(note) + 1:
            raise SystemExit("the store kept %d bytes, want %d (%d of text plus "
                             "a newline)" % (wrote, len(note) + 1, len(note)))

        mon.type_text("save read demo\n")
        m = wait_serial(r"save: opening /tmp/demo\.txt pid (\d+)")
        rpid, opened_at = int(m.group(1)), m.end()
        m = wait_serial(r"save: read back (\d+) bytes /tmp/demo\.txt pid (\d+)")
        back, bpid = int(m.group(1)), int(m.group(2))
        if back != wrote:
            raise SystemExit("a second process read %d bytes back; the writer "
                             "wrote %d" % (back, wrote))
        if rpid == wpid or bpid == wpid:
            raise SystemExit("the reader is the writer's own process (pid %d): "
                             "nothing crossed a process boundary" % rpid)
        between = read_log()[opened_at:m.start()]
        if note not in between:
            raise SystemExit("the note's bytes never landed between the reader's "
                             "two lines: %r" % between[:160])

        # And the directory: SYS_GETDIRENT now spans both stores, so a third
        # process has to see the note by name with the size the writer reported.
        mon.type_text("save list\n")
        m = wait_serial(r"save: listed (\d+) files in the store")
        if int(m.group(1)) != 1:
            raise SystemExit("the store listed %s files, want exactly the one "
                             "this run wrote" % m.group(1))
        m = wait_serial(r"save: tmp/demo\.txt\s+(\d+)")
        if int(m.group(1)) != wrote:
            raise SystemExit("the directory entry says %s bytes, the writer "
                             "wrote %d" % (m.group(1), wrote))
    elif sc == "batchfile":
        # v0.1.19: the shell reads instructions out of a file a ring-3 program
        # made. Every claim below is a line the kernel wrote itself, and the one
        # that matters is the origin: a launched program logs `from the prompt`
        # when a person typed it and `from a batch file` when the script did, so
        # the log cannot confuse the two paths.
        script = "about\necho hi\n"
        tw = min(TERM_W, W - 40)
        tx, ty = W - tw - 48, H - TASKBAR_H - TERM_H - 60
        mon.click(tx + tw // 2, ty + TITLE_H // 2)   # raise the terminal
        mon.type_text("save script demo about nl echo hi\n")
        m = wait_serial(r"save: wrote (\d+) bytes /tmp/demo\.bat pid (\d+)")
        wrote = int(m.group(1))
        if wrote != len(script):
            raise SystemExit("the script landed as %d bytes, want %d (%r)"
                             % (wrote, len(script), script))

        mon.type_text("bat demo\n")
        m = wait_serial(r"shell: batch file /tmp/demo\.bat \((\d+) bytes\)")
        if int(m.group(1)) != wrote:
            raise SystemExit("the shell read %s bytes of a file the writer "
                             "reported as %d" % (m.group(1), wrote))
        wait_serial(r"shell: running 'echo' from a batch file")
        m = wait_serial(r"shell: batch ran (\d+) commands from /tmp/demo\.bat")
        ran = int(m.group(1))
        if ran != 2:
            raise SystemExit("the script ran %d of its 2 lines (a built-in and "
                             "a /bin program)" % ran)
        # Counted, not just waited for: `batch ran 2` proves two lines reached the
        # dispatcher, and this proves one of them was the program and the other a
        # built-in rather than the same line running twice.
        if count_serial(r"shell: running 'echo' from a batch file") != 1:
            raise SystemExit("the batch file's program line did not reach the "
                             "launcher exactly once")

        # A name that is not a word is refused before anything is opened, and the
        # log echoes the whole attempt back: the regex only matches if every '/'
        # and '.' actually arrived as a key, so a dropped sendkey fails here
        # instead of quietly testing a different command line.
        mon.type_text("bat nope\n")
        wait_serial(r"shell: no batch file /tmp/nope\.bat")
        mon.type_text("bat ../bin/ls\n")
        wait_serial(r"shell: bad batch name \.\./bin/ls")

        # No nesting, and the rest of the script still runs: a refusal is an
        # answer, not a crash.
        mon.type_text("save script nest bat demo nl echo ok\n")
        m = wait_serial(r"save: wrote (\d+) bytes /tmp/nest\.bat")
        if int(m.group(1)) != len("bat demo\necho ok\n"):
            raise SystemExit("the nested script landed as %s bytes" % m.group(1))
        mon.type_text("bat nest\n")
        wait_serial(r"shell: batch refused nested script")
        m = wait_serial(r"shell: batch ran (\d+) commands from /tmp/nest\.bat")
        if int(m.group(1)) != 1:
            raise SystemExit("a script inside a script ran %s commands; the "
                             "nested line must be refused and the next one kept"
                             % m.group(1))
        if count_serial(r"shell: running 'echo' from a batch file") != 2:
            raise SystemExit("the line after a refused nested script did not run")
    elif sc == "batch2":
        # v0.1.37: the half of cmd.exe a copied script leans on. `@echo off` is
        # the first line of nearly every Windows .bat, and SET / %VAR% carry its
        # values; without them a script echoed its own commands and ran a literal
        # "%x%" where it meant "hi". Every claim sits on the kernel's own line
        # `shell: batch exec '<resolved line>'`: it is written for every script
        # line whatever the screen echo does, so even ECHO OFF leaves a transcript
        # the test can read. The three scripts stay short on purpose - the shell's
        # line editor holds SHELL_COLS-1 (61) keystrokes, and a longer typed
        # command is silently truncated at the prompt, so each `save script` here
        # is counted to fit. Everything is typed lowercase because the QMP qcode
        # alphabet has no capitals; the built-ins match case-insensitively and the
        # log folds the name, so `set x=hi` still reports `X=hi` - two spellings
        # in one transcript that prove the matcher and the folder both ran.
        script_a = "@echo off\nset x=hi\nrem note\necho %x%\n"
        tw = min(TERM_W, W - 40)
        tx, ty = W - tw - 48, H - TASKBAR_H - TERM_H - 60
        mon.click(tx + tw // 2, ty + TITLE_H // 2)   # raise the terminal
        mon.type_text("save script a @echo off nl set x=hi nl rem note nl echo %x%\n")
        m = wait_serial(r"save: wrote (\d+) bytes /tmp/a\.bat")
        if int(m.group(1)) != len(script_a):
            raise SystemExit("the script landed as %s bytes, want %d (%r)"
                             % (m.group(1), len(script_a), script_a))

        # One run of the script, and every line is asserted to come after the
        # previous: mark is the log length before `bat a`, so nothing earlier can
        # satisfy a wait. The '@' is stripped (the line still runs, only its
        # screen echo hides); SET folds its name; REM is a comment; and `echo %x%`
        # resolves to `echo hi`, launches the program, and the program's own stdout
        # row - a bare "hi" - lands between the launch and the count.
        mark = len(read_log())
        mon.type_text("bat a\n")
        m, _ = wait_serial_after(mark, r"shell: batch file /tmp/a\.bat \(%d bytes\)"
                                 % len(script_a))
        m, _ = wait_serial_after(m, r"shell: batch exec 'echo off'")
        m, _ = wait_serial_after(m, r"shell: batch echo off")
        m, _ = wait_serial_after(m, r"shell: batch exec 'set x=hi'")
        m, _ = wait_serial_after(m, r"shell: batch set X=hi")
        m, _ = wait_serial_after(m, r"shell: batch exec 'rem note'")
        m, _ = wait_serial_after(m, r"shell: batch remark skipped")
        # The expansion proof, in order: the resolved exec row says `echo hi`,
        # then the origin row says the launch came from a batch file. That exec
        # row is written from bat_work AFTER %expansion, so it reads `echo hi`
        # only because %x% resolved to the value `set` stored; a script that
        # skipped expansion would log `exec 'echo %x%'`, and the negative count
        # below proves no such unexpanded line was ever dispatched. (The /bin/echo
        # program's own bytes also land on the port, but they are routed without a
        # trailing serial newline, so the resolved exec row - not the raw stdout -
        # is the reliable byte-for-byte claim.)
        m, _ = wait_serial_after(m, r"shell: batch exec 'echo hi'")
        m, _ = wait_serial_after(m, r"shell: running 'echo' from a batch file")
        if count_serial(r"shell: batch exec 'echo %x%'") != 0:
            raise SystemExit("an unexpanded `echo %x%` reached the dispatcher; "
                             "the %VAR% expansion did not happen")
        _, ran = wait_serial_after(m, r"shell: batch ran (\d+) commands from /tmp/a\.bat")
        if int(ran.group(1)) != 1:
            raise SystemExit("the script ran %s commands; echo off, set and rem "
                             "are built-ins, only the expanded echo line may be "
                             "dispatched" % ran.group(1))

        # The echo state machine, one line per report. Each script resets to echo
        # ON (like cmd), so this script's first `echo off` is genuinely a change,
        # and the bare `echo` after it must answer "off" and the one after
        # `echo on` must answer "on". Screen echo being off does not hide these:
        # the exec row is written before the echo decision.
        script_b = "echo off\necho\necho on\necho\n"
        mon.type_text("save script b echo off nl echo nl echo on nl echo\n")
        m = wait_serial(r"save: wrote (\d+) bytes /tmp/b\.bat")
        if int(m.group(1)) != len(script_b):
            raise SystemExit("the state script landed as %s bytes, want %d"
                             % (m.group(1), len(script_b)))
        mark = len(read_log())
        mon.type_text("bat b\n")
        m, _ = wait_serial_after(mark, r"shell: batch exec 'echo off'")
        m, _ = wait_serial_after(m, r"shell: batch echo off")
        m, _ = wait_serial_after(m, r"shell: batch exec 'echo'")
        m, _ = wait_serial_after(m, r"shell: batch echo state off")
        m, _ = wait_serial_after(m, r"shell: batch exec 'echo on'")
        m, _ = wait_serial_after(m, r"shell: batch echo on")
        m, _ = wait_serial_after(m, r"shell: batch exec 'echo'")
        m, _ = wait_serial_after(m, r"shell: batch echo state on")

        # A ';' comment and a bare SET, in the shortest script that carries both.
        # The environment was filled by script a and is *not* reset by script b,
        # so listing it here proves the session persisted across two other scripts
        # - exactly the cmd behaviour a multi-file build relies on.
        script_c = "; x\nset\n"
        mon.type_text("save script c ; x nl set\n")
        m = wait_serial(r"save: wrote (\d+) bytes /tmp/c\.bat")
        if int(m.group(1)) != len(script_c):
            raise SystemExit("the listing script landed as %s bytes, want %d"
                             % (m.group(1), len(script_c)))
        mark = len(read_log())
        mon.type_text("bat c\n")
        # the ';' line is a comment answered on the transcript (not a silent hole,
        # and not dispatched); the bare `set` line then lists the one variable a
        # previous script left behind.
        m, _ = wait_serial_after(mark, r"shell: batch comment skipped")
        m, _ = wait_serial_after(m, r"shell: batch exec 'set'")
        m, _ = wait_serial_after(m, r"shell: batch env 1 variables")
        wait_serial_after(m, r"shell: batch env X=hi")
    elif sc == "promptset":
        # v0.1.38: the two cmd built-ins a person types at the keyboard, on
        # the *prompt* path rather than inside a script. `set NAME=VALUE`
        # defines a session variable, a bare `set` lists them, `set wrong`
        # (no '=') refuses honestly, and `type <path>` prints a file back out
        # of the store. Every claim is a line the kernel wrote itself on the
        # serial port, so the transcript is a byte-for-byte record of what
        # the built-in did, not what the screen echoed. Names fold to upper
        # case the way cmd folds them, so `set pz=bonjour` shows up as
        # `prompt set PZ=bonjour` - the two spellings in one log line that
        # prove the matcher ran and the folder ran. Content typed here stays
        # lowercase because the QMP qcode alphabet has no capitals.
        tw = min(TERM_W, W - 40)
        tx, ty = W - tw - 48, H - TASKBAR_H - TERM_H - 60
        mon.click(tx + tw // 2, ty + TITLE_H // 2)   # raise the terminal

        # A variable defined at the prompt, then listed back. The env lives
        # in the kernel across every path (the batch reader already shares
        # it), so the bare `set` after the definition sees exactly one
        # entry - the one this scenario just made.
        mark = len(read_log())
        mon.type_text("set pz=bonjour\n")
        m, _ = wait_serial_after(mark, r"shell: prompt set PZ=bonjour")
        mark = len(read_log())
        mon.type_text("set\n")
        m, _ = wait_serial_after(mark, r"shell: prompt env 1 variables")
        m, _ = wait_serial_after(m, r"shell: prompt env PZ=bonjour")

        # A `set` with no '=' is refused, not guessed: cmd says nothing
        # useful either, and a line the log can read beats a silent half
        # command. The count check proves this refusal was a fresh event,
        # not an earlier one still sitting in the buffer.
        mark = len(read_log())
        mon.type_text("set wrong\n")
        wait_serial_after(mark,
                          r"shell: prompt set refused - want NAME=VALUE")

        # `type` prints a file the /bin/save *program* wrote to the store,
        # so the bytes cross the store boundary twice: once as a routed
        # program stdout ("save: wrote 17 bytes"), once as a kernel-dumped
        # transcript line. The 17 is exactly "bonjour le monde\n" - the 16
        # joined bytes plus the `nl` terminator, and no trailing newline on
        # top of it because build_content refuses to add a second one. The
        # content itself then lands on the serial port as raw bytes from
        # serial_write_len, and the wait asserts the file was printed and
        # not merely named.
        mark = len(read_log())
        mon.type_text("save script hi bonjour le monde nl\n")
        _, m = wait_serial_after(mark, r"save: wrote (\d+) bytes /tmp/hi\.bat")
        if int(m.group(1)) != 17:
            raise SystemExit("the note landed as %s bytes, want 17"
                             % m.group(1))
        mark = len(read_log())
        mon.type_text("type /tmp/hi.bat\n")
        m, _ = wait_serial_after(mark,
                                 r"shell: type /tmp/hi\.bat \(17 bytes\)")
        wait_serial_after(m, r"bonjour le monde")

        # A name that is not there says so on the transcript instead of
        # printing an empty window and calling that a success.
        mark = len(read_log())
        mon.type_text("type /tmp/nope.bat\n")
        wait_serial_after(mark, r"shell: type no such file /tmp/nope\.bat")
    elif sc == "promptvar":
        # v0.1.39: %VAR% expansion on a *typed* line. cmd expands a command
        # before it runs, and now so does the prompt: `echo %pz%` after
        # `set pz=bonjour` becomes `echo bonjour`. The claim sits on the
        # kernel's own row `shell: prompt exec '<resolved>'` - written only
        # when a defined name actually substituted - then the launch row
        # `shell: running 'echo' from the prompt`. The undefined case is the
        # safety property: `echo %nope%` must produce NO exec row (a literal
        # %nope% is dispatched untouched), which is precisely what keeps the
        # batch2 scenario's typed `save ... echo %x%` from losing content.
        # Content stays lowercase; the QMP qcode alphabet has no capitals, and
        # the folder matches case-insensitively (`set pz=...` logs `PZ`).
        tw = min(TERM_W, W - 40)
        tx, ty = W - tw - 48, H - TASKBAR_H - TERM_H - 60
        mon.click(tx + tw // 2, ty + TITLE_H // 2)   # raise the terminal

        # Define the variable the expansion will use.
        mark = len(read_log())
        mon.type_text("set pz=bonjour\n")
        wait_serial_after(mark, r"shell: prompt set PZ=bonjour")

        # A defined name substitutes: the exec row reads the resolved line,
        # and the dispatcher then launches /bin/echo from the prompt.
        mark = len(read_log())
        mon.type_text("echo %pz%\n")
        m, _ = wait_serial_after(mark, r"shell: prompt exec 'echo bonjour'")
        wait_serial_after(m, r"shell: running 'echo' from the prompt")

        # An undefined name stays literal: no exec row is written at all, so
        # the raw `echo %nope%` reaches the dispatcher unchanged. Proving the
        # count is zero is what distinguishes "left literal" from "stripped to
        # nothing" (the batch reader's behaviour) - the difference that makes
        # prompt expansion safe to turn on without breaking a typed script line.
        mark = len(read_log())
        mon.type_text("echo %nope%\n")
        _, m = wait_serial_after(mark, r"shell: running 'echo' from the prompt")
        if count_serial(r"shell: prompt exec 'echo %nope%'") != 0:
            raise SystemExit("an undefined %nope% produced a prompt exec row; "
                             "undefined names must stay literal, not expand")
    elif sc == "caret":
        # v0.1.40: cmd's caret escape. `^` consumes itself and emits the next
        # character literally, and crucially a `^%` cannot start a %VAR%
        # reference - so `echo ^%pz%` prints `%pz%` even after pz is defined,
        # the exact tool that lets a real percent survive a line that also
        # expands variables. The claim is the kernel's own resolved row: the
        # caret is GONE and the percent is LITERAL in what got dispatched.
        # Typing `^` goes through the QMP shift-6 chord (the kernel maps
        # scancode 0x07 + shift to '^'), the same proven path as shift-5 for %.
        tw = min(TERM_W, W - 40)
        tx, ty = W - tw - 48, H - TASKBAR_H - TERM_H - 60
        mon.click(tx + tw // 2, ty + TITLE_H // 2)   # raise the terminal

        # Define pz so the escape is meaningful: without the caret this line
        # would expand to `echo bonjour`, so a resolved row that still reads
        # `%pz%` proves the caret blocked the expansion.
        mark = len(read_log())
        mon.type_text("set pz=bonjour\n")
        wait_serial_after(mark, r"shell: prompt set PZ=bonjour")

        # `echo ^%pz%`: the caret escapes the first percent, the dispatch sees
        # `echo %pz%` (caret consumed, no expansion), and the launch row proves
        # it still reached the dispatcher from the prompt.
        mark = len(read_log())
        mon.type_text("echo ^%pz%\n")
        m, _ = wait_serial_after(mark, r"shell: prompt exec 'echo %pz%'")
        wait_serial_after(m, r"shell: running 'echo' from the prompt")

        # The caret's other half: it vanishes and the next char is literal, so
        # `echo a^b` resolves to `echo ab` - a plain consumed-caret proof that
        # needs no variable at all.
        mark = len(read_log())
        mon.type_text("echo a^b\n")
        m, _ = wait_serial_after(mark, r"shell: prompt exec 'echo ab'")
        wait_serial_after(m, r"shell: running 'echo' from the prompt")
    elif sc == "typing":
        # v0.1.20: the keyboard reaches a *program*. Everything the earlier
        # scenarios type goes to the prompt; this one hands the same PS/2 bytes
        # to a process in ring 3 and proves three separate things from the
        # kernel's own lines: the program slept rather than polled, every byte
        # arrived (none leaked to the command line, none dropped), and the prompt
        # came back on its own when the program finished.
        typed = "hello there"
        tw = min(TERM_W, W - 40)
        tx, ty = W - tw - 48, H - TASKBAR_H - TERM_H - 60
        mon.click(tx + tw // 2, ty + TITLE_H // 2)   # raise the terminal
        mon.type_text("keys\n")

        m = wait_serial(r"wm: program window opened 'keys' pid (\d+)")
        wpid = int(m.group(1))
        m = wait_serial(r"keys: waiting for keys pid (\d+)")
        kpid = int(m.group(1))
        if kpid != wpid:
            raise SystemExit("the window belongs to pid %d but the reader is "
                             "pid %d: the keyboard went to somebody else"
                             % (wpid, kpid))
        # The claim underneath the whole release: read(0) *slept*. Nothing else
        # in the table was runnable at that moment, so the scheduler says so, and
        # it is the idle loop that stays in charge until a key lands.
        wait_serial(r"asleep until a keystroke wakes it")

        mon.type_text(typed + "\n")
        m = wait_serial(r"keys: line '([^']*)' len (\d+) pid (\d+)")
        text, count = m.group(1), int(m.group(2))
        if text != typed:
            raise SystemExit("the program read %r, want %r: a keystroke was "
                             "lost, reordered or decoded wrong" % (text, typed))
        if count != len(typed):
            raise SystemExit("the program says its line is %d bytes, it holds "
                             "%d characters" % (count, len(typed)))
        if int(m.group(3)) != kpid:
            raise SystemExit("a different pid finished the line: %s" % m.group(3))

        # Every byte typed is accounted for exactly once. The kernel may deliver
        # them in several reads — that is scheduling, not a contract — so the
        # honest claim is the sum, and it has to include the Enter.
        chunks = [int(c) for c in re.findall(r"keys: got (\d+) bytes pid",
                                             read_log())]
        if sum(chunks) != len(typed) + 1:
            raise SystemExit("the reads delivered %d of the %d bytes typed (%s "
                             "chunks)" % (sum(chunks), len(typed) + 1, chunks))

        # The other half of the proof: the prompt never saw any of it. A leak
        # would show up as the shell trying to run "hello".
        for leak in (r"shell: running 'hello'", r"shell: no program /bin/hello"):
            if count_serial(leak) != 0:
                raise SystemExit("typing reached the command line as well: %s"
                                 % leak)

        # The program ends itself on the newline, its keyboard claim goes back
        # to the console, and the next word typed is a command again — with no
        # click in between.
        m = wait_serial(r"proc: pid (\d+) died holding the keyboard")
        if int(m.group(1)) != kpid:
            raise SystemExit("pid %s died holding the keyboard, not the reader "
                             "%d" % (m.group(1), kpid))
        wait_serial(r"shell: keys exited with 0")
        mon.type_text("echo done\n")
        wait_serial(r"shell: running 'echo' from the prompt")
    elif sc == "editing":
        # v0.1.21: a text editor, which is three releases of plumbing finally
        # pointed at one window. The scenario writes a known file with one
        # process, opens it with the editor, edits it with real PS/2 events and
        # saves, then reads it back with a *third* process and compares the
        # bytes. Every number below is computed from the keys sent, so a byte
        # that arrived twice, out of order, or not at all changes a count rather
        # than passing quietly.
        base = "hello world"                      # what /bin/save puts in the file
        tail = " and zeta"                        # typed at the caret
        second = "second line"                    # after Enter broke the line
        # The file the edits are meant to produce. Enter lands *before* the
        # newline the file already ended with, so it breaks the line instead of
        # adding a blank one - which is what an editor is supposed to do.
        want = base + tail + "\n" + second + "\n"

        tw = min(TERM_W, W - 40)
        tx, ty = W - tw - 48, H - TASKBAR_H - TERM_H - 60
        mon.click(tx + tw // 2, ty + TITLE_H // 2)   # raise the terminal
        mon.type_text("save write demo " + base + "\n")
        m = wait_serial(r"save: wrote (\d+) bytes /tmp/demo\.txt pid (\d+)")
        if int(m.group(1)) != len(base) + 1:
            raise SystemExit("the file the editor is about to open is %s bytes, "
                             "want %d" % (m.group(1), len(base) + 1))
        wpid = int(m.group(2))

        mon.type_text("note demo\n")
        m = wait_serial(r"wm: program window opened 'note' pid (\d+)")
        epid = int(m.group(1))
        m = wait_serial(r"note: loaded (\d+) bytes from /tmp/demo\.txt pid (\d+)")
        if int(m.group(2)) != epid:
            raise SystemExit("the editor that loaded the file is pid %s, and the "
                             "window is pid %d: the keys would go elsewhere"
                             % (m.group(2), epid))
        if int(m.group(1)) != len(base) + 1:
            raise SystemExit("the editor read %s bytes out of the store, which "
                             "holds %d" % (m.group(1), len(base) + 1))
        if epid == wpid:
            raise SystemExit("the editor is the writer's own process (pid %d)"
                             % epid)
        # The window is a screen and not a scrollback: that is what the form feed
        # in the first frame is for, and the compositor says it saw one.
        wait_serial(r"wm: screen cleared pid %d" % epid)
        # The caret at offset 0 drawn in front of the file's own first line — the
        # text on screen came out of the store, and the '|' is the editor's.
        wait_serial(r"\|hello world")
        wait_serial(r"note: editing /tmp/demo\.txt pid %d" % epid)

        # Ctrl+E: to the end of the line. The frame has to say where the caret
        # landed, in the frame written *after* the key (see wait_serial_after).
        mark = len(read_log())
        mon.key("ctrl-e")
        mark, _ = wait_serial_after(mark, r"hello world\|")
        mark, _ = wait_serial_after(mark, r"LN 1 COL %d  %d OCTETS"
                                    % (len(base) + 1, len(base) + 1))

        # Type, erase one character, type back: insert and delete, both visible
        # in the size the ruler row reports. The unsaved flag is not part of that
        # row - the editor puts (MODIFIE) / (SAUVE) in the title, the same row the
        # save below has to see come back to (SAUVE) - so it is counted where the
        # program actually writes it instead of being crammed into this pattern.
        mark = len(read_log())
        mon.type_text(tail)
        mark, _ = wait_serial_after(
            mark, r"LN 1 COL %d  %d OCTETS"
            % (len(base) + len(tail) + 1, len(base) + len(tail) + 1))
        if count_serial(r"NOTEPAD - /tmp/demo\.txt  \(MODIFIE\)") < 1:
            raise SystemExit("the document grew by %d bytes and no frame said it "
                             "was unsaved" % len(tail))
        mon.key("backspace")
        mark, _ = wait_serial_after(
            mark, r"LN 1 COL %d  %d OCTETS"
            % (len(base) + len(tail), len(base) + len(tail)))
        mon.type_text(tail[-1])
        mark, _ = wait_serial_after(
            mark, r"LN 1 COL %d  %d OCTETS"
            % (len(base) + len(tail) + 1, len(base) + len(tail) + 1))

        # Enter breaks the line and the caret starts the next one.
        mon.key("ret")
        mark, _ = wait_serial_after(mark, r"LN 2 COL 1  \d+ OCTETS")

        mon.type_text(second)
        mark, _ = wait_serial_after(
            mark, r"LN 2 COL %d  %d OCTETS"
            % (len(second) + 1, len(want)))

        # Ctrl+S: the document goes into the store, and the count the kernel
        # hands back is the one the editor reports.
        mon.key("ctrl-s")
        m = wait_serial_after(mark, r"note: saved (\d+) bytes to /tmp/demo\.txt "
                                    r"pid (\d+)")
        mark = m[0]
        saved, spid = int(m[1].group(1)), int(m[1].group(2))
        if saved != len(want):
            raise SystemExit("the store kept %d bytes, the editor was asked to "
                             "save %d" % (saved, len(want)))
        if spid != epid:
            raise SystemExit("pid %s saved the file, not the editor (%d)"
                             % (spid, epid))
        mark, _ = wait_serial_after(mark, r"NOTEPAD - /tmp/demo\.txt  \(SAUVE\)")

        # Ctrl+Q: it ends clean, because everything on screen is in the file.
        mon.key("ctrl-q")
        wait_serial(r"note: quit pid %d" % epid)
        wait_serial(r"proc: pid %d died holding the keyboard" % epid)
        wait_serial(r"shell: note exited with 0")

        # None of that typing may have reached the prompt. A leak shows up as the
        # shell trying to run one of the words.
        for word in ("and", "zeta", "second", "line"):
            for leak in (r"shell: running '%s'" % word,
                         r"shell: no program /bin/%s" % word):
                if count_serial(leak) != 0:
                    raise SystemExit("the editor's typing reached the command "
                                     "line as well: %s" % leak)

        # A third process, with no memory of the editor, reads the file back.
        # Between its two lines are the store's bytes and nothing else.
        mon.type_text("save read demo\n")
        m = wait_serial(r"save: opening /tmp/demo\.txt pid (\d+)")
        rpid = int(m.group(1))
        if rpid == epid:
            raise SystemExit("the reader is the editor's own process (pid %d): "
                             "nothing crossed a process boundary" % rpid)
        m = wait_serial(r"save: read back (\d+) bytes /tmp/demo\.txt pid (\d+)")
        back, bpid = int(m.group(1)), int(m.group(2))
        if back != len(want) or back != saved:
            raise SystemExit("a third process read %d bytes; the editor saved "
                             "%d and the document holds %d"
                             % (back, saved, len(want)))
        if bpid != rpid:
            raise SystemExit("the reader's two lines name different processes "
                             "(%s then %s)" % (rpid, bpid))
        # What the reader dumped is the document, line by line, and it is read
        # from the untranslated log: the serial driver puts a carriage return
        # after every line feed it writes, which text mode hands back as a second
        # newline (see read_log_raw). The reader's own two lines frame the region
        # and the kernel's routing announcement inside it is dropped - what is
        # left is compared for equality, so a byte that doubled or vanished shows
        # up even though the count above already agreed.
        raw = read_log_raw()
        opening = "save: opening /tmp/demo.txt pid %d" % rpid
        routed = "wm: routed output pid %d" % rpid
        seen, dumped = False, ""
        for ln in raw.split("\n"):
            ln = ln.replace("\r", "")
            if not seen:
                seen = ln == opening
                continue
            if ln.startswith("save: read back"):
                break
            if ln == routed:
                continue
            dumped += ln + "\n"
        if dumped != want:
            raise SystemExit("the bytes read back are not the bytes the editor "
                             "saved: %r" % dumped)

        # And the frames: a window that repaints is a window whose screen
        # changes, so the title row has to be in the log once per repaint
        # rather than once in total.
        if count_serial(r"NOTEPAD - /tmp/demo\.txt") < 6:
            raise SystemExit("the editor drew %d frames, and an editor that "
                             "paints once does not show an edit"
                             % count_serial(r"NOTEPAD - /tmp/demo\.txt"))
    elif sc == "highlight":
        # v0.1.22: colour inside a program window. /bin/view opens a source file
        # that is already in the initrd, decides what every token *is*, and says
        # so with FS+digit. Three separate facts are proven below, in the order
        # that makes each one depend on the last: the program put escapes on the
        # wire attached to the words they name, the kernel consumed a pair and
        # moved a pen (its own serial line, emitted by the parser and not by a
        # byte counter), and the rows that carry them are the rows the file
        # contains - which is what scrolling proves, because a keyword that only
        # appears on the third screen cannot be painted from a table that was
        # never consulted.
        tw = min(TERM_W, W - 40)
        tx, ty = W - tw - 48, H - TASKBAR_H - TERM_H - 60
        mon.click(tx + tw // 2, ty + TITLE_H // 2)   # raise the terminal
        mon.type_text("view /src/hello.c\n")

        m = wait_serial(r"wm: program window opened 'view' pid (\d+)")
        vpid = int(m.group(1))
        m = wait_serial(r"view: opened /src/hello\.c, (\d+) bytes pid (\d+)")
        nbytes, opid = int(m.group(1)), int(m.group(2))
        if opid != vpid:
            raise SystemExit("the file was read by pid %s and the window belongs "
                             "to pid %d" % (opid, vpid))
        # The specimen has to be longer than one screen or the scroll assertions
        # below would be provable by a viewer that showed the same twelve rows
        # forever, and shorter than the viewer's own 2048-byte budget or the rows
        # past that budget would not exist and the page-downs below would be
        # waiting for text the file never delivered. Both numbers are src/user/
        # view.c's VIEW_MAX and the specimen's length, mirrored here the same way
        # RESIZE_EDGE and MIN_W are mirrored from desktop.c.
        if nbytes < 1024:
            raise SystemExit("the specimen is %d bytes, too short to overflow a "
                             "twelve-row view" % nbytes)
        if nbytes >= 2048:
            raise SystemExit("the specimen is %d bytes, past the viewer's "
                             "budget: it truncated the file" % nbytes)
        m = wait_serial(r"view: highlighted (\d+) keywords in (\d+) lines "
                        r"pid (\d+)")
        nkey, nlines, kpid = int(m.group(1)), int(m.group(2)), int(m.group(3))
        if kpid != vpid or nkey < 5 or nlines < 2 * 12:
            raise SystemExit("the viewer counted %d keywords in %d lines "
                             "(pid %s, window %d)" % (nkey, nlines, kpid, vpid))

        # A frame is a screen and not a scrollback, and the pen moved in it.
        wait_serial(r"wm: screen cleared pid %d" % vpid)
        wait_serial(r"wm: colour pid %d" % vpid)

        # The first screen is the block comment at the top of the specimen, so
        # every row on it is painted in the comment ink: one escape per row, and
        # the row content is the file's own text with no digits of its own.
        wait_serial(r"\x1c7/\* WindClone")

        # Ctrl+F once: rows 13..24, which is where the preprocessor lines are.
        # `include` and `define` are in the keyword table and `42` is a number,
        # so a viewer that painted everything in one ink could not produce these
        # three bytes in this order.
        mark = len(read_log())
        mon.key("ctrl-f")
        mark, _ = wait_serial_after(mark, r"\x1c2include")
        mark, _ = wait_serial_after(mark, r"\x1c2define")
        mark, _ = wait_serial_after(mark, r"\x1c442")

        # Again: rows 25..36, where a type and a control keyword sit on
        # consecutive rows. Both come from a table lookup per token, so the two
        # colours are the evidence the scan ran over the whole file at open
        # rather than over what was already on screen.
        mon.key("ctrl-f")
        mark, _ = wait_serial_after(mark, r"\x1c2static")
        mark, _ = wait_serial_after(mark, r"\x1c6int")

        # And again, to the row with the string literal on it.
        mon.key("ctrl-f")
        mark, _ = wait_serial_after(mark, r'\x1c3"')

        # Ctrl+E jumps to the last page. The status row is written by the viewer
        # from its own counts, so the numbers it has to agree with are the ones
        # taken from the open lines above and not from a transcription of the
        # specimen - a file that gained a line moves the expectation with it.
        mon.key("ctrl-e")
        mark, _ = wait_serial_after(
            mark, r"LIGNES %d-%d SUR %d" % (nlines - 11, nlines, nlines))

        # Ctrl+A back to the top, and the first row of the file reappears.
        mon.key("ctrl-a")
        mark, _ = wait_serial_after(mark, r"\x1c7/\* WindClone")

        # Ctrl+Q, and the exit code the window keeps showing.
        mon.key("ctrl-q")
        wait_serial(r"view: quit pid %d" % vpid)
        wait_serial(r"proc: pid %d died holding the keyboard" % vpid)
        wait_serial(r"shell: view exited with 0")

        # Exactly one window named view: the path was typed once, at the prompt,
        # and no fragment of it or of the viewer's own output came back as a
        # second command. A key that landed on the prompt would show up here as
        # a launch nobody asked for or a name the shell could not find.
        if count_serial(r"wm: program window opened 'view'") != 1:
            raise SystemExit("the view opened %d windows, not one"
                             % count_serial(r"wm: program window opened 'view'"))
        for leak in (r"shell: no program /bin/",
                     r"shell: running 'static'",
                     r"shell: running 'include'"):
            if count_serial(leak) != 0:
                raise SystemExit("something the viewer was given reached the "
                                 "command line: %s" % leak)
    elif sc == "editcode":
        # v0.1.23: the editor and the highlighter became one program, and a file
        # became able to cross from the boot archive into the store. Four claims
        # are checked below in the order that makes each one depend on the last:
        # a *program* moved the bytes (copy, not a syscall and not a builtin), the
        # editor holds exactly those bytes (its digest matches the copier's and
        # the test's), it paints them as source (the escapes on the wire), and
        # what it wrote back is the file plus one typed key and nothing else (a
        # second copy process, with no memory of the editor, agrees).
        data = spec("rootfs/src/hello.c")
        digest = fnv1a(data)
        typed = "x"                          # the one keystroke the edit is
        DOC_MAX = 2047                       # NOTE_DOC_MAX - 1, src/user/note.c
        # Both numbers are the program's own budget and the specimen's length,
        # mirrored here the way the viewer scenario mirrors VIEW_MAX: a file that
        # stopped fitting would make the assertions below prove a truncation the
        # scenario never claimed to test.
        if len(data) >= DOC_MAX:
            raise SystemExit("the specimen is %d bytes, at the editor's buffer: "
                             "it would have opened truncated" % len(data))

        tw = min(TERM_W, W - 40)
        tx, ty = W - tw - 48, H - TASKBAR_H - TERM_H - 60
        mon.click(tx + tw // 2, ty + TITLE_H // 2)   # raise the terminal

        # The archive holds the specimen and the editor only reads /tmp, so the
        # crossing has to happen first, and it happens in a process the shell
        # launched from a line typed at the prompt.
        mon.type_text("copy /src/hello.c /tmp/work.c\n")
        m = wait_serial(r"wm: program window opened 'copy' pid (\d+)")
        cpid = int(m.group(1))
        m = wait_serial(r"copy: read (\d+) bytes from /src/hello\.c pid (\d+)")
        read_n, rpid = int(m.group(1)), int(m.group(2))
        if read_n != len(data):
            raise SystemExit("the copier read %d bytes out of the archive and the "
                             "archive holds %d" % (read_n, len(data)))
        if rpid != cpid:
            raise SystemExit("the bytes were read by pid %s, the window belongs to "
                             "pid %d" % (rpid, cpid))
        m = wait_serial(r"copy: digest 0x([0-9a-f]{8}) pid (\d+)")
        if int(m.group(2)) != cpid or m.group(1) != digest:
            raise SystemExit("the copier digested %s, the file on disk digests to "
                             "%s (pid %s, window %d)"
                             % (m.group(1), digest, m.group(2), cpid))
        m = wait_serial(r"copy: wrote (\d+) bytes to /tmp/work\.c pid (\d+)")
        if int(m.group(1)) != read_n or int(m.group(2)) != cpid:
            raise SystemExit("the store took %s of the %d bytes it was handed "
                             "(pid %s)" % (m.group(1), read_n, m.group(2)))
        wait_serial(r"shell: copy exited with 0")

        # The editor opens it under the name that was typed, dot and all: a
        # program that had to invent ".txt" would be opening /tmp/work.c.txt and
        # nobody would have asked for that file.
        mark = len(read_log())
        mon.type_text("note work.c\n")
        mark, m = wait_serial_after(mark,
                                    r"wm: program window opened 'note' pid (\d+)")
        epid = int(m.group(1))
        mark, m = wait_serial_after(mark,
                                    r"note: loaded (\d+) bytes from /tmp/work\.c "
                                    r"pid (\d+)")
        if int(m.group(2)) != epid:
            raise SystemExit("the document belongs to pid %s and the window that "
                             "gets the keys is pid %d" % (m.group(2), epid))
        if int(m.group(1)) != len(data):
            raise SystemExit("the editor loaded %s bytes of a %d-byte file"
                             % (m.group(1), len(data)))
        mark, m = wait_serial_after(mark, r"note: digest 0x([0-9a-f]{8}) pid (\d+)")
        if m.group(1) != digest or int(m.group(2)) != epid:
            raise SystemExit("the editor's copy of the file digests to %s and the "
                             "archive's digests to %s (pid %s)"
                             % (m.group(1), digest, m.group(2)))

        # Source, so the colour is on: the same table /bin/view reads, the same
        # escape the kernel says it consumed, and the comment ink in front of the
        # file's own first row. These three are read out of the log as a whole and
        # not from a mark, because they are emitted while the kernel parses the
        # editor's *first* frame - which the program writes before it logs the
        # keyword count that frame was computed from.
        wait_serial(r"wm: screen cleared pid %d" % epid)
        wait_serial(r"wm: colour pid %d" % epid)
        wait_serial(r"\x1c7/\* WindClone")
        mark, m = wait_serial_after(mark,
                                    r"note: highlighted (\d+) keywords in (\d+) "
                                    r"lines pid (\d+)")
        nkey, nlines, kpid = int(m.group(1)), int(m.group(2)), int(m.group(3))
        if kpid != epid or nkey < 5 or nlines < 24:
            raise SystemExit("the editor counted %d keywords in %d lines (pid %s, "
                             "window %d)" % (nkey, nlines, kpid, epid))
        mark, _ = wait_serial_after(mark, r"note: editing /tmp/work\.c pid %d"
                                    % epid)
        if count_serial(r" OCTETS  \d+ MOTS-CLES") < 1:
            raise SystemExit("an editor that paints keywords reports no keyword "
                             "count on its ruler row")

        # Ctrl+E to the end of the first line, and the column has to be the
        # offset the test computes from the file: a line ends where its newline
        # is, in a CRLF checkout as much as in an LF one, because the carriage
        # return is *inside* the line the editor walks. So the caret lands on byte
        # `data.index(b"\n")` either way and the ruler can be held to a number
        # rather than to a range.
        mark = len(read_log())
        mon.key("ctrl-e")
        mark, m = wait_serial_after(mark, r"LN 1 COL (\d+)  (\d+) OCTETS")
        col0, size0 = int(m.group(1)), int(m.group(2))
        if size0 != len(data):
            raise SystemExit("the ruler says the document holds %d bytes and the "
                             "file holds %d" % (size0, len(data)))
        if col0 != data.index(b"\n") + 1:
            raise SystemExit("Ctrl+E left the caret at column %d; the first line "
                             "of the file ends at byte %d"
                             % (col0, data.index(b"\n")))

        # One key, and the frame after it has to be one byte bigger with the
        # caret one cell along: an edit is a size change before it is anything
        # else, and the size here is the document's own count.
        mon.type_text(typed)
        mark, _ = wait_serial_after(
            mark, r"LN 1 COL %d  %d OCTETS" % (col0 + 1, size0 + 1))
        if count_serial(r"NOTEPAD - /tmp/work\.c  \(MODIFIE\)") < 1:
            raise SystemExit("the document grew by a byte and no frame said it "
                             "was unsaved")

        # Ctrl+S. The bytes written have to be the file with the typed character
        # at the offset the ruler reported - not the file, not the file with the
        # character at the end, and not the file with a colour escape leaked into
        # it, which is the failure a highlighter inside an editor is prone to.
        want = data[:col0 - 1] + typed.encode() + data[col0 - 1:]
        want_digest = fnv1a(want)
        mon.key("ctrl-s")
        mark, m = wait_serial_after(mark,
                                    r"note: saved (\d+) bytes to /tmp/work\.c "
                                    r"pid (\d+)")
        saved, spid = int(m.group(1)), int(m.group(2))
        if saved != size0 + 1 or spid != epid:
            raise SystemExit("pid %s wrote %s bytes; the editor is pid %d and its "
                             "document holds %d" % (spid, saved, epid, size0 + 1))
        mark, m = wait_serial_after(mark, r"note: digest 0x([0-9a-f]{8}) pid (\d+)")
        if m.group(1) != want_digest or int(m.group(2)) != spid:
            raise SystemExit("the editor saved bytes digesting to %s, not the %s "
                             "the file plus one typed %r should be (pid %s)"
                             % (m.group(1), want_digest, typed, m.group(2)))
        mark, _ = wait_serial_after(mark, r"NOTEPAD - /tmp/work\.c  \(SAUVE\)")

        mon.key("ctrl-q")
        mark, _ = wait_serial_after(mark, r"note: quit pid %d" % epid)
        mark, _ = wait_serial_after(mark,
                                    r"proc: pid %d died holding the keyboard"
                                    % epid)
        wait_serial(r"shell: note exited with 0")

        # The other half of the proof, in a process that never saw the editor:
        # read the saved file out of the store again and digest it. Same bytes,
        # same number, and a count that a copy of the *original* would not give.
        mark = len(read_log())
        mon.type_text("copy /tmp/work.c /tmp/work2.c\n")
        mark, m = wait_serial_after(mark,
                                    r"copy: read (\d+) bytes from /tmp/work\.c "
                                    r"pid (\d+)")
        back, bpid = int(m.group(1)), int(m.group(2))
        if back != saved:
            raise SystemExit("a second process read %d bytes back from the file "
                             "the editor saved as %d" % (back, saved))
        if bpid == epid or bpid == cpid:
            raise SystemExit("the reader is pid %d, a process that already ran: "
                             "nothing crossed a boundary" % bpid)
        mark, m = wait_serial_after(mark, r"copy: digest 0x([0-9a-f]{8}) pid (\d+)")
        if m.group(1) != want_digest or int(m.group(2)) != bpid:
            raise SystemExit("the bytes in the store digest to %s and the document "
                             "the editor saved digested to %s"
                             % (m.group(1), want_digest))
        mark, _ = wait_serial_after(mark,
                                    r"copy: wrote \d+ bytes to /tmp/work2\.c pid")
        mark, _ = wait_serial_after(mark, r"shell: copy exited with 0")

        # Nothing typed into the editor may have reached the prompt: a leak would
        # turn `x` or `work` into a program the shell went looking for.
        if count_serial(r"wm: program window opened 'note'") != 1:
            raise SystemExit("the editor opened %d windows, not one"
                             % count_serial(r"wm: program window opened 'note'"))
        for leak in (r"shell: no program /bin/", r"shell: running 'x'",
                     r"shell: running 'work'", r"shell: running 'c'"):
            if count_serial(leak) != 0:
                raise SystemExit("something the editor was given reached the "
                                 "command line: %s" % leak)
    elif sc == "edittrunc":
        # v0.1.23's second half, and the one that matters more: an editor that
        # cannot hold a file must not write it. /src/big.c is bigger than a
        # document buffer and smaller than the store, so note loads the first
        # 2047 bytes, says (TRONQUE), and then has to refuse to save. A quiet
        # half-write would have been reported by the store as a clean write of a
        # shorter file - success, wearing the face of a deletion.
        data = spec("rootfs/src/big.c")
        digest = fnv1a(data)
        DOC_MAX = 2047                       # NOTE_DOC_MAX - 1, src/user/note.c
        if len(data) <= DOC_MAX:
            raise SystemExit("the specimen is %d bytes: it fits a document, so "
                             "this scenario would prove nothing" % len(data))
        if len(data) > 4096:
            raise SystemExit("the specimen is %d bytes: past the store's "
                             "TMPFS_FILE_MAX, so copy would refuse it instead of "
                             "the editor refusing to save it" % len(data))

        tw = min(TERM_W, W - 40)
        tx, ty = W - tw - 48, H - TASKBAR_H - TERM_H - 60
        mon.click(tx + tw // 2, ty + TITLE_H // 2)   # raise the terminal

        mark = len(read_log())
        mon.type_text("copy /src/big.c /tmp/big.c\n")
        mark, m = wait_serial_after(mark,
                                    r"copy: read (\d+) bytes from /src/big\.c "
                                    r"pid (\d+)")
        if int(m.group(1)) != len(data):
            raise SystemExit("the copier read %s bytes of a %d-byte file"
                             % (m.group(1), len(data)))
        mark, m = wait_serial_after(mark, r"copy: digest 0x([0-9a-f]{8}) pid (\d+)")
        whole = m.group(1)
        if whole != digest:
            raise SystemExit("the archive copy digests to %s and the file on disk "
                             "to %s" % (m.group(1), digest))
        wait_serial(r"shell: copy exited with 0")

        mark = len(read_log())
        mon.type_text("note big.c\n")
        mark, m = wait_serial_after(mark,
                                    r"wm: program window opened 'note' pid (\d+)")
        epid = int(m.group(1))
        mark, m = wait_serial_after(mark,
                                    r"note: loaded (\d+) bytes from /tmp/big\.c "
                                    r"pid (\d+)")
        if int(m.group(1)) != DOC_MAX or int(m.group(2)) != epid:
            raise SystemExit("the editor holds %s of the %d bytes the store has "
                             "(pid %s, window %d)"
                             % (m.group(1), len(data), m.group(2), epid))
        mark, m = wait_serial_after(mark,
                                    r"note: truncated (\d+) of (\d+) bytes pid "
                                    r"(\d+)")
        if (int(m.group(1)) != DOC_MAX or int(m.group(2)) != len(data)
                or int(m.group(3)) != epid):
            raise SystemExit("the editor says it holds %s of %s bytes; the file is "
                             "%d and a document is %d"
                             % (m.group(1), m.group(2), len(data), DOC_MAX))
        mark, m = wait_serial_after(mark, r"note: digest 0x([0-9a-f]{8}) pid (\d+)")
        if m.group(1) != fnv1a(data[:DOC_MAX]):
            raise SystemExit("the truncated document digests to %s and the first "
                             "%d bytes of the file to %s"
                             % (m.group(1), DOC_MAX, fnv1a(data[:DOC_MAX])))
        mark, _ = wait_serial_after(mark, r"NOTEPAD - /tmp/big\.c  \(TRONQUE\)")
        mark, _ = wait_serial_after(mark, r"wm: colour pid %d" % epid)

        # The buffer is full, so an insert can only be refused - which is a fact
        # about a 2048-byte document and not about truncation, so the edit that
        # makes the document dirty is a deletion: one byte removed is a document
        # that is neither the file nor a whole reading of it.
        mark = len(read_log())
        mon.key("ctrl-d")
        mark, _ = wait_serial_after(mark, r"LN 1 COL 1  %d OCTETS"
                                    % (DOC_MAX - 1))
        # The title row and the ruler row are two rows of the *same* frame, and
        # the title comes first, so this one is read from the whole log: nothing
        # before the deletion could have written (TRONQUE MODIFIE).
        wait_serial(r"NOTEPAD - /tmp/big\.c  \(TRONQUE MODIFIE\)")

        # Ctrl+S: refused, in words, on the serial line and on the screen.
        mon.key("ctrl-s")
        mark, _ = wait_serial_after(
            mark, r"note: refused to save a truncated document pid %d" % epid)
        if count_serial(r"note: saved (\d+) bytes to /tmp/big\.c") != 0:
            raise SystemExit("the editor refused and still saved: a truncated "
                             "document went into the file")

        # Ctrl+Q: the edits are unsaved, and the exit code says so.
        mon.key("ctrl-q")
        mark, m = wait_serial_after(mark, r"note: quit with (\d+) unsaved bytes "
                                           r"pid (\d+)")
        if int(m.group(1)) != DOC_MAX - 1 or int(m.group(2)) != epid:
            raise SystemExit("pid %s quit with %s unsaved bytes, expected %d"
                             % (m.group(2), m.group(1), DOC_MAX - 1))
        wait_serial(r"shell: note exited with 7")

        # And the file is exactly what it was. A copier in a third process reads
        # the whole thing back: the count and the digest from before the editor
        # ever opened it, which is the only proof that the refusal was a refusal
        # and not a save that forgot to say it happened.
        mark = len(read_log())
        mon.type_text("copy /tmp/big.c /tmp/big2.c\n")
        mark, m = wait_serial_after(mark,
                                    r"copy: read (\d+) bytes from /tmp/big\.c "
                                    r"pid (\d+)")
        if int(m.group(1)) != len(data):
            raise SystemExit("the store holds %s bytes where %d went in: the "
                             "refused save wrote anyway"
                             % (m.group(1), len(data)))
        mark, m = wait_serial_after(mark, r"copy: digest 0x([0-9a-f]{8}) pid (\d+)")
        if m.group(1) != whole:
            raise SystemExit("the file digested to %s before the editor and %s "
                             "after it" % (whole, m.group(1)))
        mark, _ = wait_serial_after(mark,
                                    r"copy: wrote \d+ bytes to /tmp/big2\.c pid")
        mark, _ = wait_serial_after(mark, r"shell: copy exited with 0")
        for leak in (r"shell: no program /bin/", r"shell: running 'big'",
                     r"shell: running 'c'"):
            if count_serial(leak) != 0:
                raise SystemExit("something the editor was given reached the "
                                 "command line: %s" % leak)
    elif sc == "net":
        # v0.1.26 -> v0.1.28: the card receives, speaks IP, and now negotiates a
        # lease. The idle-loop client broadcast who-has 10.0.2.2, the emulator's
        # user-mode network answered through the DMA receive ring, we built a full
        # IPv4/ICMP echo and read the reply, then sent a DHCPDISCOVER over UDP and
        # read the server's offer back. Each check guards a false positive.
        m = wait_serial(r"arp: 10\.0\.2\.2 is at ([0-9a-f:]{17}) - gateway resolved")
        got = m.group(1)
        if got == "00:00:00:00:00:00":
            raise SystemExit("resolved gateway MAC was all zeros - the reply was "
                             "parsed from an empty buffer, not a real frame")
        own = re.search(r"e1000: link up, mac ([0-9a-f:]{17})", read_log())
        if own and own.group(1) == got:
            raise SystemExit("gateway MAC equals our own NIC - that is an echo of "
                             "the request, not a reply from another host")
        # The echo reply is the prize: it only parses if a valid IPv4 packet we
        # built (correct header AND ICMP checksums) reached SLIRP and a
        # well-formed answer came back. A bogus ttl means we mis-parsed a frame.
        r = wait_serial(r"icmp: reply from 10\.0\.2\.2: seq (\d+) ttl (\d+) - host reachable")
        ttl = int(r.group(2))
        if ttl <= 0 or ttl > 255:
            raise SystemExit("implausible ttl %d in the icmp reply - the packet "
                             "was likely mis-parsed, not genuinely received" % ttl)
        # The DHCP offer is the first UDP round-trip: it only parses if a
        # well-formed datagram (correct IPv4 + UDP + pseudo-header checksums, our
        # xid echoed) reached the emulator's BOOTP server and it answered. An all
        # zero yiaddr would mean we mis-read the offer rather than got a lease.
        o = wait_serial(r"dhcp: offer (\d+\.\d+\.\d+\.\d+) from 10\.0\.2\.2 - lease understood")
        lease = o.group(1)
        if lease == "0.0.0.0":
            raise SystemExit("dhcp offer carried yiaddr 0.0.0.0 - the server's "
                             "reply was mis-parsed, not a real lease")
        # v0.1.29: the transport layer. The kernel ran a TCP three-way handshake
        # with a real HTTP server (the harness runs one on the host loopback,
        # reachable at 10.0.2.2), sent a GET, and read the status line back. An
        # established marker plus a 200 with a non-zero byte count is the honest
        # proof; a status outside the 100..599 band would mean a mis-parsed body.
        wait_serial(r"tcp: syn-ack received - connection established")
        h = wait_serial(r"http: status (\d+), (\d+) bytes read - first web fetch over tcp")
        status, nbytes = int(h.group(1)), int(h.group(2))
        if not (100 <= status <= 599):
            raise SystemExit("bogus http status %d - the response was mis-parsed" % status)
        if nbytes <= 0:
            raise SystemExit("http fetch reported 0 bytes - nothing was really read back")
    elif sc == "pkg":
        # v0.1.30: the fetched page becomes a package a program can open. The
        # boot-time TCP/HTTP GET now stores the response body in the writable
        # file layer, so the whole claim is: bytes that came off the network land
        # in a real file, /bin/pkg opens that file through the ordinary VFS, and
        # the bytes it paints are the exact bytes this test served. The digest is
        # the witness - a count alone is satisfied by any 43 wrong bytes.
        s = wait_serial(r"http: stored (\d+) body bytes into /net/fetched")
        stored = int(s.group(1))
        if stored <= 0:
            raise SystemExit("the kernel stored 0 body bytes - the response was "
                             "parsed but nothing was handed to the file layer")
        want = fnv1a(PAGE_BODY.encode())

        tw = min(TERM_W, W - 40)
        tx, ty = W - tw - 48, H - TASKBAR_H - TERM_H - 60
        mon.click(tx + tw // 2, ty + TITLE_H // 2)   # raise the terminal
        mon.type_text("pkg\n")

        m = wait_serial(r"wm: program window opened 'pkg' pid (\d+)")
        ppid = int(m.group(1))
        # The file the kernel wrote is readable by a fresh process through the
        # same open()/read() the editor and copier use - the count must match
        # what was stored, which must match the page this harness served.
        r = wait_serial(r"pkg: read (\d+) bytes from /net/fetched pid (\d+)")
        if int(r.group(2)) != ppid:
            raise SystemExit("pkg read on pid %s but its window is pid %s"
                             % (r.group(2), ppid))
        if int(r.group(1)) != stored or int(r.group(1)) != len(PAGE_BODY):
            raise SystemExit("pkg read %s bytes, want %d stored of a %d-byte page"
                             % (r.group(1), stored, len(PAGE_BODY)))
        # The window was drawn as a screen (a form feed), not appended to the
        # terminal's scrollback, and it is the pkg process that drew it.
        wait_serial(r"wm: screen cleared pid %d" % ppid)
        d = wait_serial(r"pkg: rendered fetched page, (\d+) bytes, digest 0x([0-9a-f]{8})")
        if int(d.group(1)) != len(PAGE_BODY):
            raise SystemExit("pkg rendered %s bytes, not the %d-byte page"
                             % (d.group(1), len(PAGE_BODY)))
        if d.group(2) != want:
            raise SystemExit("the page round-tripped to 0x%s but the served body "
                             "digests to 0x%s - the bytes are not the same"
                             % (d.group(2), want))
    elif sc == "browser":
        # v0.1.31: the machine navigates on request and a window says where it
        # went. The boot fetch ("/") belongs to the pkg scenario; here the test
        # asks the shell for a second page, and every assertion is made against
        # bytes only /second.html has - a different body, a different digest.
        # The address bar text is not echoed from the keyboard: a ring-3 program
        # cannot invent it, it reads the kernel's own record over syscall 14, so
        # a bar naming /second.html means the request line, the stored body and
        # the painted page all followed the same navigation.
        s = wait_serial(r"http: stored (\d+) body bytes into /net/fetched")
        if int(s.group(1)) != len(PAGE_BODY):
            raise SystemExit("the boot fetch stored %s bytes, not the %d-byte "
                             "index page - there is nothing to navigate away from"
                             % (s.group(1), len(PAGE_BODY)))

        tw = min(TERM_W, W - 40)
        tx, ty = W - tw - 48, H - TASKBAR_H - TERM_H - 60
        mon.click(tx + tw // 2, ty + TITLE_H // 2)   # raise the terminal
        mon.type_text("fetch /second.html\n")

        n = wait_serial(r"shell: fetch /second\.html - navigating over tcp")
        mark = n.end()
        # Same client, re-armed: a fresh ephemeral port and a new handshake,
        # on the lease and gateway MAC the boot already negotiated.
        mark, _ = wait_serial_after(
            mark, r"tcp: connecting to 10\.0\.2\.2:8080/second\.html")
        mark, h = wait_serial_after(
            mark, r"http: status (\d+), (\d+) bytes read"
                  r" - first web fetch over tcp: /second\.html")
        status = int(h.group(1))
        if not (100 <= status <= 599):
            raise SystemExit("bogus http status %d on the second navigation"
                             % status)
        mark, st = wait_serial_after(
            mark, r"http: stored (\d+) body bytes into /net/fetched")
        stored = int(st.group(1))
        if stored != len(SECOND_BODY):
            raise SystemExit("the navigation stored %s bytes where the server "
                             "holds %d for /second.html" % (st.group(1),
                                                            len(SECOND_BODY)))
        want = fnv1a(SECOND_BODY.encode())
        if want == fnv1a(PAGE_BODY.encode()):
            raise SystemExit("the two served pages digest alike - the test "
                             "cannot tell a navigation from the boot fetch")

        mon.type_text("browser\n")
        mark, m = wait_serial_after(
            mark, r"wm: program window opened 'browser' pid (\d+)")
        ppid = int(m.group(1))
        mark, r2 = wait_serial_after(
            mark, r"browser: read (\d+) bytes from /net/fetched pid (\d+)")
        if int(r2.group(2)) != ppid:
            raise SystemExit("browser read on pid %s but its window is pid %s"
                             % (r2.group(2), ppid))
        if int(r2.group(1)) != stored:
            raise SystemExit("browser read %s bytes, not the %d the navigation "
                             "stored - the file kept the old page"
                             % (r2.group(1), stored))
        mark, _ = wait_serial_after(mark, r"wm: screen cleared pid %d" % ppid)
        mark, _ = wait_serial_after(mark, r"wm: colour pid %d" % ppid)
        mark, d = wait_serial_after(
            mark, r"browser: rendered (\d+) bytes, (\d+) tags, "
                  r"digest 0x([0-9a-f]{8})")
        if int(d.group(1)) != stored:
            raise SystemExit("browser rendered %s bytes but read %d"
                             % (d.group(1), stored))
        if int(d.group(2)) < 4:
            raise SystemExit("the lexer counted %s tags in a document with six "
                             "- it painted the page without reading it"
                             % d.group(2))
        if d.group(3) != want:
            raise SystemExit("the second page round-tripped to 0x%s but the "
                             "served body digests to 0x%s - the bytes are not "
                             "the same" % (d.group(3), want))
        mark, a = wait_serial_after(
            mark, r"browser: address bar shows (\S+)")
        if a.group(1) != "10.0.2.2:8080/second.html":
            raise SystemExit("the address bar showed %s, not the address the "
                             "kernel put on the wire" % a.group(1))
    elif sc == "fileops":
        # v0.1.32: DIR, DEL and REN — cmd.exe's own file verbs — reach the
        # writable store through the VFS. A real file is put there by a program
        # first (copy, the route the store already proved it takes), and then
        # the shell's built-ins list it, move its name, and throw it away. The
        # refusals are the other half of the release: a DEL of an archive file
        # must fail because the initrd is read-only, and a DEL of a name nobody
        # wrote must say "no such file" rather than pretend.
        tw = min(TERM_W, W - 40)
        tx, ty = W - tw - 48, H - TASKBAR_H - TERM_H - 60
        mon.click(tx + tw // 2, ty + TITLE_H // 2)   # raise the terminal
        mon.type_text("copy /src/hello.c /tmp/dst1\n")
        wait_serial(r"copy: wrote \d+ bytes to /tmp/dst1")
        wait_serial(r"shell: copy exited with 0")

        mon.type_text("dir\n")
        m = wait_serial(r"shell: dir shows (\d+) entries, (\d+) bytes"
                        r" in the file layer")
        n1, base = int(m.group(1)), m.end()
        if n1 < 1:
            raise SystemExit("dir showed %d entries after a file was copied "
                             "into the store" % n1)

        # A rename moves the name and nothing else: the store keeps one file.
        mon.type_text("ren /tmp/dst1 /tmp/dst2\n")
        base, _ = wait_serial_after(base,
                                    r"shell: renamed /tmp/dst1 -> /tmp/dst2")
        mon.type_text("dir\n")
        base, m2 = wait_serial_after(base, r"shell: dir shows (\d+) entries")
        if int(m2.group(1)) != n1:
            raise SystemExit("a rename left the file layer holding %s entries, "
                             "want the same %d - bytes moved with the name"
                             % (m2.group(1), n1))

        # The archive is read-only, and that rule reaches the new verbs too:
        # ren refuses before a single byte of /bin/hello is relabelled.
        mon.type_text("ren /bin/hello /tmp/x\n")
        base, _ = wait_serial_after(
            base, r"shell: ren refused /bin/hello - the archive is read-only")

        mon.type_text("del /tmp/dst2\n")
        base, m3 = wait_serial_after(
            base, r"shell: deleted /tmp/dst2 - (\d+) entries left")
        if int(m3.group(1)) != n1 - 1:
            raise SystemExit("deleting the one store file left %s entries, "
                             "want %d" % (m3.group(1), n1 - 1))

        # Deleting what is not yours, and deleting what is not there, are two
        # different answers and the shell gives both in their own words.
        mon.type_text("del /bin/hello\n")
        base, _ = wait_serial_after(
            base, r"shell: del refused /bin/hello - the archive is read-only")
        mon.type_text("del /tmp/ghost\n")
        base, _ = wait_serial_after(
            base, r"shell: del refused /tmp/ghost - no such file")

        # And the file really went: gone is counted, not assumed.
        mon.type_text("dir\n")
        base, m4 = wait_serial_after(base, r"shell: dir shows (\d+) entries")
        if int(m4.group(1)) != n1 - 1:
            raise SystemExit("after the delete dir showed %s entries, want %d"
                             % (m4.group(1), n1 - 1))
    elif sc == "move":
        # v0.1.42: cmd's `move`, the file verb that is not a rename. Two real
        # files are put in the store by copy (the route already proven), then
        # move is shown doing the one thing `ren` refuses: overwriting a name
        # that is already taken. The count is the arithmetic of the claim — a
        # plain move keeps the total, a move that replaces drops it by one, and
        # the file that was in the way is really gone rather than shadowed.
        tw = min(TERM_W, W - 40)
        tx, ty = W - tw - 48, H - TASKBAR_H - TERM_H - 60
        mon.click(tx + tw // 2, ty + TITLE_H // 2)   # raise the terminal
        # Names are lower case on purpose: `type_text` can only spell what the
        # keyboard can produce, and an upper-case letter is a chord the harness
        # would have to synthesise — a path a real user types is not the thing
        # under test here, the store's behaviour is.
        base = len(read_log())
        mon.type_text("copy /src/hello.c /tmp/mv1\n")
        base, _ = wait_serial_after(base, r"copy: wrote \d+ bytes to /tmp/mv1")
        base, _ = wait_serial_after(base, r"shell: copy exited with 0")
        base = len(read_log())
        mon.type_text("copy /src/hello.c /tmp/mv2\n")
        base, _ = wait_serial_after(base, r"copy: wrote \d+ bytes to /tmp/mv2")
        base, _ = wait_serial_after(base, r"shell: copy exited with 0")

        # Two files now sit in the way of the moves to come, and the count is
        # read from the store rather than assumed: /bin/copy has already proven
        # it writes, so at least the two names must be there.
        base = len(read_log())
        mon.type_text("dir\n")
        base, m0 = wait_serial_after(base, r"shell: dir shows (\d+) entries,")
        n0 = int(m0.group(1))
        if n0 < 2:
            raise SystemExit("move: store held %d entries after two copies, "
                             "want at least 2" % n0)

        # A plain move: the destination name is free, so nothing is replaced.
        # Each assertion starts from the size the log had *before* the command
        # was typed, never from the end of the previous match: `shell: moved
        # /tmp/mv2 -> /tmp/mv3` is also the prefix of the `(replaced existing)`
        # line, and matching a prefix that is still being written would let a
        # later claim read a half-finished row.
        base = len(read_log())
        mon.type_text("move /tmp/mv1 /tmp/mv3\n")
        base, _ = wait_serial_after(base,
                                    r"shell: moved /tmp/mv1 -> /tmp/mv3\n")

        # The move that is not a rename: mv3 already exists, and where `ren`
        # would answer EEXIST, `move` drops it and lands the source in its place.
        # The store goes from two files to one, and the line says "replaced".
        base = len(read_log())
        mon.type_text("move /tmp/mv2 /tmp/mv3\n")
        base, _ = wait_serial_after(
            base, r"shell: moved /tmp/mv2 -> /tmp/mv3 \(replaced existing\)\n")
        base = len(read_log())
        mon.type_text("dir\n")
        base, m1 = wait_serial_after(base, r"shell: dir shows (\d+) entries,")
        if int(m1.group(1)) != n0 - 1:
            raise SystemExit("a move that replaced left %s entries, want %d - "
                             "the file in the way must really be gone"
                             % (m1.group(1), n0 - 1))

        # cmd's /Y switch: parsed so the line still resolves to the source and
        # target, not to "/y" and the source. mv3 exists, mv4 does not, so this
        # is a plain move that proves the switch was skipped, not mistaken.
        base = len(read_log())
        mon.type_text("move /y /tmp/mv3 /tmp/mv4\n")
        base, _ = wait_serial_after(base, r"shell: moved /tmp/mv3 -> /tmp/mv4\n")

        # And the two refusals, each in its own words: nothing there to move,
        # and a name that belongs to the read-only archive rather than the store.
        base = len(read_log())
        mon.type_text("move /tmp/ghost /tmp/x\n")
        base, _ = wait_serial_after(
            base, r"shell: move refused /tmp/ghost - no such file\n")
        base = len(read_log())
        mon.type_text("move /bin/hello /tmp/x\n")
        base, _ = wait_serial_after(
            base, r"shell: move refused /bin/hello - the archive is read-only\n")
    elif sc == "fc":
        # v0.1.43: /bin/fc, cmd's File Compare over both halves of the file
        # layer. The claim is the *verdict*, so the test brings its own truth:
        # Python computes FNV-1a over the specimen's bytes and over the exact
        # content /bin/save writes, and the guest's two digests have to answer
        # to it - equal for identical files, different when one byte moves. The
        # equal-length single-byte case pins the first difference at byte 16,
        # line 2, computed here from the file layout rather than trusted from
        # the program's arithmetic.
        data = spec("rootfs/src/hello.c")
        want = fnv1a(data)
        if len(data) >= 4096:
            raise SystemExit("the specimen is %d bytes: past what fc holds, so "
                             "the identical run would report a refusal"
                             % len(data))

        tw = min(TERM_W, W - 40)
        tx, ty = W - tw - 48, H - TASKBAR_H - TERM_H - 60
        mon.click(tx + tw // 2, ty + TITLE_H // 2)   # raise the terminal

        base = len(read_log())
        mon.type_text("copy /src/hello.c /tmp/fca\n")
        base, m = wait_serial_after(base,
                                    r"copy: digest 0x([0-9a-f]{8}) pid")
        if m.group(1) != want:
            raise SystemExit("the copy digests to %s and hello.c to %s - the "
                             "copier itself broke before fc was even asked"
                             % (m.group(1), want))
        base, _ = wait_serial_after(base, r"copy: wrote \d+ bytes to /tmp/fca")
        base, _ = wait_serial_after(base, r"shell: copy exited with 0")

        # Archive versus store: the question a person asks after a copy, and
        # both sides of it read through the same open().
        base = len(read_log())
        mon.type_text("fc /src/hello.c /tmp/fca\n")
        base, m = wait_serial_after(
            base, r"fc: a=(\d+)b=(\d+) bytes 0x([0-9a-f]{8})/0x([0-9a-f]{8})")
        if int(m.group(1)) != len(data) or int(m.group(2)) != len(data):
            raise SystemExit("fc read %s/%s bytes, the file is %d"
                             % (m.group(1), m.group(2), len(data)))
        if m.group(3) != want or m.group(4) != want:
            raise SystemExit("fc digests %s/%s, hello.c digests to %s - one "
                             "side was not read from the bytes it claims"
                             % (m.group(3), m.group(4), want))
        base, _ = wait_serial_after(base, r"fc: no differences encountered")
        base, _ = wait_serial_after(base, r"shell: fc exited with 0")

        # Same file under one name: identical by construction, and the answer
        # must not depend on opening a path twice.
        base = len(read_log())
        mon.type_text("fc /tmp/fca /tmp/fca\n")
        base, _ = wait_serial_after(base, r"fc: no differences encountered")
        base, _ = wait_serial_after(base, r"shell: fc exited with 0")

        # Two store files of equal length, one letter apart in the last word:
        # The shell's input buffer holds SHELL_COLS-1 (61) characters; the
        # command "save write fc1 hello world alpha beta gam" is 41 chars and
        # 8 tokens (both within the caps). Content = "hello world alpha beta gam\n"
        # (27 bytes). The twin ends "gan": they agree for 25 bytes and disagree
        # at index 25 ('m' vs 'n'). No newline before byte 25, so it is line 1.
        fc1_content = "hello world alpha beta gam\n"
        fc2_content = "hello world alpha beta gan\n"
        if len(fc1_content) != len(fc2_content):
            raise SystemExit("the equal-length pair changed size, the byte/line "
                             "assertion below would test the wrong branch")
        diff_i = next(i for i in range(len(fc1_content))
                      if fc1_content[i] != fc2_content[i])
        diff_line = fc1_content[:diff_i].count("\n") + 1
        if diff_i != 25 or diff_line != 1:
            raise SystemExit("the pair no longer first differs at "
                             "byte 25 line 1 (got %d/%d)"
                             % (diff_i, diff_line))
        base = len(read_log())
        mon.type_text("save write fc1 hello world alpha beta gam\n")
        base, m = wait_serial_after(
            base, r"save: wrote (\d+) bytes /tmp/fc1\.txt")
        if int(m.group(1)) != len(fc1_content):
            raise SystemExit("save wrote %s bytes for fc1, the command carries "
                             "%d - the input buffer or arg cap truncated it"
                             % (m.group(1), len(fc1_content)))
        base, _ = wait_serial_after(base, r"shell: save exited with 0")
        base = len(read_log())
        mon.type_text("save write fc2 hello world alpha beta gan\n")
        base, m = wait_serial_after(
            base, r"save: wrote (\d+) bytes /tmp/fc2\.txt")
        if int(m.group(1)) != len(fc2_content):
            raise SystemExit("save wrote %s bytes for fc2, the command carries "
                             "%d - the input buffer or arg cap truncated it"
                             % (m.group(1), len(fc2_content)))
        base, _ = wait_serial_after(base, r"shell: save exited with 0")
        base = len(read_log())
        mon.type_text("fc /tmp/fc1.txt /tmp/fc1.txt\n")
        base, _ = wait_serial_after(base, r"fc: no differences encountered")
        base, _ = wait_serial_after(base, r"shell: fc exited with 0")
        base = len(read_log())
        mon.type_text("fc /tmp/fc1.txt /tmp/fc2.txt\n")
        base, m = wait_serial_after(
            base, r"fc: a=(\d+)b=(\d+) bytes 0x([0-9a-f]{8})/0x([0-9a-f]{8})")
        if m.group(3) == m.group(4):
            raise SystemExit("fc1 and fc2 digest alike (%s) even though they "
                             "differ at byte %d - the two files are the same "
                             "text, so the differing word never reached save"
                             % (m.group(3), diff_i))
        base, _ = wait_serial_after(
            base, r"fc: first difference at byte %d line %d"
                  % (diff_i, diff_line))
        base, _ = wait_serial_after(base, r"fc: comparison failed")
        base, _ = wait_serial_after(base, r"shell: fc exited with 1")

        # A difference on the second line: two scripts whose first line is the
        # same words and whose second differs by one letter, so the sizes match
        # and the location claim - byte 16, line 2 - is checkable.
        # p1 = "@echo one\n@echo two\n" vs p2 = "@echo one\n@echo rwo\n"
        # First difference: byte 16, 't' vs 'r'. Both are 20 bytes.
        p1 = "@echo one\n@echo two\n"
        p2 = "@echo one\n@echo rwo\n"
        if len(p1) != len(p2):
            raise SystemExit("the two scripts changed size, the byte/line "
                             "assertion below would test the wrong branch")
        if p1[:16] != p2[:16] or p1[16] == p2[16]:
            raise SystemExit("the scripts no longer first differ at byte 16")
        base = len(read_log())
        mon.type_text("save script p1 @echo one nl @echo two\n")
        base, _ = wait_serial_after(base, r"save: wrote \d+ bytes /tmp/p1\.bat")
        base, _ = wait_serial_after(base, r"shell: save exited with 0")
        base = len(read_log())
        mon.type_text("save script p2 @echo one nl @echo rwo\n")
        base, _ = wait_serial_after(base, r"save: wrote \d+ bytes /tmp/p2\.bat")
        base, _ = wait_serial_after(base, r"shell: save exited with 0")
        base = len(read_log())
        mon.type_text("fc /tmp/p1.bat /tmp/p2.bat\n")
        base, m = wait_serial_after(
            base, r"fc: a=(\d+)b=(\d+) bytes 0x([0-9a-f]{8})/0x([0-9a-f]{8})")
        if int(m.group(1)) != len(p1) or int(m.group(2)) != len(p2):
            raise SystemExit("fc read %s/%s bytes, the scripts are %d/%d"
                             % (m.group(1), m.group(2), len(p1), len(p2)))
        if m.group(3) != fnv1a(p1.encode()) or \
           m.group(4) != fnv1a(p2.encode()):
            raise SystemExit("fc's digests %s/%s do not answer to Python over "
                             "the exact bytes save wrote (%s/%s)"
                             % (m.group(3), m.group(4),
                                fnv1a(p1.encode()), fnv1a(p2.encode())))
        if m.group(3) == m.group(4):
            raise SystemExit("two files differing at byte 16 digest alike - "
                             "one side is not being read")
        base, _ = wait_serial_after(base,
                                    r"fc: first difference at byte 16 line 2")
        base, _ = wait_serial_after(base, r"fc: comparison failed")
        base, _ = wait_serial_after(base, r"shell: fc exited with 1")

        # Unequal sizes: one file is a proper prefix of the other, so the
        # first "difference" is actually the end of the shorter file.
        # save write sm one → content "one\n" (4 bytes)
        # save script bg one nl two → content "one\ntwo\n" (8 bytes, nl becomes newline)
        # They share the first 4 bytes ("one\n"), so diff_at = min = 4.
        # Bytes[0..3] of the longer file = "one\n" → 1 newline → line 2.
        base = len(read_log())
        mon.type_text("save write sm one\n")
        base, _ = wait_serial_after(base, r"save: wrote \d+ bytes /tmp/sm\.txt")
        base, _ = wait_serial_after(base, r"shell: save exited with 0")
        base = len(read_log())
        mon.type_text("save script bg one nl two\n")
        base, _ = wait_serial_after(base, r"save: wrote \d+ bytes /tmp/bg\.bat")
        base, _ = wait_serial_after(base, r"shell: save exited with 0")
        sm_len = 4   # len("one\n")
        bg_len = 8   # len("one\ntwo\n")
        base = len(read_log())
        mon.type_text("fc /tmp/bg.bat /tmp/sm.txt\n")
        base, m = wait_serial_after(
            base, r"fc: sizes differ by (\d+) bytes")
        if int(m.group(1)) != bg_len - sm_len:
            raise SystemExit("fc says the sizes are %s apart, %d - %d = %d"
                             % (m.group(1), bg_len, sm_len,
                                bg_len - sm_len))
        base, _ = wait_serial_after(
            base, r"fc: first difference at byte 4 line 2")
        base, _ = wait_serial_after(base, r"fc: comparison failed")
        base, _ = wait_serial_after(base, r"shell: fc exited with 1")

        # The refusals, in their own words: a name nobody wrote, one word
        # where two belong, and a file past what this program holds. The last
        # one matters most for an archive: /bin/hello is an 8.4 KiB ELF, and a
        # compare that quietly stopped at 4096 could call the truncated halves
        # "identical" - so the size is said in full and the answer is a refusal,
        # never a verdict. fc is the first program whose exit code *is* the
        # answer.
        base = len(read_log())
        mon.type_text("fc /bin/hello /tmp/fca\n")
        base, _ = wait_serial_after(
            base, r"fc: too big, \d{4,} bytes at /bin/hello")
        base, _ = wait_serial_after(base, r"shell: fc exited with 2")
        base = len(read_log())
        mon.type_text("fc /tmp/nofc /tmp/fca\n")
        base, _ = wait_serial_after(base, r"fc: cannot open /tmp/nofc")
        base, _ = wait_serial_after(base, r"shell: fc exited with 2")
        base = len(read_log())
        mon.type_text("fc /tmp/fca\n")
        base, _ = wait_serial_after(base, r"fc: usage fc")
        base, _ = wait_serial_after(base, r"shell: fc exited with 2")
    elif sc == "find":
        # v0.1.44: /bin/find, cmd's string search over the file layer fc
        # already reads. The test brings its own truth: Python splits the
        # specimen's bytes into lines and counts which carry the pattern,
        # case-sensitive and case-folded, so the guest's "N matches" answers
        # to the harness — not a count trusted from the program.
        data = spec("rootfs/src/hello.c")
        file_lines = data.split(b"\n")
        # A trailing newline leaves an empty element that cmd does not count.
        if file_lines and file_lines[-1] == b"":
            file_lines = file_lines[:-1]

        def match_lines(needle, ci):
            hits = []
            for idx, ln in enumerate(file_lines):
                s = ln.decode("ascii", "replace")
                n = needle
                if ci:
                    s = s.lower()
                    n = n.lower()
                if n in s:
                    hits.append(idx + 1)  # 1-based line number
            return hits

        cs = match_lines("answer", False)   # lowercase 'answer'
        ci = match_lines("answer", True)    # folds ANSWER too
        if len(cs) != 1 or len(ci) != 2:
            raise SystemExit(
                "the specimen no longer has 1 case-sensitive and 2 case-"
                "insensitive 'answer' lines (got %d/%d), so the counts "
                "asserted below would be wrong" % (len(cs), len(ci)))

        tw = min(TERM_W, W - 40)
        tx, ty = W - tw - 48, H - TASKBAR_H - TERM_H - 60
        mon.click(tx + tw // 2, ty + TITLE_H // 2)  # raise the terminal

        # Plain search: exit 0, count == cs.
        base = len(read_log())
        mon.type_text("find answer /src/hello.c\n")
        base, _ = wait_serial_after(base, r"find: /src/hello\.c")
        base, m = wait_serial_after(base, r"find: (\d+) matches")
        if int(m.group(1)) != len(cs):
            raise SystemExit("find counted %s matches, the bytes give %d"
                             % (m.group(1), len(cs)))
        base, _ = wait_serial_after(base, r"shell: find exited with 0")

        # /i folds case: ANSWER on the #define joins the set, proving the
        # fold ran rather than the plain search being re-reported.
        base = len(read_log())
        mon.type_text("find /i answer /src/hello.c\n")
        base, m = wait_serial_after(base, r"find: (\d+) matches")
        if int(m.group(1)) != len(ci):
            raise SystemExit("/i counted %s, bytes give %d" % (m.group(1), len(ci)))
        base, _ = wait_serial_after(base, r"shell: find exited with 0")

        # /n prefixes each hit with its line number: the first cs hit is
        # lines[cs[0]-1]; the output must start with "find: <n>: ".
        base = len(read_log())
        mon.type_text("find /n answer /src/hello.c\n")
        base, _ = wait_serial_after(base, r"find: %d: " % cs[0])
        base, _ = wait_serial_after(base, r"shell: find exited with 0")

        # Nothing matches: count 0, exit 1 — 'not found' is the real answer.
        base = len(read_log())
        mon.type_text("find zzzqqx /src/hello.c\n")
        base, m = wait_serial_after(base, r"find: (\d+) matches")
        if int(m.group(1)) != 0:
            raise SystemExit("found %s copies of a string not in the file"
                             % m.group(1))
        base, _ = wait_serial_after(base, r"shell: find exited with 1")

        # A store file with hand-built known content: /n numbers matches and
        # /v inverts.  Content: "one\ntwo\nthree\ntwo\n" (4 lines, 17 bytes).
        fnd = "one\ntwo\nthree\ntwo\n"
        base = len(read_log())
        mon.type_text("save script fnd one nl two nl three nl two\n")
        base, m = wait_serial_after(
            base, r"save: wrote (\d+) bytes /tmp/fnd\.bat")
        if int(m.group(1)) != len(fnd):
            raise SystemExit("save wrote %s, command carries %d"
                             % (m.group(1), len(fnd)))
        base, _ = wait_serial_after(base, r"shell: save exited with 0")
        # /n two: lines 2 and 4 carry 'two'.
        base = len(read_log())
        mon.type_text("find /n two /tmp/fnd.bat\n")
        base, _ = wait_serial_after(base, r"find: /tmp/fnd\.bat")
        base, _ = wait_serial_after(base, r"find: 2: two")
        base, _ = wait_serial_after(base, r"find: 4: two")
        base, _ = wait_serial_after(base, r"find: 2 matches")
        base, _ = wait_serial_after(base, r"shell: find exited with 0")
        # /v two: lines 1 and 3 lack it → "find: one" + "find: three".
        base = len(read_log())
        mon.type_text("find /v two /tmp/fnd.bat\n")
        base, _ = wait_serial_after(base, r"find: one")
        base, _ = wait_serial_after(base, r"find: three")
        base, _ = wait_serial_after(base, r"find: 2 matches")
        base, _ = wait_serial_after(base, r"shell: find exited with 0")

        # The refusals: usage, unopenable, too big — each exit 2.
        base = len(read_log())
        mon.type_text("find /src/hello.c\n")
        base, _ = wait_serial_after(base, r"find: usage find")
        base, _ = wait_serial_after(base, r"shell: find exited with 2")
        base = len(read_log())
        mon.type_text("find answer /tmp/nofind\n")
        base, _ = wait_serial_after(base, r"find: cannot open /tmp/nofind")
        base, _ = wait_serial_after(base, r"shell: find exited with 2")
        base = len(read_log())
        mon.type_text("find answer /bin/hello\n")
        base, _ = wait_serial_after(
            base, r"find: too big, \d{4,} bytes at /bin/hello")
        base, _ = wait_serial_after(base, r"shell: find exited with 2")
    elif sc == "sort":
        # v0.1.45: /bin/sort, cmd's SORT over the same file layer. The claim is
        # an *order*, so the harness computes it: Python sorts the exact lines
        # save will write, and every printed line has to appear in that order —
        # chaining wait_serial_after forces the sequence, not just the presence.
        srt_lines = ["banana", "apple", "cherry", "date"]
        srt_content = "\n".join(srt_lines) + "\n"        # 25 bytes
        asc = sorted(srt_lines)                          # byte order
        if asc != ["apple", "banana", "cherry", "date"]:
            raise SystemExit("python's byte sort changed, the assertion below "
                             "would prove the wrong order")

        tw = min(TERM_W, W - 40)
        tx, ty = W - tw - 48, H - TASKBAR_H - TERM_H - 60
        mon.click(tx + tw // 2, ty + TITLE_H // 2)  # raise the terminal

        base = len(read_log())
        mon.type_text("save script srt banana nl apple nl cherry nl date\n")
        base, m = wait_serial_after(
            base, r"save: wrote (\d+) bytes /tmp/srt\.bat")
        if int(m.group(1)) != len(srt_content):
            raise SystemExit("save wrote %s bytes for srt, the command carries "
                             "%d - truncated by a cap"
                             % (m.group(1), len(srt_content)))
        base, _ = wait_serial_after(base, r"shell: save exited with 0")

        # Ascending: header, then each line in the harness's own sorted order,
        # then the count and exit 0.
        base = len(read_log())
        mon.type_text("sort /tmp/srt.bat\n")
        base, _ = wait_serial_after(base, r"sort: /tmp/srt\.bat\n")
        for w in asc:
            base, _ = wait_serial_after(base, r"sort: %s\n" % re.escape(w))
        base, m = wait_serial_after(base, r"sort: (\d+) lines sorted")
        if int(m.group(1)) != len(asc):
            raise SystemExit("sort counted %s lines, the file has %d"
                             % (m.group(1), len(asc)))
        base, _ = wait_serial_after(base, r"shell: sort exited with 0")

        # /r reads the same sort backwards: assert the exact reverse sequence.
        base = len(read_log())
        mon.type_text("sort /r /tmp/srt.bat\n")
        base, _ = wait_serial_after(base, r"sort: /tmp/srt\.bat \(reverse\)")
        for w in reversed(asc):
            base, _ = wait_serial_after(base, r"sort: %s\n" % re.escape(w))
        base, _ = wait_serial_after(base, r"shell: sort exited with 0")

        # The prefix rule: when one line is a prefix of another the shorter one
        # comes first (ab before abc) — the other branch of the comparator.
        base = len(read_log())
        mon.type_text("save script srt2 abc nl ab\n")
        base, _ = wait_serial_after(base, r"save: wrote \d+ bytes /tmp/srt2\.bat")
        base, _ = wait_serial_after(base, r"shell: save exited with 0")
        base = len(read_log())
        mon.type_text("sort /tmp/srt2.bat\n")
        base, _ = wait_serial_after(base, r"sort: ab\n")
        base, _ = wait_serial_after(base, r"sort: abc\n")
        base, _ = wait_serial_after(base, r"sort: 2 lines sorted")
        base, _ = wait_serial_after(base, r"shell: sort exited with 0")

        # The refusals, each exit 2: too many lines (the specimen source is 55,
        # past SORT_LINES), too big (the 8.4 KiB ELF), an unreadable name, and a
        # command line with no file. A partial sort is never reported as an answer.
        base = len(read_log())
        mon.type_text("sort /src/hello.c\n")
        base, _ = wait_serial_after(
            base, r"sort: too many lines, \d+ at /src/hello\.c")
        base, _ = wait_serial_after(base, r"shell: sort exited with 2")
        base = len(read_log())
        mon.type_text("sort /bin/hello\n")
        base, _ = wait_serial_after(
            base, r"sort: too big, \d{4,} bytes at /bin/hello")
        base, _ = wait_serial_after(base, r"shell: sort exited with 2")
        base = len(read_log())
        mon.type_text("sort /tmp/ghost\n")
        base, _ = wait_serial_after(base, r"sort: cannot open /tmp/ghost")
        base, _ = wait_serial_after(base, r"shell: sort exited with 2")
        base = len(read_log())
        mon.type_text("sort\n")
        base, _ = wait_serial_after(base, r"sort: usage sort")
        base, _ = wait_serial_after(base, r"shell: sort exited with 2")
    elif sc == "certutil":
        # v0.1.46: /bin/certutil, cmd's hash tool. A wrong SHA-256 still prints
        # 64 plausible hex digits, so presence proves nothing — the harness
        # brings its own oracle: hashlib over the exact bytes save wrote, and
        # over the specimen the archive serves, and the guest's digest line has
        # to equal them digit for digit. hello.c (1874 bytes, 29 whole blocks +
        # 18) also exercises the streaming block-chaining, not just one block.
        import hashlib

        tw = min(TERM_W, W - 40)
        tx, ty = W - tw - 48, H - TASKBAR_H - TERM_H - 60
        mon.click(tx + tw // 2, ty + TITLE_H // 2)  # raise the terminal

        cu_text = "hello world\n"                   # save write joins + newline
        cu_hex = hashlib.sha256(cu_text.encode()).hexdigest()

        base = len(read_log())
        mon.type_text("save write cu hello world\n")
        base, m = wait_serial_after(
            base, r"save: wrote (\d+) bytes /tmp/cu\.txt")
        if int(m.group(1)) != len(cu_text):
            raise SystemExit("save wrote %s bytes for cu, the command carries "
                             "%d - truncated by a cap"
                             % (m.group(1), len(cu_text)))
        base, _ = wait_serial_after(base, r"shell: save exited with 0")

        # Store file: header, then the digest the harness computed itself.
        base = len(read_log())
        mon.type_text("certutil -hashfile /tmp/cu.txt\n")
        base, _ = wait_serial_after(base, r"certutil: /tmp/cu\.txt")
        base, _ = wait_serial_after(base, r"certutil: sha256 %s" % re.escape(cu_hex))
        base, _ = wait_serial_after(base, r"shell: certutil exited with 0")

        # The algorithm word is accepted and selects the same hash.
        base = len(read_log())
        mon.type_text("certutil -hashfile /tmp/cu.txt sha256\n")
        base, _ = wait_serial_after(base, r"certutil: sha256 %s" % re.escape(cu_hex))
        base, _ = wait_serial_after(base, r"shell: certutil exited with 0")

        # The archive, streamed across many blocks: the digest must answer to
        # hashlib over the very bytes rootfs/src/hello.c contributes to the
        # initrd — the same file the guest opens as /src/hello.c.
        arc = spec("rootfs/src/hello.c")
        arc_hex = hashlib.sha256(arc).hexdigest()
        base = len(read_log())
        mon.type_text("certutil -hashfile /src/hello.c\n")
        base, _ = wait_serial_after(base, r"certutil: /src/hello\.c")
        base, _ = wait_serial_after(base, r"certutil: sha256 %s" % re.escape(arc_hex))
        base, _ = wait_serial_after(base, r"shell: certutil exited with 0")

        # The refusals, each exit 2: a broken switch, no file at all, a relative
        # path, an unreadable name, and an algorithm this program does not have.
        # An unsupported name is refused rather than silently hashed as sha256.
        base = len(read_log())
        mon.type_text("certutil -bogus /tmp/cu.txt\n")
        base, _ = wait_serial_after(base, r"certutil: usage certutil")
        base, _ = wait_serial_after(base, r"shell: certutil exited with 2")
        base = len(read_log())
        mon.type_text("certutil\n")
        base, _ = wait_serial_after(base, r"certutil: usage certutil")
        base, _ = wait_serial_after(base, r"shell: certutil exited with 2")
        base = len(read_log())
        mon.type_text("certutil -hashfile cu.txt\n")
        base, _ = wait_serial_after(base, r"certutil: a path is absolute")
        base, _ = wait_serial_after(base, r"shell: certutil exited with 2")
        base = len(read_log())
        mon.type_text("certutil -hashfile /tmp/nocuv\n")
        base, _ = wait_serial_after(base, r"certutil: cannot open /tmp/nocuv")
        base, _ = wait_serial_after(base, r"shell: certutil exited with 2")
        base = len(read_log())
        mon.type_text("certutil -hashfile /tmp/cu.txt md5\n")
        base, _ = wait_serial_after(base, r"certutil: algorithm not supported")
        base, _ = wait_serial_after(base, r"shell: certutil exited with 2")
    elif sc == "unzip":
        # v0.1.36: /bin/unzip unpacks a real archive the way Windows does, folder
        # names included. The archive is the single source of truth: the test
        # opens it with Python's own zipfile and asks, per member in archive
        # order, what the format says the file is. A safe member - flat or nested
        # - must come out byte for byte, its digest and count on a line whose
        # label may be the tail of a long path. A folder record (a name ending in
        # '/') is skipped: this store has no directories. And a name that climbs
        # above the destination (a '..' component) is refused, never written - the
        # zip-slip a naive extractor would happily plant outside the folder.
        import io
        import zipfile as _zf

        raw = spec("rootfs/src/sample.zip")
        zf = _zf.ZipFile(io.BytesIO(raw))
        plan = []                        # (kind, name, method, data), archive order
        for info in zf.infolist():
            name = info.filename
            if info.is_dir() or name.endswith("/"):
                plan.append(("folder", name, 0, b""))
            elif ".." in name.split("/"):
                plan.append(("slip", name, 0, b""))
            else:
                plan.append(("file", name, info.compress_type, zf.read(name)))
        if sum(1 for k, *_ in plan if k == "file") < 2:
            raise SystemExit("the specimen must hold more than one real member")
        if not any(k == "folder" for k, *_ in plan):
            raise SystemExit("the specimen lost its folder record")
        if not any(k == "slip" for k, *_ in plan):
            raise SystemExit("the specimen lost its path-traversal member")
        safe = [(n, m, d) for k, n, m, d in plan if k == "file"]
        if not any(m == 8 for _, m, _ in safe):
            raise SystemExit("the specimen has no deflated member")
        if not any("/" in n for n, _, _ in safe):
            raise SystemExit("the specimen has no nested member to unpack")

        tw = min(TERM_W, W - 40)
        tx, ty = W - tw - 48, H - TASKBAR_H - TERM_H - 60
        mon.click(tx + tw // 2, ty + TITLE_H // 2)   # raise the terminal

        mark = len(read_log())
        mon.type_text("unzip /src/sample.zip /tmp\n")
        mark, m = wait_serial_after(
            mark, r"unzip: read (\d+) bytes from /src/sample\.zip pid")
        if int(m.group(1)) != len(raw):
            raise SystemExit("the archive is %d bytes on disk and the reader "
                             "saw %s" % (len(raw), m.group(1)))

        total = 0
        files = folders = slips = 0
        for kind, name, method, data in plan:        # archive order, not ours
            if kind == "folder":
                mark, _ = wait_serial_after(
                    mark, r"unzip: skipped folder %s" % re.escape(name))
                folders += 1
                continue
            if kind == "slip":
                mark, _ = wait_serial_after(
                    mark, r"unzip: refused entry %s" % re.escape(name))
                slips += 1
                continue
            mark, m = wait_serial_after(
                mark, r"unzip: extracted 0x([0-9a-f]{8}) (\d+) b %s"
                      % re.escape(name))
            if int(m.group(2)) != len(data) or m.group(1) != fnv1a(data):
                raise SystemExit("%s (method %d): unpacked %s bytes digest %s, "
                                 "want %d %s"
                                 % (name, method, m.group(2), m.group(1),
                                    len(data), fnv1a(data)))
            total += len(data)
            files += 1

        mark, m = wait_serial_after(
            mark, r"unzip: (\d+) files, (\d+) bytes into /tmp, refused (\d+)")
        if (int(m.group(1)) != files or int(m.group(2)) != total
                or int(m.group(3)) != slips):
            raise SystemExit("summary says %s files %s bytes refused %s, "
                             "want %d %d %d"
                             % (m.group(1), m.group(2), m.group(3),
                                files, total, slips))
        wait_serial(r"shell: unzip exited with 0")

        # The nested path is the whole claim of this release, and a digest on a
        # log line is only half of it: another program must be able to open the
        # file at <dest>/<name-with-a-slash> out of the store and answer with the
        # bytes the archive's own format says are there. fixed.bl is read back
        # first to free a slot (the store is eight files), then the nested,
        # deflated member - so the round trip proves a compressed file living
        # under a directory name exists nowhere except inside a working extractor.
        mark = len(read_log())
        mon.type_text("del /tmp/fixed.bl\n")
        mark, _ = wait_serial_after(mark, r"shell: deleted /tmp/fixed\.bl -")

        nested = next((n for n, m, d in safe if "/" in n and m == 8), None)
        if nested is None:
            raise SystemExit("no nested deflated member to read back")
        want = next(fnv1a(d) for n, m, d in safe if n == nested)
        mark = len(read_log())
        mon.type_text("copy /tmp/%s /tmp/verify\n" % nested)
        mark, m = wait_serial_after(mark, r"copy: digest 0x([0-9a-f]{8}) pid")
        if m.group(1) != want:
            raise SystemExit("%s read back through the store digests to %s and "
                             "the archive member to %s"
                             % (nested, m.group(1), want))
        wait_serial(r"shell: copy exited with 0")

        # And the nested file can go, which only succeeds because it lives in the
        # writable store under that exact path: the archive's own copy would be
        # refused as read-only, and a flat-name store could not have held it.
        mark = len(read_log())
        mon.type_text("del /tmp/%s\n" % nested)
        mark, _ = wait_serial_after(
            mark, r"shell: deleted /tmp/%s -" % re.escape(nested))


    elif sc == "img":
        # v0.1.34: /bin/imgview decodes a 24-bit bitmap and pushes its pixels
        # through the kernel's raster channel (RS 'R' + WW HH + RGB24). The file
        # on disk is the source of truth: the test decodes it the way the format
        # says - little-endian header, four-byte row stride, bottom-up rows, BGR
        # to RGB - builds the exact top-down byte stream the window should show,
        # and requires the guest's printed digest to equal fnv1a over those bytes.
        # That is the whole claim: not "a count appeared" but "these pixels, in
        # this order, landed in the framebuffer", and it catches every classic
        # decoder bug (wrong stride shifts rows, forgotten flip mirrors them, a
        # skipped channel swap recolours the picture).
        import struct as _st

        raw = spec("rootfs/src/logo.bmp")
        if raw[:2] != b"BM":
            raise SystemExit("the specimen lost its BM signature")
        dataoff = _st.unpack("<I", raw[10:14])[0]
        w, h = _st.unpack("<ii", raw[18:26])
        bpp = _st.unpack("<H", raw[28:30])[0]
        comp = _st.unpack("<I", raw[30:34])[0]
        topdown = h < 0
        if topdown:
            h = -h
        if bpp != 24 or comp != 0 or not (1 <= w <= 128 and 1 <= h <= 80):
            raise SystemExit("specimen shape changed unexpectedly: %dbpp "
                             "comp=%d %dx%d" % (bpp, comp, w, h))
        stride = (w * 3 + 3) & ~3
        want = bytearray()
        for y in range(h):                       # top-down, as the blit wants it
            fr = y if topdown else (h - 1 - y)
            base = dataoff + fr * stride
            for x in range(w):
                b, g, r = raw[base + x * 3: base + x * 3 + 3]
                want += bytes((r, g, b))         # BGR on disk -> RGB to screen
        want = bytes(want)

        tw = min(TERM_W, W - 40)
        tx, ty = W - tw - 48, H - TASKBAR_H - TERM_H - 60
        mon.click(tx + tw // 2, ty + TITLE_H // 2)   # raise the terminal

        mark = len(read_log())
        mon.type_text("imgview /src/logo.bmp\n")
        mark, m = wait_serial_after(mark, r"imgview: read (\d+) bytes pid")
        if int(m.group(1)) != len(raw):
            raise SystemExit("the bitmap is %d bytes on disk and the reader "
                             "saw %s" % (len(raw), m.group(1)))

        # The kernel says a whole record landed, keyed to the dimensions it
        # parsed itself - they must equal the file's own header, and the file's
        # header must equal what the test decoded.
        mark, m = wait_serial_after(
            mark, r"wm: image (\d+)x(\d+) pid")
        if (int(m.group(1)), int(m.group(2))) != (w, h):
            raise SystemExit("kernel raster %sx%s, file header %dx%d"
                             % (m.group(1), m.group(2), w, h))

        mark, m = wait_serial_after(
            mark, r"imgview: image (\d+)x(\d+) digest 0x([0-9a-f]{8})")
        if (int(m.group(1)), int(m.group(2))) != (w, h):
            raise SystemExit("imgview reported %sx%s, the file is %dx%d"
                             % (m.group(1), m.group(2), w, h))
        if m.group(3) != fnv1a(want):
            raise SystemExit("decoded pixels digest to %s, the file decodes to "
                             "%s - a decode, order or channel bug"
                             % (m.group(3), fnv1a(want)))
        wait_serial(r"shell: imgview exited with 0")
    else:
        raise SystemExit("unknown scenario: %s" % sc)


MARKERS = {
    # the screenshot scenario types a command first, so it doubles as the
    # proof that keystrokes reach the window that has focus
    "shot":      ["shell: running 'hello'", "shell: hello ran and returned"],
    "focus":     ["mouse: first packet decoded"],
    "minimize":  ["wm: window minimised", "wm: window restored from taskbar"],
    "maxmax":    ["wm: window maximised", "wm: window restored"],
    "close":     ["wm: window closed", "wm: window opened from icon"],
    "start":     ["wm: start menu opened", "wm: show desktop",
                  "wm: windows restored", "wm: start menu closed"],
    # a click on a row the kernel listed itself: the name reaches the log
    # twice, once as the menu's content and once as a program that ran
    "menu":      ["wm: menu row", "shell: launched 'hello' from the start menu",
                  "shell: hello ran and returned", "hello from ring 3"],
    "menushot":  ["wm: start menu opened", "wm: menu rows"],
    # the pid itself is cross-checked inside the scenario (spawn line versus
    # what the program printed); these are the two claims around it
    "menuproc":  ["shell: launched 'whoami' from the start menu",
                  "proc: refused to kill pid 1 (the shell owns it)"],
    # the interleave itself is asserted in the scenario (a paint strictly
    # between two live rounds); these are the honest async lines around it
    "live":      ["shell: launched 'hold' from the start menu",
                  "shell: hold started, pid",
                  "shell: reaped pid", "shell: hold exited with 5"],
    # the open / route / finish / survive-then-close ordering is asserted in the
    # scenario against the dynamic pid; these are the marker lines it turns on
    "progwin":   ["wm: program window opened 'hold' pid",
                  "wm: routed output pid",
                  "wm: program window pid",
                  "wm: program window closed 'hold'"],
    # the small-band vs full-screen arithmetic is asserted in the scenario against
    # the measured areas; these are the two marker lines the claim turns on
    "damage":    ["gfx: back buffer", "gfx: flip"],
    # a sub-screen flip strictly between two hold rounds is asserted in the
    # scenario; these markers prove the back buffer and the flip log exist
    "progdamage": ["gfx: back buffer", "gfx: flip"],
    # the grow/shrink-to-floor arithmetic is asserted in the scenario against
    # the kernel's own resize line; this marker is what that line turns on
    "resize":    ["wm: window resized"],
    # the per-position mask is asserted in the scenario; this marker proves the
    # kernel reports which border the pointer is hovering (the resize cursor)
    "cursor":    ["wm: cursor resize"],
    # the half/quarter geometry is asserted in the scenario against the rect the
    # kernel logged; this marker is what a snap turns on
    "snap":      ["wm: snapped"],
    # the byte counts, the pid boundary and the note's own text between the
    # reader's two lines are all asserted in the scenario; these markers are the
    # three steps that claim rests on - a write that landed, a read that came
    # back from another process, and a store the boot said is RAM-only
    "filewrite": ["tmpfs: writable store live", "save: wrote",
                  "save: read back"],
    # the byte counts, the command count and the three refusals are asserted in
    # the scenario; these are the lines the claim turns on - a script a program
    # wrote, a shell that opened it, and a launched program that knows it came
    # from a file rather than from a keyboard
    "batchfile": ["tmpfs: writable store live", "save: wrote",
                  "shell: batch file", "shell: running 'echo' from a batch file",
                  "shell: batch ran", "shell: batch refused nested script"],
    # the byte counts, the expansion proof, the echo-state machine and the
    # environment list are all asserted in the scenario; these are the lines the
    # claim turns on - a resolved `%X%`, an `@`-stripped `echo off`, a SET that
    # folded its name, `rem` answered as a comment, and ECHO reporting its state
    "batch2":    ["save: wrote", "shell: batch exec 'echo hi'",
                  "shell: batch exec 'echo off'", "shell: batch echo off",
                  "shell: batch set X=hi", "shell: batch remark skipped",
                  "shell: batch comment skipped",
                  "shell: batch echo state off", "shell: batch echo state on",
                  "shell: batch env 1 variables", "shell: batch env X=hi"],
    # v0.1.38: prompt-time SET and TYPE. The definition, the environment
    # listing, the malformed refusal, the file dump and the missing-file
    # refusal are all asserted in the scenario; these are the lines the claim
    # turns on - a variable folded to upper case at the keyboard, a bare SET
    # that lists it back, a `type` whose 17-byte body the kernel wrote itself
    # out of the store /bin/save had filled
    "promptset": ["shell: prompt set PZ=bonjour",
                  "shell: prompt env 1 variables", "shell: prompt env PZ=bonjour",
                  "shell: prompt set refused - want NAME=VALUE",
                  "save: wrote", "shell: type /tmp/hi.bat (17 bytes)",
                  "bonjour le monde", "shell: type no such file /tmp/nope.bat"],
    # v0.1.39: %VAR% expansion on a typed line. The exec row is written only
    # when a defined name actually substituted, so its presence proves the
    # expansion ran; the launch row proves the resolved line still dispatched.
    "promptvar": ["shell: prompt set PZ=bonjour",
                  "shell: prompt exec 'echo bonjour'",
                  "shell: running 'echo' from the prompt"],
    # v0.1.40: caret escape. `^%` blocks expansion (the resolved row keeps a
    # literal %pz% even though pz is set) and a bare `^` vanishes (a^b -> ab).
    "caret":     ["shell: prompt set PZ=bonjour",
                  "shell: prompt exec 'echo %pz%'",
                  "shell: prompt exec 'echo ab'",
                  "shell: running 'echo' from the prompt"],
    # the line's text, the byte total across reads and the absence of any leak to
    # the prompt are all asserted in the scenario; these are the lines the claim
    # turns on - a program that asked for the keyboard, slept until a key landed,
    # got the whole word typed to it, and handed the keys back when it finished
    "typing":    ["shell: running 'keys' from the prompt",
                  "keys: waiting for keys pid",
                  "asleep until a keystroke wakes it",
                  "keys: line 'hello there' len 11",
                  "died holding the keyboard; the console has it back",
                  "shell: keys exited with 0",
                  "shell: running 'echo' from the prompt"],
    # the caret positions, the byte counts, the repaint-after-key frames and the
    # exact bytes read back by a third process are all asserted in the scenario;
    # these are the lines the claim turns on - a file opened out of the store, a
    # window painted as a screen, a save the kernel agreed to, and an editor that
    # ended with everything on its screen inside its file
    "editing":   ["shell: running 'note' from the prompt",
                  "note: loaded", "wm: screen cleared pid",
                  "note: editing /tmp/demo.txt", "note: saved",
                  "note: quit", "shell: note exited with 0",
                  "save: read back"],
    # the token-by-token colour, the page-downs and the status row are asserted in
    # the scenario; these are the lines the claim turns on - a source file out of
    # the initrd, a window the parser moved the pen inside, and a viewer that
    # counted the tokens it was about to colour before it painted any
    "highlight": ["shell: running 'view' from the prompt",
                  "wm: program window opened 'view'",
                  "view: opened /src/hello.c", "wm: screen cleared pid",
                  "wm: colour pid", "view: highlighted",
                  "view: quit", "shell: view exited with 0"],
    # the digests, the caret offset, the byte counts and the cross-process read
    # back are all asserted in the scenario; these are the lines the claim turns
    # on - a program that moved a file from the archive into the store, the editor
    # that loaded those exact bytes, painted them as source, and wrote back the
    # document a keystroke had changed
    "editcode":  ["shell: running 'copy' from the prompt", "copy: read",
                  "copy: digest 0x", "copy: wrote",
                  "wm: program window opened 'note'", "note: loaded",
                  "note: highlighted", "wm: colour pid",
                  "note: editing /tmp/work.c", "note: saved",
                  "shell: note exited with 0"],
    # the truncation arithmetic and the state of the file afterwards are asserted
    # in the scenario; these are the lines the claim turns on - a document that
    # says it is not the whole file, a save that was refused out loud, an exit
    # code that names the unsaved bytes, and the digest that proves the refused
    # file was never written to at all
    "edittrunc": ["copy: digest 0x", "note: loaded 2047 bytes",
                  "note: truncated 2047 of", "NOTEPAD - /tmp/big.c  (TRONQUE)",
                  "note: refused to save a truncated document",
                  "note: quit with 2046 unsaved bytes",
                  "shell: note exited with 7"],
    # the size match and the clock ranges are asserted in the scenario; these are
    # the lines the claim turns on - a wallpaper painted once into a cached
    # surface, and a taskbar showing the CMOS chip's own decoded time and date
    "chrome":    ["wm: wallpaper cached", "rtc: hardware clock",
                  "rtc: date"],
    # the gateway MAC being neither zeros nor our own address, the icmp ttl being
    # plausible, the dhcp yiaddr being non-zero, and the http status/byte count
    # being real are asserted in the scenario; these are the lines the
    # receive-ping-lease-fetch claim turns on - the client armed on top of the
    # driver, ARP resolved, an ICMP echo SLIRP answered, a DHCP lease offered over
    # UDP, and a TCP three-way handshake plus an HTTP 200 read back from a server
    "net":       ["net: arp/icmp/dhcp client armed", "arp: who-has 10.0.2.2",
                  "arp: 10.0.2.2 is at", "gateway resolved",
                  "icmp: echo request to 10.0.2.2", "host reachable",
                  "dhcp: discover broadcast", "lease understood",
                  "tcp: syn-ack received - connection established",
                  "first web fetch over tcp"],
    # v0.1.30: the fetch lands in the file layer and a program opens it. The body
    # bytes are stored, /bin/pkg reads them back through the VFS and paints them
    # in its own window; the rendered digest (checked against the served page
    # inside the scenario) is what makes "the same bytes" a fact and not a hope.
    "pkg":       ["http: stored", "into /net/fetched",
                  "wm: program window opened 'pkg'",
                  "pkg: read", "bytes from /net/fetched",
                  "wm: screen cleared", "pkg: rendered fetched page"],
    # v0.1.31: a navigation, not a download. The shell queues /second.html, the
    # same TCP client re-arms and fetches it, the store holds the new bytes, and
    # /bin/browser paints them under the address the kernel itself recorded (all
    # cross-checked - digest, byte counts, pid, bar text - inside the scenario).
    "browser":   ["shell: fetch /second.html", "tcp: connecting to 10.0.2.2:8080",
                  "first web fetch over tcp: /second.html",
                  "http: stored", "wm: program window opened 'browser'",
                  "browser: read", "wm: colour pid",
                  "browser: rendered", "browser: address bar shows"],
    # v0.1.32: the shell's own file verbs. copy (a program) puts a real file in
    # the store, then dir lists it, ren moves its name, del throws it away - and
    # the archive answers both mutations with its own refusal.
    "fileops":   ["copy: wrote", "bytes to /tmp/dst1",
                  "shell: dir shows", "shell: renamed /tmp/dst1 -> /tmp/dst2",
                  "shell: ren refused /bin/hello - the archive is read-only",
                  "shell: deleted /tmp/dst2", "entries left",
                  "shell: del refused /bin/hello - the archive is read-only",
                  "shell: del refused /tmp/ghost - no such file"],
    # v0.1.42: cmd's `move`, the verb that is not a rename. The arithmetic (the
    # entry count dropping by one when a name is overwritten) is asserted inside
    # the scenario; these are the lines the claim turns on - a plain move, the
    # move that replaces what was in the way and says so, the /Y switch parsed
    # rather than mistaken for the source, and the two refusals in their own words.
    "move":      ["copy: wrote", "shell: moved /tmp/mv1 -> /tmp/mv3",
                  "shell: moved /tmp/mv2 -> /tmp/mv3 (replaced existing)",
                  "shell: moved /tmp/mv3 -> /tmp/mv4",
                  "shell: move refused /tmp/ghost - no such file",
                  "shell: move refused /bin/hello - the archive is read-only"],
    # v0.1.43: /bin/fc, cmd's File Compare. The digest equality against Python's
    # own FNV-1a and the byte/line arithmetic are asserted inside the scenario;
    # these are the verdict lines the claim turns on - identical (archive vs its
    # store copy, and a file against itself), the single-byte difference located
    # on the second line, the size difference with its shared prefix, and the
    # two refusals, each ending in the exit code that IS the answer.
    "fc":        ["copy: digest", "fc: a=", "bytes 0x",
                  "fc: no differences encountered", "shell: fc exited with 0",
                  "fc: first difference at byte 25 line 1",
                  "fc: first difference at byte 16 line 2",
                  "fc: sizes differ by", "fc: first difference at byte 4 line 2",
                  "fc: comparison failed",
                  "shell: fc exited with 1",
                  "fc: cannot open /tmp/nofc", "fc: usage fc",
                  "fc: too big", "shell: fc exited with 2"],
    # v0.1.44: /bin/find, cmd's string search. The scenario cross-checks the
    # counts against Python's own line-splitting of the specimen; these are the
    # lines the claim turns on - header, each count, every exit code, and the
    # three refusals that prove a bad path or a too-big ELF is never silently
    # skipped (which would poison the verdict).
    "find":      ["find: /src/hello.c", "find: 1 matches",
                  "find: 2 matches", "find: 0 matches",
                  "shell: find exited with 0",
                  "shell: find exited with 1",
                  "find: /tmp/fnd.bat", "find: 2: two",
                  "find: 4: two", "find: one", "find: three",
                  "find: usage find", "find: cannot open /tmp/nofind",
                  "find: too big", "shell: find exited with 2"],
    # v0.1.45: /bin/sort, cmd's SORT. The scenario asserts the exact ordering
    # chain (Python's own byte sort) rather than mere presence; these are the
    # lines the claim turns on - the header, each sorted line, the reverse
    # header and its reversed sequence, the prefix-rule pair, the count, and
    # the four exit-2 refusals (too many lines, too big, unopenable, no file).
    "sort":      ["sort: /tmp/srt.bat", "sort: apple", "sort: banana",
                  "sort: cherry", "sort: date", "sort: 4 lines sorted",
                  "sort: /tmp/srt.bat (reverse)", "sort: /tmp/srt2.bat",
                  "sort: ab", "sort: abc", "sort: 2 lines sorted",
                  "shell: sort exited with 0", "sort: too many lines",
                  "sort: too big", "sort: cannot open /tmp/ghost",
                  "sort: usage sort", "shell: sort exited with 2"],
    # v0.1.46: /bin/certutil, cmd's hash tool. The exact digest is matched in
    # the scenario against hashlib; these are the lines the claim turns on -
    # each header, the sha256 tag on the digest line, both exit-0 successes,
    # and the five exit-2 refusals (broken switch, no file, relative path,
    # unopenable name, unsupported algorithm).
    "certutil":  ["certutil: /tmp/cu.txt", "certutil: sha256",
                  "certutil: /src/hello.c", "shell: certutil exited with 0",
                  "certutil: usage certutil", "certutil: a path is absolute",
                  "certutil: cannot open /tmp/nocuv",
                  "certutil: algorithm not supported",
                  "shell: certutil exited with 2"],
    # v0.1.36: a real ZIP unpacked, folder names included. The strict byte/digest
    # comparisons are in the scenario; these are the lines the claim turns on -
    # the archive read whole, each safe member out byte for byte (flat and nested,
    # stored and deflated) with the digest leading the line, the folder record
    # skipped, the path-traversal member refused rather than written, a summary
    # that counts them, and the nested file read back out of the store.
    "unzip":     ["unzip: read", "b readme.txt", "b notes.md",
                  "b packed.dat", "b fixed.bl", "b stored.bl",
                  "unzip: skipped folder demo/", "b demo/__init__.py",
                  "unzip: refused entry ../evil.txt",
                  "unzip: 6 files,", "refused 1",
                  "shell: unzip exited with 0",
                  "shell: deleted /tmp/fixed.bl",
                  "shell: copy exited with 0",
                  "shell: deleted /tmp/demo/__init__.py"],
    # v0.1.34: a bitmap decoded to real pixels. The strict digest match is in the
    # scenario; these are the lines the claim turns on - the file read whole, the
    # kernel accepting a whole RGB24 record into a window (its own dimensions, not
    # the program's say-so), and the viewer's decoded-pixel digest line.
    "img":       ["imgview: read", "wm: image 48x32 pid",
                  "imgview: image 48x32 digest 0x",
                  "shell: imgview exited with 0"],
}


def main():
    iso, sc = sys.argv[1], sys.argv[2]
    shot = sys.argv[3] if len(sys.argv) > 3 else None

    for f in (LOG, MON, QMP):
        if os.path.exists(f):
            os.remove(f)

    # v0.1.29: the net scenario now fetches a page over TCP. SLIRP exposes the
    # host's loopback to the guest at 10.0.2.2, so a real HTTP server on the
    # host becomes the peer the kernel handshakes with and GETs from. Serving a
    # known page (a fixed word we can look for) keeps the claim honest: the test
    # passes only if the kernel completes a TCP handshake and reads a 200 back.
    web = None
    webdir = None
    if sc in ("net", "pkg", "browser"):
        webdir = tempfile.mkdtemp(prefix="wcweb-")
        with open(os.path.join(webdir, "index.html"), "w") as fh:
            fh.write(PAGE_BODY)
        if sc == "browser":
            with open(os.path.join(webdir, "second.html"), "w") as fh:
                fh.write(SECOND_BODY)
        web = subprocess.Popen(
            [sys.executable, "-m", "http.server", "8080",
             "--bind", "127.0.0.1", "--directory", webdir],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1.0)

    proc = subprocess.Popen(
        ["qemu-system-x86_64", "-cdrom", iso, "-m", "512", "-display", "none",
         "-rtc", "base=2026-08-21T05:37:13,clock=vm",
         "-device", "e1000,netdev=n0", "-netdev", "user,id=n0",
         "-no-reboot", "-serial", "file:%s" % LOG,
         "-monitor", "unix:%s,server,nowait" % MON,
         "-qmp", "unix:%s,server,nowait" % QMP],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    mon = None

    try:
        # A CI runner boots this a lot slower than a laptop does, so the
        # wait is generous; the desktop marker is what we wait for, not time.
        for _ in range(120):
            time.sleep(0.5)
            if os.path.exists(LOG) and "gfx: desktop composited" in open(LOG).read():
                break
        else:
            raise SystemExit("desktop never came up (%s)" % LOG)

        text = open(LOG, errors="replace").read()
        m = re.search(r"gfx: desktop composited (\d+)x(\d+)", text)
        if not m:
            raise SystemExit("serial banner has no framebuffer size")
        W, H = int(m.group(1)), int(m.group(2))

        mon = Monitor(MON)
        # Connected up front, not on first use: a guest that came up is a guest
        # whose QMP socket exists, and finding that out in the middle of an edit
        # sequence would look like an editor bug.
        mon.qmp = Qmp(QMP)
        if sc == "shot":
            tw = min(TERM_W, W - 40)
            tx = W - tw - 48
            ty = H - TASKBAR_H - TERM_H - 60
            mon.click(tx + tw // 2, ty + TITLE_H // 2)   # raise the terminal
            mon.type_text("hello")
            time.sleep(0.4)
            mon.key("ret")
            time.sleep(1.5)
            mon.shot(shot or "/tmp/windclone-desktop.png")
        else:
            scenario_actions(sc, mon, W, H)
            if shot:
                mon.shot(shot)
    finally:
        # An emulator that outlives the test slows every later run — and
        # with it the guest clock, which silently breaks timing assertions.
        if mon is not None:
            mon.close()
            if mon.qmp is not None:
                mon.qmp.close()
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        time.sleep(0.3)
        if web is not None:
            web.terminate()
            try:
                web.wait(timeout=5)
            except subprocess.TimeoutExpired:
                web.kill()
        if webdir is not None:
            for name in ("index.html", "second.html"):
                try:
                    os.remove(os.path.join(webdir, name))
                except OSError:
                    pass
            try:
                os.rmdir(webdir)
            except OSError:
                pass

    text = open(LOG, errors="replace").read()
    missing = [w for w in MARKERS[sc] if w not in text]
    if missing:
        raise SystemExit("scenario %s: missing %s\n--- serial tail ---\n%s"
                         % (sc, missing, "\n".join(text.splitlines()[-14:])))
    print("    ok: %s -> %s" % (sc, " | ".join(MARKERS[sc])))


if __name__ == "__main__":
    main()
