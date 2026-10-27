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

/* The pid of the program whose window is on top, or 0 when the keyboard belongs
 * to the console. The idle loop asks this for every keystroke: a program that has
 * claimed the keyboard (by calling read(0)) keeps receiving keys only while this
 * says its pid, so clicking past its window — minimising it, raising another, or
 * the process dying while its window stays to show an exit code — hands typing
 * straight back to the prompt. Transfers are one-way here: this reports the
 * window manager's opinion and never takes or grants a claim itself. */
int desktop_key_pid(void);

#endif
