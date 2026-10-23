/* WindClone — desktop shell (Phase 8).
 *
 * desktop_draw() paints one full frame: wallpaper, taskbar, window. It is
 * called once after boot today; the moment there is a mouse and a clock
 * that ticks, it becomes the redraw entry point of a real compositor.
 *
 * Does nothing when there is no framebuffer (BIOS text-mode fallback).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_DESKTOP_H
#define WINDCLONE_DESKTOP_H

void desktop_draw(void);

#endif
