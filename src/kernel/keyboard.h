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

/* --- the keyboard a program holds --------------------------------------
 * Everything above is the console's: one ring, drained by the idle loop into
 * the command line. This second ring is what lets a *program* read its own
 * keyboard, and the rule it implements is one sentence: the process that calls
 * read(0) owns the keys, and keeps them only while its window is on top.
 *
 * Why a claim rather than a route: an editor that cannot read the keyboard is a
 * viewer, and every earlier release refused fd 0 with -ENOSYS because the shell
 * really did own every IRQ1 byte. The owner is a single pid, not a table,
 * because only one window can be focused and only the focused one is offered
 * keys at all. A second program that reads takes the claim away, and the first
 * one's blocked read comes back answered with 0 bytes rather than sleeping on
 * a keyboard somebody else is now holding.
 *
 * Called from the idle loop (push) and from the syscall handler (pop, claim).
 * Never from IRQ context: the IRQ path still only ever fills the console ring
 * above, so a program cannot be handed a byte the decoder has not finished
 * turning into a character. */
void keyboard_own(int pid);        /* claim for `pid`; a new owner starts empty */
int  keyboard_owner(void);         /* the claiming pid, or 0 for the console    */
int  keyboard_push(int ch);        /* 1 = landed in the owner's ring, 0 = no owner */
int  keyboard_pop(int pid, void *buf, int len); /* bytes copied, 0 if not owner */

#endif
