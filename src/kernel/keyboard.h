/* WindClone — PS/2 keyboard (8042 controller).
 * IRQ1 calls keyboard_irq(); keyboard_init() arms the port.
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_KEYBOARD_H
#define WINDCLONE_KEYBOARD_H

void keyboard_init(void);
void keyboard_irq(void);
int  keyboard_presses(void);
int  keyboard_releases(void);

/* Next decoded character for the shell, or -1 when the queue is empty.
 * Printable ASCII, plus '\n' (Enter) and '\b' (Backspace). */
int  keyboard_take_char(void);

#endif
