/* WindClone — desktop shell (Phase 8).
 *
 * desktop_frame() paints one full frame: wallpaper, icons, window, taskbar,
 * cursor — and applies whatever the mouse did since the last one (drag the
 * window by its title bar). Called after boot and whenever the mouse driver
 * reports a change; a real compositor will redraw only the damaged region.
 *
 * Does nothing when there is no framebuffer (BIOS text-mode fallback).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_DESKTOP_H
#define WINDCLONE_DESKTOP_H

void desktop_frame(void);

#endif
