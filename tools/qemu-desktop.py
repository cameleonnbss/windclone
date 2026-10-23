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
    start      DEMARRER hides every window; pressing it again brings them back

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
        mon.click(START_X + START_W // 2, H - TASKBAR_H + 22)   # DEMARRER
        mon.click(START_X + START_W // 2, H - TASKBAR_H + 22)   # ...and back
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
    "start":     ["wm: show desktop", "wm: windows restored"],
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
