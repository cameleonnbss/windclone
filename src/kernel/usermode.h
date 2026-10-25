/* WindClone — userland entry (Phase 4).
 *
 * usermode_run() opens a path through the VFS, hands the descriptor to the
 * ELF64 loader, sets up a user stack, and iretq's into ring 3. When the
 * program calls exit(), the kernel unwinds back to the caller of
 * usermode_run() as if it had simply returned — no thread, no scheduler,
 * just a controlled privilege round trip. That "return" is what a real
 * kernel will later turn into "resume the next runnable process".
 *
 * One address space is shared by kernel and program, so the constants below
 * are the fence between them: everything from USER_BASE up to
 * USER_ADDR_LIMIT is memory a program may own, and the loader rejects any
 * PT_LOAD that reaches outside it. That single rule is what keeps a
 * hand-edited binary from mapping its code over the kernel's own text.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_USERMODE_H
#define WINDCLONE_USERMODE_H

#include <stdint.h>

#define USER_BASE        0x400000ULL                    /* 4 MiB  */
#define USER_STACK_TOP   0x800000ULL                    /* 8 MiB  */
#define USER_STACK_SIZE  0x4000ULL                      /* 16 KiB */
#define USER_STACK_BOTTOM (USER_STACK_TOP - USER_STACK_SIZE)

/* Ceiling for a program's whole address space. The kernel and the HHDM live
 * at bit 47 and above, so the entire lower half is technically free; we stop
 * at 2 GiB to leave a wide, obvious gap and to keep every user address
 * representable in a signed 32-bit int for later debugging. */
#define USER_ADDR_LIMIT  0x80000000ULL

/* Returns 0 when the program ran to exit(), negative on a load failure.
 * `exit_code` receives what the program passed to exit(). */
int usermode_run(const char *path, int *exit_code);

/* Called from the SYS_EXIT handler; does not return. */
void usermode_exit(int code) __attribute__((noreturn));

/* Is [addr, addr+len) safely inside the user window? The syscall layer uses
 * this before touching a pointer that came from ring 3: a bad address should
 * answer -WC_EFAULT, not take a page fault and panic the machine. */
int user_range_ok(uint64_t addr, uint64_t len);

#endif
