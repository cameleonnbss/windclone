/* WindClone — userland entry (Phase 4).
 *
 * usermode_run() takes a path that exists in the initrd, copies those
 * bytes into freshly allocated frames mapped with the USER bit, sets up a
 * user stack, and iretq's into ring 3. When the program calls exit(), the
 * kernel unwinds back to the caller of usermode_run() as if it had simply
 * returned — no thread, no scheduler, just a controlled privilege round
 * trip. That "return" is what a real kernel will later turn into "resume
 * the next runnable process".
 *
 * Layout is fixed for now: program at USER_BASE, stack growing down from
 * USER_STACK_TOP, both inside the lower half our own page tables own.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_USERMODE_H
#define WINDCLONE_USERMODE_H

#include <stdint.h>

#define USER_BASE        0x400000ULL                    /* 4 MiB  */
#define USER_STACK_TOP   0x800000ULL                    /* 8 MiB  */
#define USER_STACK_SIZE  0x4000ULL                      /* 16 KiB */
#define USER_MAX_IMAGE   0x10000ULL                     /* 64 KiB of code, for now */

/* Returns 0 when the program ran to exit(), negative on a load failure.
 * `exit_code` receives what the program passed to exit(). */
int usermode_run(const char *path, int *exit_code);

/* Called from the SYS_EXIT handler; does not return. */
void usermode_exit(int code) __attribute__((noreturn));

#endif
