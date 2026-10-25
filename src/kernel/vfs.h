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
 *   - the only backing store is the initrd, whose files are read-only
 *     slices of RAM. open() resolves a path there and nothing else;
 *   - fd 0/1/2 are the console. Writing to 1 and 2 reaches the serial port
 *     (what CI greps) and, when there is a text screen, the screen;
 *   - reading fd 0 fails with -WC_ENOSYS. The keyboard exists and the shell
 *     eats it; wiring it to a line buffer for userland is Phase 5's job, and
 *     returning "not implemented" is honest where returning 0 would look
 *     exactly like end-of-file;
 *   - writing to a file descriptor obtained from open() fails with -WC_EBADF,
 *     which is also what Linux answers for an fd opened O_RDONLY. There is
 *     no O_CREAT and no write-back, because pretending a write to the initrd
 *     succeeded would be a bug the tests could not catch.
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

/* Read at the descriptor's offset and advance it. 0 means clean EOF. */
int64_t vfs_read(int fd, void *buf, uint64_t len);

/* Same, at an explicit offset, without touching the descriptor's position.
 * This is what the ELF loader uses to walk program headers and segment data
 * without disturbing a fd a program may be holding. */
int64_t vfs_pread(int fd, void *buf, uint64_t len, uint64_t off);

int64_t vfs_write(int fd, const void *buf, uint64_t len);
int64_t vfs_close(int fd);

/* Size of the open file — the smallest slice of fstat() the loader needs,
 * because it must know how far the program headers go before mapping. */
int64_t vfs_size(int fd);

/* Does this path resolve to anything? The shell needs the yes/no answer to
 * decide whether a word is a program, and it must not have to reach into the
 * initrd to get it — that is the rule this header opened with. */
int vfs_exists(const char *path);

/* Fill one directory entry from the archive, by index. 0 = wrote it, -WC_ENOENT
 * once `index` is past the last file (that is how a caller loop finds the end
 * without a separate count call), -WC_EINVAL on a null pointer. Kernel-side
 * pointer only: syscall.c copies it out to ring 3 after validating. */
int64_t vfs_dirent(uint64_t index, struct wc_dirent *out);

/* Where a program's write(1)/write(2) lands when there is something on the
 * screen that wants it. Serial always gets every byte — it is what CI greps —
 * and the sink is what a human sees. NULL restores the VGA text console. */
typedef void (*vfs_sink_fn)(const char *data, uint64_t len);
void vfs_set_console_sink(vfs_sink_fn fn);

/* Live descriptors, for the boot report and for leaking tests. */
unsigned vfs_open_count(void);

#endif
