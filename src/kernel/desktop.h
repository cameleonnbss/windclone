/* WindClone — desktop shell (Phase 8).
 *
 * desktop_frame() paints one full frame — wallpaper, icons, windows,
 * taskbar, cursor — and applies whatever the mouse did since the last one:
 * click to focus, drag by the title bar, minimise / maximise / close from
 * the title-bar buttons, restore from the taskbar, reopen from an icon.
 * Called after boot and whenever the keyboard or the mouse has something.
 *
 * Does nothing when there is no framebuffer (BIOS text-mode fallback).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_DESKTOP_H
#define WINDCLONE_DESKTOP_H

void desktop_frame(void);

/* Open a real window on the desktop for a program the shell just launched, bound
 * to its surface slot and pid. It is a full window — draggable title bar,
 * minimise / maximise / close, a taskbar button — whose body is that program's
 * own text (shell_program_draw). The window is created here and owned by the
 * desktop, but it *outlives the process*: when the program finishes the window
 * stays, showing the exit code, until the user closes it. `slot < 0` is a no-op
 * (the shell had no window room and told the program to use the terminal). Called
 * from the shell's launch path, never from an IRQ. */
void wm_open_program(int slot, int pid);

#endif
