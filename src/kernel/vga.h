/* WindClone — VGA text-mode driver (80x25 character cell buffer).
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_VGA_H
#define WINDCLONE_VGA_H

#include <stddef.h>

/* Map the classic VGA text buffer. `hw` is the (HHDM-translated)
 * physical address 0xB8000; passing NULL disables kprint().
 *
 * force_text_mode is false when the kernel is driving a linear
 * framebuffer (Phase 8): the legacy CRTC programming would disable the
 * very LFB we draw into. */
void vga_init(void *hw, int force_text_mode);
int  vga_available(void);
void kprint(const char *s);
void kprint_len(const char *s, size_t n);   /* for non-NUL-terminated buffers */
void kclear(void);

#endif
