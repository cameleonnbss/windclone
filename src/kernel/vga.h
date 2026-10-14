/* WindClone — VGA text-mode driver (80x25 character cell buffer).
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_VGA_H
#define WINDCLONE_VGA_H

#include <stddef.h>

/* Map the classic VGA text buffer. `hw` is the (HHDM-translated)
 * physical address 0xB8000; passing NULL disables kprint(). */
void vga_init(void *hw);
int  vga_available(void);
void kprint(const char *s);
void kclear(void);

#endif
