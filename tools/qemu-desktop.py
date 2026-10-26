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

Exit status 0 = every step did what the serial log says it did.
SPDX-License-Identifier: MIT
"""
import os
import re
import socket
import subprocess
import sys
import time

MON = os.environ.get("WM_MON", "/tmp/wc-wmmon")
LOG = os.environ.get("WM_LOG", "/tmp/wc-wm.log")

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


class Monitor:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.settimeout(5)
        self.s.connect(path)
        time.sleep(0.2)
        self.s.recv(4096)

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
        self.cmd("sendkey %s" % name)

    def type_text(self, text):
        """Type through the emulated 8042: every key is a real IRQ1."""
        for ch in text:
            self.key({" ": "spc", "\n": "ret"}.get(ch, ch))
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
}


def main():
    iso, sc = sys.argv[1], sys.argv[2]
    shot = sys.argv[3] if len(sys.argv) > 3 else None

    for f in (LOG, MON):
        if os.path.exists(f):
            os.remove(f)

    proc = subprocess.Popen(
        ["qemu-system-x86_64", "-cdrom", iso, "-m", "512", "-display", "none",
         "-no-reboot", "-serial", "file:%s" % LOG,
         "-monitor", "unix:%s,server,nowait" % MON],
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
