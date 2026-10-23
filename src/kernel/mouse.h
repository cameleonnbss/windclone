/* WindClone — PS/2 mouse (Phase 8 input).
 *
 * The mouse hangs off the *second* channel of the same 8042 controller the
 * keyboard uses: commands go out with a 0xD4 prefix ("next byte is for the
 * aux device"), data comes back on IRQ12, and the slave PIC line it arrives
 * on only works while the cascade (IRQ2) is unmasked. Those three details
 * are where most PS/2 mouse drivers die.
 *
 * Packets are three bytes: flags, dx, dy. dx/dy are 8 bits plus a sign bit
 * carried in the flags byte, and y points *up*, so screen coordinates need
 * y -= dy. This file only decodes and accumulates; drawing belongs to the
 * desktop.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_MOUSE_H
#define WINDCLONE_MOUSE_H

int  mouse_init(void);      /* 0 = aux channel answered; arms IRQ12 */

void mouse_irq(void);       /* called from the IRQ12 dispatch */

/* Consume the "something moved/changed" flag; 1 = redraw worth doing. */
int  mouse_take_dirty(void);

int  mouse_x(void);
int  mouse_y(void);
int  mouse_left(void);      /* left button currently down */

#endif
