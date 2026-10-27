/* WindClone — VFS implementation. See vfs.h for the contract.
 *
 * The descriptor table is a fixed array over two stores: slices of the initrd,
 * which are read-only, and slots of tmpfs, which are not. No allocation happens
 * on the syscall path: a kmalloc here would run under whatever interrupt state
 * the program left, and a file layer that can only be used with interrupts off
 * would be a file layer we cannot keep.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>

#include "vfs.h"
#include "initrd.h"
#include "serial.h"
#include "tmpfs.h"
#include "vga.h"
#include "string.h"

struct open_file {
    int             used;
    const uint8_t  *data;      /* the bytes: into the initrd, or a tmpfs blob */
    uint8_t        *writable;  /* non-NULL only when write() may touch `data` */
    int             tidx;      /* tmpfs slot, or -1 for an archive file */
    uint32_t        gen;       /* that slot's generation at open time */
    uint64_t        size;      /* archive files: fixed. tmpfs: a snapshot,
                                * and current_size() is what reads it live. */
    uint64_t        offset;
};

static struct open_file slots[VFS_MAX_OPEN];
static unsigned live;
static vfs_sink_fn console_sink;

void vfs_init(void)
{
    memset(slots, 0, sizeof slots);
    live = 0;
    tmpfs_init();
    serial_write("vfs: descriptor table live (fd 0/1/2 = console, fd 3+ = "
                 "initrd read-only + tmpfs writable)\n");
    serial_write("tmpfs: writable store live (RAM only, so nothing here "
                 "survives a reboot)\n");
}

/* Descriptor numbers: 0..2 are reserved for the console, so a file starts
 * at 3. That gap is the whole mapping, which is why it is one function —
 * an off-by-one here is a program reading someone else's file. */
static struct open_file *slot_of(int fd)
{
    if (fd < 3 || fd >= 3 + VFS_MAX_OPEN)
        return NULL;
    struct open_file *f = &slots[fd - 3];
    return f->used ? f : NULL;
}

/* A descriptor over a tmpfs file is a *snapshot of a live thing*: the bytes are
 * the pool's, so they cannot move, but the file can grow under us (another
 * process wrote it) and the slot can be recycled (the file was removed and a
 * different one was created in its place). Both are answered here, in one place,
 * so no reader or writer can bypass them: an fd whose file is gone is a bad fd,
 * and an fd whose file grew is a reader that gets the new length. */
static struct open_file *live_file(int fd)
{
    struct open_file *f = slot_of(fd);
    if (f == NULL)
        return NULL;
    if (f->tidx >= 0 && tmpfs_gen(f->tidx) != f->gen)
        return NULL;        /* the file this descriptor was opened on is gone */
    return f;
}

static uint64_t current_size(const struct open_file *f)
{
    return f->tidx >= 0 ? tmpfs_size(f->tidx) : f->size;
}

/* The store's name form for a path, or WC_ENAMETOOLONG / WC_EINVAL. The archive
 * is consulted with the path as given (initrd_find strips the slash itself), so
 * a name that is too long for tmpfs can still be a real file in the tar. */
static int store_name(const char *path, char *out, size_t cap)
{
    return tmpfs_norm(path, out, cap);
}

int64_t vfs_open(const char *path)
{
    return vfs_open_flags(path, WC_O_RDONLY);
}

/* One free descriptor, or -WC_EMFILE. Its own function because both stores need
 * it and a slot left half-initialised is a descriptor that points at nothing. */
static struct open_file *reserve(void)
{
    for (int i = 0; i < VFS_MAX_OPEN; i++) {
        if (slots[i].used)
            continue;
        slots[i].used = 1;
        live++;
        return &slots[i];
    }
    return NULL;
}

