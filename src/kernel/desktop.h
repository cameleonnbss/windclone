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

#endif
