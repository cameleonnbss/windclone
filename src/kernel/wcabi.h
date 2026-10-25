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
 * How a program starts (the Part 5 addition, frozen like everything else):
 *
 *   _start is entered with rdi = argc and rsi = argv, *and* the same
 *   information sits on the stack the way the AMD64 psABI describes it:
 *
 *     (%rsp)  argc
 *             argv[0] .. argv[argc-1], NULL
 *             envp[0] = NULL            (no environment yet)
 *             0, 0                      (an empty auxv: AT_NULL)
 *
 *   Registers are the convenient contract, the stack is the honest one: a
 *   Linux binary reads argv from memory, never from rdi, so passing both now
 *   is what lets Part 7 run one without rewriting the loader.
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

/* The dirent struct below needs fixed-width types, and a freestanding program
 * has no reason to have included stdint.h before this header. gcc ships
 * stdint.h for -ffreestanding builds, so both sides of the boundary get the
 * same 8-byte uint64_t - which is the whole point of freezing a struct here. */
#include <stdint.h>

/* --- call numbers ----------------------------------------------------- */
#define SYS_WRITE   1
#define SYS_EXIT    2
#define SYS_OPEN    3
#define SYS_READ    4
#define SYS_CLOSE   5
#define SYS_GETDIRENT 6
#define SYS_GETPID  7
#define SYS_SCHED_YIELD 8
#define SYS_SPAWN   9
#define SYS_WAIT   10
#define SYS_KILL   11

/* --- signals ------------------------------------------------------------ */
/* Only SIGKILL exists so far, and only as the argument of kill(): there is no
 * delivery, no masking, no handler. It is Linux's number because Linux's
 * numbers are the whole reason for this header's errno choices. */
#define SIGKILL       9

/* --- directory entries ------------------------------------------------- */
/* One file's metadata, copied to the caller by SYS_GETDIRENT. Fixed size and
 * fixed offsets because it crosses a privilege boundary: a program built for
 * this kernel has to be able to lay the same bytes out without our headers.
 * `name` has no leading '/' - the initrd stores it that way - and is always
 * NUL-terminated inside the 64 bytes. */
#define WC_NAME_MAX 64

struct wc_dirent {
    uint64_t size;
    uint64_t namelen;               /* strlen(name), excluding the NUL */
    char     name[WC_NAME_MAX];
};

/* --- the three standard descriptors ----------------------------------- */
#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

/* Anything the kernel hands back to userland is negative on failure, so a
 * program can test `rax < 0` without knowing the table. */
#define WC_EPERM    (-1)
#define WC_ENOENT   (-2)
#define WC_ESRCH    (-3)                /* no such process               */
#define WC_EBADF    (-9)
#define WC_ECHILD   (-10)               /* no children to wait for   */
#define WC_EFAULT   (-14)
#define WC_EINVAL   (-22)
#define WC_EMFILE   (-24)
#define WC_ENAMETOOLONG (-36)
#define WC_ENOSYS   (-38)
#define WC_ERANGE   (-75)

#endif
