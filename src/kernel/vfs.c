/* WindClone — VFS implementation. See vfs.h for the contract.
 *
 * The descriptor table is a fixed array of (pointer, size, offset) triples
 * over slices of the initrd. No allocation happens on the syscall path: a
 * kmalloc here would run under whatever interrupt state the program left,
 * and a file layer that can only be used with interrupts off would be a
 * file layer we cannot keep.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>

#include "vfs.h"
#include "initrd.h"
#include "serial.h"
#include "vga.h"
#include "string.h"

struct open_file {
    int             used;
    const uint8_t  *data;      /* into the initrd module; read-only */
    uint64_t        size;
    uint64_t        offset;
};

static struct open_file slots[VFS_MAX_OPEN];
static unsigned live;

void vfs_init(void)
{
    memset(slots, 0, sizeof slots);
    live = 0;
    serial_write("vfs: descriptor table live (fd 0/1/2 = console, fd 3+ = initrd, read-only)\n");
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

int64_t vfs_open(const char *path)
{
    if (path == NULL || path[0] == '\0')
        return WC_EINVAL;

    const struct initrd_file *file = initrd_find(path);
    if (file == NULL)
        return WC_ENOENT;

    for (int i = 0; i < VFS_MAX_OPEN; i++) {
        if (slots[i].used)
            continue;
        slots[i].used  = 1;
        slots[i].data  = file->data;
        slots[i].size  = file->size;
        slots[i].offset = 0;
        live++;
        return (int64_t)(3 + i);
    }
    return WC_EMFILE;
}

int64_t vfs_pread(int fd, void *buf, uint64_t len, uint64_t off)
{
    const struct open_file *f = slot_of(fd);
    if (f == NULL)
        return WC_EBADF;
    if (buf == NULL)
        return WC_EFAULT;

    /* A read past the end is 0 bytes, not an error: that is how a caller
     * loop finds EOF without special-casing the last chunk. */
    if (off >= f->size)
        return 0;
    uint64_t avail = f->size - off;
    uint64_t n = len < avail ? len : avail;
    if (n > 0)
        memcpy(buf, f->data + off, (size_t)n);
    return (int64_t)n;
}

int64_t vfs_read(int fd, void *buf, uint64_t len)
{
    if (fd == STDIN_FILENO)
        return WC_ENOSYS;     /* no keyboard -> userland path yet */

    struct open_file *f = slot_of(fd);
    if (f == NULL)
        return WC_EBADF;

    int64_t n = vfs_pread(fd, buf, len, f->offset);
    if (n > 0)
        f->offset += (uint64_t)n;
    return n;
}

/* Both consoles, always: serial is what CI and the smoke tests read, the
 * text screen is what a human sitting at the machine reads. A program that
 * only reached one of them would look broken from the other. */
static void console_write(const void *buf, uint64_t len)
{
    serial_write_len((const char *)buf, (size_t)len);
    if (vga_available())
        kprint_len((const char *)buf, (size_t)len);
}

int64_t vfs_write(int fd, const void *buf, uint64_t len)
{
    if (fd != STDOUT_FILENO && fd != STDERR_FILENO)
        return WC_EBADF;      /* initrd descriptors are read-only by design */
    if (buf == NULL)
        return WC_EFAULT;

    console_write(buf, len);
    return (int64_t)len;
}

int64_t vfs_close(int fd)
{
    struct open_file *f = slot_of(fd);
    if (f == NULL)
        return WC_EBADF;

    f->used = 0;
    f->data = NULL;
    f->size = f->offset = 0;
    live--;
    return 0;
}

int64_t vfs_size(int fd)
{
    const struct open_file *f = slot_of(fd);
    if (f == NULL)
        return WC_EBADF;
    return (int64_t)f->size;
}

unsigned vfs_open_count(void)
{
    return live;
}
