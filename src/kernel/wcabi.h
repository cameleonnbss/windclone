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
/* getppid() — the twelfth call, and the only one whose answer can change during a
 * program's life: it names whoever the table says owns the caller *now*, so when
 * the process that spawned it dies first, the answer becomes the adopter's pid
 * (pid 1, the shell). A program can therefore report the re-parenting the kernel
 * performed on its behalf, which is the half of an orphan story the kernel's own
 * log line cannot testify to. */
#define SYS_GETPPID 12

/* open(path, flags) — the thirteenth call, and the reason the twelfth one is
 * still twelve. SYS_OPEN above takes one argument, and a program compiled
 * against it sets only rdi: rsi is whatever the compiler had lying around, so
 * giving SYS_OPEN a second, *meaningful* argument would make existing binaries
 * create files or open for writing by accident, depending on garbage. A new
 * number is the only safe way to add a parameter, which is exactly what
 * "frozen" is for. */
#define SYS_OPEN2 13

/* --- open flags (the argument of SYS_OPEN2) ----------------------------- */
/* Linux's values, with the same split Linux uses: the low two bits select the
 * access mode, the rest are bit flags. A program that wants to create a file
 * asks for O_WRONLY | O_CREAT | O_TRUNC; nothing here implements O_APPEND,
 * O_SYNC or O_NONBLOCK, and unknown bits are ignored rather than refused so a
 * later addition cannot break an older caller that happened to pass one. */
#define WC_O_RDONLY   0       /* ask for nothing: the old SYS_OPEN behaviour */
#define WC_O_WRONLY   1
#define WC_O_RDWR     2
#define WC_O_ACCMODE  3       /* mask for the three bits above               */
#define WC_O_CREAT    0x40    /* 0100: make the file if it is not there      */
#define WC_O_TRUNC    0x200   /* 01000: throw away what is already in it     */
#define WC_O_EXCL     0x400   /* 04000: fail if O_CREAT finds it already there */

/* net_addr(buf, len) — the fourteenth call, added with v0.1.31's browser. The
 * address bar of a browser must show the request the *machine* actually put on
 * the wire, and that record lives in the kernel (tcp.c), not in any file a
 * program could open. Handing ring 3 a kernel pointer would break the border,
 * and storing the text in a side file would make every scenario that counts
 * the files in the store wrong; so this copies the kernel's own words into a
 * caller-owned buffer, checked page by page like every other return. 0 on
 * success (buf holds a NUL-terminated address), -WC_ENOENT when nothing has
 * been fetched this boot, -WC_EFAULT for an unmapped or read-only buffer and
 * -WC_ERANGE when the buffer cannot hold the whole string plus its NUL. */
#define SYS_NET_ADDR 14

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
#define WC_EBUSY    (-16)                /* somebody else is using it now */
#define WC_EEXIST   (-17)               /* the name is already taken    */
#define WC_EINVAL   (-22)
#define WC_EMFILE   (-24)
#define WC_ENOSPC   (-28)               /* the writable store is full   */
#define WC_ENAMETOOLONG (-36)
#define WC_ENOSYS   (-38)
#define WC_ERANGE   (-75)

#endif
