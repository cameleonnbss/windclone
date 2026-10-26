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
 * terminal as its stdout and the same pid/exit-code lines as a word typed at
 * the prompt — only the log says who asked. This is what the desktop's start
 * menu calls, so a mouse click runs a program instead of describing one. It is
 * *asynchronous*: it places the process in the table and returns while the
 * program is still running, so the idle loop keeps painting and the terminal
 * keeps receiving the program's output; the exit-code line arrives later, from
 * the visit that runs it to its end. Called from the idle loop, never from an
 * IRQ. 0 = the program was launched (the code is not known yet, only the pid);
 * -1 = no such program, the loader refused it, or there was no room in the
 * table. */
int shell_launch(const char *name);

/* Paint the grid at (x,y); `width` is for wrapping the input line. */
void shell_draw(int x, int y, int width);

/* True when the shell changed something (so the compositor can redraw). */
int  shell_dirty(void);
void shell_clear_dirty(void);

/* --- a program's own window ------------------------------------------
 * Each launched program gets a text surface of its own, bound to the pid the
 * shell was given. The console sink routes that running program's write(1) here
 * instead of into the terminal, so its output belongs to a window the user can
 * move, overlap and — once it finishes — close. The desktop drives the window
 * chrome; the shell owns the text and the pid binding. See shell.c. */

/* Give the just-launched program a surface. Returns its slot id (0..), or -1
 * when all are still held by windows not yet closed. Called from the launch
 * path before the window is opened. */
int  shell_program_open(const char *name, int pid);

/* Once a frame: notice any window whose process stopped owning a CPU, record its
 * exit code, mark it finished (the window stays), and log the fact. */
void shell_program_pump(void);

/* The window's read-only view of a program surface, by slot id. */
const char *shell_program_name(int slot);
int  shell_program_live(int slot);
int  shell_program_exit_code(int slot);

/* Paint a program's surface into a window body (no prompt, no cursor). */
void shell_program_draw(int slot, int x, int y, int width);

/* Non-zero when any program surface changed since it was last painted, so a
 * running program repaints the desktop on its own — exactly as the terminal does. */
int  shell_program_dirty_any(void);
void shell_program_clear_dirty_all(void);

/* Non-zero when *this* slot's surface changed since it was last painted, so the
 * compositor can damage only the window whose text moved (0 for a free slot). */
int  shell_program_dirty_slot(int slot);

/* The user closed the window: free its slot. Does not touch the process. */
void shell_program_close(int slot);

#endif
