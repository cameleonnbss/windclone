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
               the whole screen, a pure mouse move after it flips only a
               cursor-sized band (a fraction of the pixels), and a click on a
               window body is structural and flips the whole screen again
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

Exit status 0 = every step did what the serial log says it did.
SPDX-License-Identifier: MIT
"""
import json
import os
import re
import socket
import subprocess
import sys
import time

MON = os.environ.get("WM_MON", "/tmp/wc-wmmon")
LOG = os.environ.get("WM_LOG", "/tmp/wc-wm.log")
# A second control channel, used only for key chords; see Qmp below.
QMP = os.environ.get("WM_QMP", "/tmp/wc-wmqmp")

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


def wait_flip_after(mark, pred, timeout=15.0):
    """Poll the serial log for a `gfx: flip <n> rects, <area> px` line that
    appears *after* byte offset `mark` and whose (rects, area) satisfies `pred`.
    Damage is a measured claim, not a fixed string, so the test reads the number
    out of the log and compares it - offsetting by `mark` keeps the boot flip and
    earlier frames from satisfying a later step's predicate."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(LOG, errors="replace") as fh:
                tail = fh.read()[mark:]
        except OSError:
            tail = ""
        for n, a in re.findall(r"gfx: flip (\d+) rects, (\d+) px", tail):
            if pred(int(n), int(a)):
                return (int(n), int(a))
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

    if sc == "focus":
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
        text = open(LOG, errors="replace").read()
        if "gfx: back buffer" not in text:
            raise SystemExit("no back buffer was allocated - direct-to-framebuffer fallback")
        flips = [(int(n), int(a)) for n, a in
                 re.findall(r"gfx: flip (\d+) rects, (\d+) px", text)]
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
        mark = len(text)
        mon.move_rel(40, 30)
        small = wait_flip_after(mark, lambda n, a: a * 8 < W * H)
        if small is None:
            raise SystemExit("a mouse move still repainted the whole screen")
        # a click on a window body is a structural change (z-order/raise), so the
        # honesty of the smaller blit depends on this still flipping everything.
        mark = len(open(LOG, errors="replace").read())
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
}


def main():
    iso, sc = sys.argv[1], sys.argv[2]
    shot = sys.argv[3] if len(sys.argv) > 3 else None

    for f in (LOG, MON, QMP):
        if os.path.exists(f):
            os.remove(f)

    proc = subprocess.Popen(
        ["qemu-system-x86_64", "-cdrom", iso, "-m", "512", "-display", "none",
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

    text = open(LOG, errors="replace").read()
    missing = [w for w in MARKERS[sc] if w not in text]
    if missing:
        raise SystemExit("scenario %s: missing %s\n--- serial tail ---\n%s"
                         % (sc, missing, "\n".join(text.splitlines()[-14:])))
    print("    ok: %s -> %s" % (sc, " | ".join(MARKERS[sc])))


if __name__ == "__main__":
    main()
