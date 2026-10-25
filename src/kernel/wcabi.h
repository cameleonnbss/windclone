/* WindClone — the userland ABI.
 *
 * This is the contract between a program and the kernel, and the only
 * header both sides include. It is frozen from here on: a program built for
 * v0.1.0 has to keep working, so numbers never get renumbered and the
 * register roles never change.
 *
 *   entry:   int 0x80, with rax = the call number
 *   args:    rdi, rsi, rdx, r10
 *   result:  rax; a negative value is one of the -WC_* errors below
 *
 * Two choices here are deliberate and worth defending:
 *
 * 1) The fourth argument is r10, not rcx. rcx is scratch in this ABI, and a
 *    later move to syscall/sysret (Phase 7) would make the CPU overwrite it
 *    with the return address. Keeping it out of the argument set now means
 *    programs survive that switch unchanged.
 *
 * 2) A C wrapper needs no register clobbers, because int 0x80 lands in
 *    syscall_stub (stubs.asm), which saves all sixteen GPRs, calls C with
 *    the frame, and restores everything except rax. That stops being true
 *    the moment SYSCALL replaces it: SYSCALL clobbers only rcx and r11, so
 *    gcc keeps rdi/rsi/rdx/r8/r9/r10 live across the instruction and the
 *    *kernel* has to preserve them if it wants to run C code. Anyone
 *    making that switch must snapshot those six, or user programs will
 *    silently receive the kernel's leftovers as their own arguments.
 *
 * Error values are Linux's errno numbers, negated. Phase 7 promises to run
 * static Linux binaries; matching errno now is what keeps that translation
 * layer thin.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_WCABI_H
#define WINDCLONE_WCABI_H

/* --- call numbers ----------------------------------------------------- */
#define SYS_WRITE   1
#define SYS_EXIT    2
#define SYS_OPEN    3
#define SYS_READ    4
#define SYS_CLOSE   5

/* --- the three standard descriptors ----------------------------------- */
#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

/* Anything the kernel hands back to userland is negative on failure, so a
 * program can test `rax < 0` without knowing the table. */
#define WC_EPERM    (-1)
#define WC_ENOENT   (-2)
#define WC_EBADF    (-9)
#define WC_EFAULT   (-14)
#define WC_EINVAL   (-22)
#define WC_EMFILE   (-24)
#define WC_ENAMETOOLONG (-36)
#define WC_ENOSYS   (-38)
#define WC_ERANGE   (-75)

#endif
