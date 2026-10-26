/* WindClone — the command line (Phase 5, inside a Phase 8 window).
 *
 * A shell is three things: a line editor, a parser, and a way to start
 * programs. This one has all three on purpose — `hello` really does load
 * /bin/hello from the initrd and run it in ring 3, then comes back to the
 * prompt. Text is kept in a character grid and redrawn every frame, so the
 * window can be dragged without losing the scrollback.
 *
 * The fourth thing is new, and it is the one that changed what the other three
 * can promise: the shell owns a slot in the process table (pid 1, see proc.h),
 * so a program started here is its *child* rather than a guest call — it has a
 * pid, it can spawn and wait and kill, and it can be killed.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_SHELL_H
#define WINDCLONE_SHELL_H

/* Seed the banner and the first prompt. */
void shell_init(void);

/* Feed one key from keyboard_take_char(). */
void shell_key(int c);

/* Start /bin/<name> as the shell's own child: the same lookup, the same
 * terminal as its stdout and the same exit-code line as a word typed at the
 * prompt — only the log says who asked. This is what the desktop's start menu
 * calls, so a mouse click runs a program instead of describing one. Blocks
 * until the scheduler has run that program's whole group to the end: it is
 * called from the idle loop, never from an IRQ. 0 = the program ran and came
 * back (its code, -9 included, is what the shell printed); -1 = no such
 * program, the loader refused it, or it never returned. */
int shell_launch(const char *name);

/* Paint the grid at (x,y); `width` is for wrapping the input line. */
void shell_draw(int x, int y, int width);

/* True when the shell changed something (so the compositor can redraw). */
int  shell_dirty(void);
void shell_clear_dirty(void);

#endif
