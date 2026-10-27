/* WindClone — tmpfs: the writable half of the file layer.

 * The initrd is a boot archive: its files are slices of RAM that the
 * bootloader handed us, and there is nowhere for a changed byte to go. Until
 * now that meant the whole file layer could only answer reads, so "save a file"
 * was not implementable — a text editor that pretended to save would have been
 * a screenshot of a save dialog.
 *
 * This is the store that makes write() mean something. What it is:
 *   - a fixed pool of named blobs in the kernel's own .bss, so opening,
 *     writing and closing never allocate. That is not laziness: vfs.c's rule is
 *     that no allocation happens on the syscall path, because a syscall runs in
 *     whatever interrupt state the program left, and a kmalloc there would be a
 *     lock we do not have. Static buffers are the honest way to keep that rule.
 *   - RAM-only. Nothing here survives a reboot, and no test may claim
 *     otherwise. Persistence across boots needs a block device driver, and
 *     there is none yet (PCI is enumerated, no disk is read); what this release
 *     proves is that bytes a program wrote are readable by *another* program,
 *     which is the part that has nothing to do with media.
 *   - names are stored the way initrd stores them: no leading '/'. The VFS
 *     normalises before calling in, so both sides of the archive boundary speak
 *     the same path.
 *
 * Error convention: every WC_* macro in wcabi.h is *already* negative, so the
 * code below returns `WC_ENOENT` and the caller sees -2. The "-WC_ENOENT"
 * spelled in the comments is the same number, written the way vfs.h writes it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_TMPFS_H
#define WINDCLONE_TMPFS_H

#include <stddef.h>
#include <stdint.h>

#include "vfs.h"          /* struct wc_dirent lives in wcabi.h, pulled by vfs.h */

#define TMPFS_MAX_FILES  8      /* files in the store                     */
#define TMPFS_NAME_MAX   64     /* per file, NUL-terminated, no leading '/' */
#define TMPFS_FILE_MAX   4096   /* bytes per file; 32 KiB of .bss in total  */

/* Zero the pool. Called from vfs_init(), before any path lookup can happen. */
void tmpfs_init(void);

/* Normalise a path into the store's name form (strip a leading '/'). Returns
 * 0 on success, WC_EINVAL on empty, WC_ENAMETOOLONG past TMPFS_NAME_MAX-1. */
int tmpfs_norm(const char *path, char *out, size_t outsz);

/* -1 when no file has that (already normalised) name, else its index. */
int tmpfs_lookup(const char *name);

/* Create an empty file. Index on success, -WC_EEXIST when the name is taken,
 * -WC_ENOSPC when the pool is full, -WC_EINVAL on a bad name. */
int tmpfs_create(const char *name);

/* Forget a file and free its slot. 0 / -WC_ENOENT. */
int tmpfs_remove(const char *name);

/* Live-file count, in pool order. Also the number of entries the merged
 * directory enumeration has to offer before the archive's turn comes. */
unsigned tmpfs_count(void);

/* Accessors for an index returned by tmpfs_lookup/create. NULL-safe: a caller
 * that passes a slot it no longer holds gets NULL / 0 rather than a wild
 * pointer into someone else's file. */
const char *tmpfs_name(int idx);
uint8_t    *tmpfs_data(int idx);
uint64_t    tmpfs_size(int idx);
void        tmpfs_truncate(int idx);

/* The slot's generation: a number that goes up every time that pool entry
 * becomes a *file*, and 0 while it is free. It exists for one reason — a slot
 * index is a position in an array, and a position can be reused by a different
 * file after a remove. A caller that kept an index (the VFS's open descriptors
 * do) has to be able to ask whether what sits there now is still the thing it
 * opened, and 0-means-nothing-here is what makes "no file" a value a real
 * generation can never collide with. */
uint32_t    tmpfs_gen(int idx);

/* Write len bytes at off. Grows the file when the write runs past its end, and
 * pads the gap with NULs — the same answer a sparse write gives on a real
 * filesystem, stated here because there is no sparse machinery behind it.
 * Returns the bytes stored (0 when the file is already full), or -WC_ENOENT for
 * a slot nobody holds. A write that would pass TMPFS_FILE_MAX is clipped to the
 * capacity rather than refused, so a program can never wedge the store by
 * asking for too much. */
int64_t tmpfs_write(int idx, uint64_t off, const void *buf, uint64_t len);

/* Read one directory entry by position among the live files, exactly like
 * vfs_dirent does for the archive: 0 when `out` holds a name, -WC_ENOENT past
 * the last one. The kernel-side struct is filled; copying it to ring 3 stays
 * syscall.c's job. */
int64_t tmpfs_dirent(uint64_t index, struct wc_dirent *out);

#endif
