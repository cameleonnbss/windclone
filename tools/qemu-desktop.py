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
# Characters a scenario may type that are not letters or digits. These are
# QEMU's monitor key names, and the kernel's own scancode table (keyboard.c
# map_plain) has to agree: 0x33 ',', 0x34 '.', 0x35 '/' are mapped there, so a
# `sendkey slash` really does land as a '/' in the shell's input buffer.
KEYMAP = {" ": "spc", "\n": "ret", "/": "slash", ".": "dot", "-": "minus",
          "_": "underscore"}
# Key names the *human* monitor has been proved to deliver: the letters and
# digits every scenario has always typed, plus the spellings KEYMAP maps to.
# Anything else goes through QMP, because `sendkey` accepts an unknown name,
# replies nothing, and sends no scancode - a Backspace spelled "bspace" looked
# exactly like a guest that had lost the key. QMP answers, so a name QEMU does
# not know fails the run at the keystroke instead of at the assertion 25
# seconds later.
HMP_OK = set("abcdefghijklmnopqrstuvwxyz0123456789") | {
    "spc", "ret", "slash", "dot", "minus", "underscore"}

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
    elif sc == "unzip":
        # v0.1.35: /bin/unzip unpacks a real ZIP archive, compressed included.
        # The archive is the single source of truth: the test opens it with
        # Python's own zipfile and asks, per member, what the format says the
        # file is - a flat-named member in method 0 or 8 must come out byte for
        # byte with the digest of those decoded bytes, and a member whose name
        # carries a '/' must be refused, because this program writes flat names
        # only and a malicious archive must not be able to choose the path.
        # Then the file verbs from v0.1.32 finish the claim on a *deflated*
        # member: a second program re-digests it out of the store and a delete
        # frees it, which only works because unzip wrote it into the writable
        # store and not the read-only archive it came from.
        import io
        import zipfile as _zf

        raw = spec("rootfs/src/sample.zip")
        zf = _zf.ZipFile(io.BytesIO(raw))
        flat, nested = [], []
        for info in zf.infolist():
            if info.is_dir():
                continue
            if "/" in info.filename:
                nested.append(info.filename)
            else:
                flat.append((info.filename, info.compress_type,
                             zf.read(info.filename)))
        if not flat or not nested:
            raise SystemExit("the specimen must hold both a flat member and a "
                             "nested one (flat %d, nested %d)"
                             % (len(flat), len(nested)))
        methods = {m for _, m, _ in flat}
        if 8 not in methods:
            raise SystemExit("the specimen has no deflated member: the whole "
                             "point of this release is method 8")

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
        for name, method, data in flat:          # archive order, not ours
            mark, m = wait_serial_after(
                mark, r"unzip: extracted %s (\d+) bytes digest 0x([0-9a-f]{8})"
                      % re.escape(name))
            if int(m.group(1)) != len(data) or m.group(2) != fnv1a(data):
                raise SystemExit("%s (method %d): unpacked %s bytes digest %s, "
                                 "want %d %s"
                                 % (name, method, m.group(1), m.group(2),
                                    len(data), fnv1a(data)))
            total += len(data)
        for name in nested:
            mark, _ = wait_serial_after(
                mark, r"unzip: refused entry %s" % re.escape(name))

        mark, m = wait_serial_after(
            mark, r"unzip: (\d+) files, (\d+) bytes into /tmp, refused (\d+)")
        if (int(m.group(1)) != len(flat) or int(m.group(2)) != total
                or int(m.group(3)) != len(nested)):
            raise SystemExit("summary says %s files %s bytes refused %s, "
                             "want %d %d %d"
                             % (m.group(1), m.group(2), m.group(3),
                                len(flat), total, len(nested)))
        wait_serial(r"shell: unzip exited with 0")

        # The round trip goes through the member that only exists if inflate
        # worked - and through the canned-table block in preference to the
        # zlib-produced ones, because those three bytes of BTYPE=01 are what no
        # other path in this decoder touches. A different program opens it out
        # of the store and must answer with the digest the archive's own format
        # says the file is.
        packed = None
        for name, method, data in flat:
            if method == 8:
                if packed is None:
                    packed = (name, data)
                if name == "fixed.bl":
                    packed = (name, data)
                    break
        if packed is None:
            raise SystemExit("no deflated member to read back")
        name, data = packed
        mark = len(read_log())
        mon.type_text("copy /tmp/%s /tmp/verify\n" % name)
        mark, m = wait_serial_after(mark, r"copy: digest 0x([0-9a-f]{8}) pid")
        if m.group(1) != fnv1a(data):
            raise SystemExit("%s read back through the store digests to %s and "
                             "the archive member to %s"
                             % (name, m.group(1), fnv1a(data)))
        wait_serial(r"shell: copy exited with 0")

        # And it can go, which only succeeds because it lives in the writable
        # store: the archive's own copy of the same name would be refused as
        # read-only.
        mark = len(read_log())
        mon.type_text("del /tmp/%s\n" % name)
        mark, _ = wait_serial_after(
            mark, r"shell: deleted /tmp/%s -" % re.escape(name))

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
    # v0.1.35: a real ZIP unpacked, compressed members included. The strict
    # byte/digest comparisons are in the scenario; these are the lines the claim
    # turns on - the archive read whole, each flat member out byte for byte
    # including the three deflated ones (dynamic trees, the canned tables and a
    # stored block), the nested name refused because a program must not get its
    # path from an archive, a summary that counts both, and the shell's exit.
    "unzip":     ["unzip: read", "unzip: extracted readme.txt",
                  "unzip: extracted notes.md",
                  "unzip: extracted packed.dat",
                  "unzip: extracted fixed.bl",
                  "unzip: extracted stored.bl",
                  "unzip: refused entry dir/inner.txt",
                  "unzip: 5 files, 1011 bytes into /tmp, refused 1",
                  "shell: unzip exited with 0",
                  "shell: deleted /tmp/fixed.bl"],
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
