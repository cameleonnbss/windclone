/* WindClone — VFS: one file layer for the kernel, the shell and ring 3.
 *
 * Before this, every reader of the initrd called initrd_find() and walked
 * the returned bytes itself. That works while there is one archive and no
 * state, and it breaks the moment a program has to be able to read part of a
 * file, or a real disk driver wants to replace the archive behind the same
 * interface. So the rule from here is: *nothing* outside this file holds a
 * pointer into the initrd. Callers get a descriptor and ask it for bytes.
 *
 * What is real, and what is not:
 *   - two stores answer one path. The initrd is a boot archive whose files are
 *     read-only slices of RAM, and tmpfs is a fixed pool of RAM blobs that are
 *     not: open() looks in the archive first, then in the store, and a path
 *     resolves to whichever has it. Nothing here is a disk, so nothing survives
 *     a reboot;
 *   - fd 0/1/2 are the console. Writing to 1 and 2 reaches the serial port
 *     (what CI greps) and, when there is a text screen, the screen;
 *   - reading fd 0 is not this file's business. Since v0.1.20 the syscall layer
 *     intercepts read(0) before it reaches the VFS and hands it to the scheduler,
 *     which may put the caller to *sleep* until a keystroke arrives — a file
 *     layer cannot block, so the keyboard belongs to whoever can. vfs_read()'s
 *     fd-0 branch stays as the honest answer for the one place that still has no
 *     process to sleep: outside the scheduler, where it returns -WC_ENOSYS rather
 *     than the 0 that would look exactly like end-of-file;
 *   - writing to a descriptor depends on what it was opened from. A tmpfs file
 *     opened for writing stores the bytes and reads back later, including from
 *     another process. An archive file is read-only whatever the caller asked
 *     for: open(..., O_WRONLY) on it answers -WC_EPERM rather than handing back
 *     a descriptor that would lie, and writing to one opened read-only answers
 *     -WC_EBADF, which is also what Linux says. A write that was accepted is a
 *     write that happened; that is the rule this whole file is built on.
 *
 * Offsets live in the descriptor, never in the caller, so two programs with
 * the same file open cannot step on each other's read position.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_VFS_H
#define WINDCLONE_VFS_H

#include <stdint.h>
#include <stddef.h>

#include "wcabi.h"

/* fd 3 .. 3+VFS_MAX_OPEN-1. Small on purpose: there is no process table to
 * grow into yet, and a fixed array is what lets a syscall path allocate
 * nothing while interrupts stay enabled. */
#define VFS_MAX_OPEN 8

/* Called after initrd_init(): the descriptor table itself has no backing
 * store, only slots. */
void vfs_init(void);

/* Path lookups are the kernel's job; a user path is copied into a kernel
 * buffer before reaching here (see syscall.c). Returns fd >= 3, or
 * -WC_ENOENT / -WC_EINVAL / -WC_EMFILE. */
int64_t vfs_open(const char *path);

/* The same resolution with intent attached, for the caller that wants to write.
 * `flags` is the WC_O_* set from wcabi.h (SYS_OPEN2's second argument), and only
 * the access mode plus CREAT / TRUNC / EXCL are honoured — anything else is
 * ignored, on purpose, so a program that passes a bit invented after this file
 * was frozen still gets the file it asked for.
 *
 * Resolution order is the archive first, then the writable store, and the two
 * never share a name: a program cannot shadow /bin/hello by saving a file under
 * it. Returns fd >= 3, or -WC_ENOENT (nothing has that path), -WC_EPERM (the
 * archive has it and the caller wants to write), -WC_EEXIST (O_CREAT | O_EXCL on
 * a name that is already in the store), -WC_ENOSPC (the pool is full),
 * -WC_EMFILE (no descriptor slot) or -WC_EINVAL. */
int64_t vfs_open_flags(const char *path, uint64_t flags);

/* Read at the descriptor's offset and advance it. 0 means clean EOF. */
int64_t vfs_read(int fd, void *buf, uint64_t len);

/* Same, at an explicit offset, without touching the descriptor's position.
 * This is what the ELF loader uses to walk program headers and segment data
 * without disturbing a fd a program may be holding. */
int64_t vfs_pread(int fd, void *buf, uint64_t len, uint64_t off);

/* fd 1/2 go to the console. A descriptor from vfs_open_flags() that was opened
 * for writing stores the bytes at its own offset and advances it, so a program
 * that wrote 40 bytes reads 40 back. Everything else answers -WC_EBADF. */
int64_t vfs_write(int fd, const void *buf, uint64_t len);
int64_t vfs_close(int fd);

/* Size of the open file — the smallest slice of fstat() the loader needs,
 * because it must know how far the program headers go before mapping. */
int64_t vfs_size(int fd);

/* Does this path resolve to anything? The shell needs the yes/no answer to
 * decide whether a word is a program, and it must not have to reach into the
 * initrd to get it — that is the rule this header opened with. Both stores are
 * asked, in the same order open() uses. */
int vfs_exists(const char *path);

/* One entry of the file layer by position, for a program that wants to list it
 * and for the desktop's start menu. The writable store is enumerated first, then
 * the archive, so `index` runs over every name either side has and a caller loop
 * that stops at -WC_ENOENT sees the whole filesystem rather than one half of it.
 * 0 = wrote it, -WC_ENOENT once `index` is past the last file (that is how a
 * caller loop finds the end without a separate count call), -WC_EINVAL on a null
 * pointer. Kernel-side struct only: syscall.c copies it out to ring 3 after
 * validating. */
int64_t vfs_dirent(uint64_t index, struct wc_dirent *out);

/* Where a program's write(1)/write(2) lands when there is something on the
 * screen that wants it. Serial always gets every byte — it is what CI greps —
 * and the sink is what a human sees. NULL restores the VGA text console. */
typedef void (*vfs_sink_fn)(const char *data, uint64_t len);
void vfs_set_console_sink(vfs_sink_fn fn);

/* Live descriptors, for the boot report and for leaking tests. */
unsigned vfs_open_count(void);

#endif