int64_t vfs_open_flags(const char *path, uint64_t flags)
{
    if (path == NULL || path[0] == '\0')
        return WC_EINVAL;

    const uint64_t mode = flags & WC_O_ACCMODE;
    const int want_write = mode != WC_O_RDONLY;
    const int create = (flags & WC_O_CREAT) != 0;
    const int excl   = (flags & WC_O_EXCL) != 0;
    const int trunc  = (flags & WC_O_TRUNC) != 0;

    char name[TMPFS_NAME_MAX];
    const struct initrd_file *file = initrd_find(path);

    /* The archive wins every time, which is the property that matters: a
     * program cannot shadow /bin/ls by saving a file under that name, because
     * the name never gets as far as the writable store. */
    if (file != NULL) {
        if (create && excl)
            return WC_EEXIST;     /* Linux answers this way whatever the fd
                                   * would have been opened for */
        if (want_write)
            return WC_EPERM;      /* a boot archive is not a disk; saying so
                                   * beats handing back a descriptor that cannot
                                   * do the one thing it was opened for */
        struct open_file *f = reserve();
        if (f == NULL)
            return WC_EMFILE;
        f->data     = file->data;
        f->writable = NULL;
        f->tidx     = -1;
        f->gen      = 0;
        f->size     = file->size;
        f->offset   = 0;
        return (int64_t)(3 + (int)(f - slots));
    }

    /* Nothing in the tar: the writable store is the only place left, and it
     * keeps its own rules about what a name may be. */
    int norm = store_name(path, name, sizeof name);
    if (norm != 0)
        return norm;

    int idx = tmpfs_lookup(name);
    int made_it = 0;
    if (idx < 0) {
        if (!create)
            return WC_ENOENT;
        idx = tmpfs_create(name);
        if (idx < 0)
            return idx;           /* WC_ENOSPC or WC_EINVAL */
        made_it = 1;
    } else if (create && excl) {
        return WC_EEXIST;         /* O_EXCL only means something next to O_CREAT,
                                   * and this is the case it exists for */
    } else if (trunc && want_write) {
        tmpfs_truncate(idx);
    }
    /* O_TRUNC on a read-only open is ignored rather than obeyed. A flag that
     * destroyed a file the caller could not write to would be data loss with
     * a permission error stapled on the front of it. */

    struct open_file *f = reserve();
    if (f == NULL) {
        /* No descriptor: undo the creation this call just did, so a failed open
         * never leaves a file behind. A program that retries after an EMFILE
         * would otherwise find an empty note where it expected to make one, and
         * "open failed" that changed the filesystem is not a failed open. */
        if (made_it)
            tmpfs_remove(name);
        return WC_EMFILE;
    }
    f->data     = tmpfs_data(idx);
    f->writable = want_write ? tmpfs_data(idx) : NULL;
    f->tidx     = idx;
    f->gen      = tmpfs_gen(idx);
    f->size     = tmpfs_size(idx);
    f->offset   = 0;
    return (int64_t)(3 + (int)(f - slots));
}

int64_t vfs_pread(int fd, void *buf, uint64_t len, uint64_t off)
{
    const struct open_file *f = live_file(fd);
    if (f == NULL)
        return WC_EBADF;
    if (buf == NULL)
        return WC_EFAULT;

    /* A read past the end is 0 bytes, not an error: that is how a caller
     * loop finds EOF without special-casing the last chunk. */
    const uint64_t size = current_size(f);
    if (off >= size)
        return 0;
    uint64_t avail = size - off;
    uint64_t n = len < avail ? len : avail;
    if (n > 0)
        memcpy(buf, f->data + off, (size_t)n);
    return (int64_t)n;
}

int64_t vfs_read(int fd, void *buf, uint64_t len)
{
    if (fd == STDIN_FILENO)
        return WC_ENOSYS;     /* read(0) is the scheduler's now (see syscall.c);
                               * this stays for a caller outside a process, where
                               * there is no slot to put to sleep and a 0 would
                               * look exactly like end-of-file */

    struct open_file *f = live_file(fd);
    if (f == NULL)
        return WC_EBADF;

    int64_t n = vfs_pread(fd, buf, len, f->offset);
    if (n > 0)
        f->offset += (uint64_t)n;
    return n;
}

