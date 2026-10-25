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
static vfs_sink_fn console_sink;

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
 * pointer into the archive. */
int vfs_exists(const char *path)
{
    if (path == NULL || path[0] == '\0')
        return 0;
    return initrd_find(path) != NULL ? 1 : 0;
}

/* One entry of the archive by position, for a program that wants to list it.
 * Names longer than WC_NAME_MAX are truncated rather than refused: a 100-byte
 * name is legal in a tar and `ls` showing the first 63 characters of it beats
 * failing the whole listing over cosmetics. */
int64_t vfs_dirent(uint64_t index, struct wc_dirent *out)
{
    if (out == NULL)
        return WC_EINVAL;

    const struct initrd_file *f = initrd_at((unsigned)index);
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