/* Both consoles, always: serial is what CI and the smoke tests read, the
 * screen is what a human sitting at the machine reads. Which screen depends
 * on the phase: once the desktop is up, the terminal window is the only
 * surface a user can see (0xB8000 is behind a linear framebuffer), so the
 * shell registers a sink and program output lands in its scrollback. The
 * serial port never stops receiving anything, or every CI grep would depend
 * on a window manager that has nothing to do with files. */
static void console_write(const void *buf, uint64_t len)
{
    serial_write_len((const char *)buf, (size_t)len);
    if (console_sink != NULL) {
        console_sink((const char *)buf, len);
        return;
    }
    if (vga_available())
        kprint_len((const char *)buf, (size_t)len);
}

void vfs_set_console_sink(vfs_sink_fn fn)
{
    console_sink = fn;
}

/* The path answer the shell wants before it tries to run something. Held to
 * the rule at the top of this file: the shell asks, it does not keep a
 * pointer into the archive. Both stores are asked, in open()'s order, so a
 * saved file is as real to a lookup as a packaged one. */
int vfs_exists(const char *path)
{
    if (path == NULL || path[0] == '\0')
        return 0;
    if (initrd_find(path) != NULL)
        return 1;
    char name[TMPFS_NAME_MAX];
    if (tmpfs_norm(path, name, sizeof name) != 0)
        return 0;
    return tmpfs_lookup(name) >= 0 ? 1 : 0;
}

/* One entry of the file layer by position. The writable store comes first and
 * the archive is shifted by however many files the store holds, so a caller loop
 * sees one filesystem instead of two halves. The offset is read once, here: an
 * enumeration that a concurrent save interrupts may show a file twice or skip
 * one, which is what getdents does on a real machine and is stated rather than
 * hidden. Names longer than WC_NAME_MAX are truncated rather than refused: a
 * 100-byte name is legal in a tar and `ls` showing the first 63 characters of it
 * beats failing the whole listing over cosmetics. */
int64_t vfs_dirent(uint64_t index, struct wc_dirent *out)
{
    if (out == NULL)
        return WC_EINVAL;

    const unsigned store = tmpfs_count();
    if (index < store)
        return tmpfs_dirent(index, out);

    const struct initrd_file *f = initrd_at((unsigned)(index - store));
    if (f == NULL)
        return WC_ENOENT;

    memset(out, 0, sizeof *out);
    size_t l = strlen(f->name);
    if (l > WC_NAME_MAX - 1)
        l = WC_NAME_MAX - 1;
    memcpy(out->name, f->name, l);
    out->name[l] = '\0';
    out->namelen = l;
    out->size = f->size;
    return 0;
}

int64_t vfs_write(int fd, const void *buf, uint64_t len)
{
    if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        if (buf == NULL)
            return WC_EFAULT;
        console_write(buf, len);
        return (int64_t)len;
    }

    struct open_file *f = live_file(fd);
    if (f == NULL)
        return WC_EBADF;
    if (buf == NULL)
        return WC_EFAULT;
    if (f->writable == NULL)
        return WC_EBADF;      /* opened read-only: the same answer Linux gives */

    int64_t n = tmpfs_write(f->tidx, f->offset, buf, len);
    if (n > 0)
        f->offset += (uint64_t)n;
    return n;
}

int64_t vfs_close(int fd)
{
    /* slot_of and not live_file: a descriptor must be releasable even when the
     * file behind it is gone, or closing after an unlink would leak the slot and
     * the next open would fail with EMFILE for a program that closed everything
     * it opened. */
    struct open_file *f = slot_of(fd);
    if (f == NULL)
        return WC_EBADF;

    f->used = 0;
    f->data = NULL;
    f->writable = NULL;
    f->tidx = -1;
    f->gen = 0;
    f->size = f->offset = 0;
    live--;
    return 0;
}

int64_t vfs_size(int fd)
{
    const struct open_file *f = live_file(fd);
    if (f == NULL)
        return WC_EBADF;
    return (int64_t)current_size(f);
}

unsigned vfs_open_count(void)
{
    return live;
}
