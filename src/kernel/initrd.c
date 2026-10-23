/* WindClone — initrd: parse the tar module the bootloader loaded.
 *
 * Why a tar and not our own format: tar is 512-byte block oriented, every
 * field is fixed-width ASCII, and the whole archive can be walked with an
 * offset and a length — no libc, no allocation per file, no seek API. The
 * kernel slices file data straight out of the module: zero copies.
 *
 * Layout of one entry:
 *   [ 512-byte header ][ data, padded up to a 512-byte multiple ]
 * A ustar header carries the name (100 B) + a prefix (155 B, used for
 * paths longer than 100 B), the size in *octal* ASCII, and a checksum of
 * the header itself (with the checksum field read as spaces). Two
 * all-zero blocks end the archive.
 *
 * What we skip on purpose: typeflags other than '0'/'\0' (directories,
 * symlinks, pax headers), hard links, and multi-module "overlay" mounts.
 * Phase 4 only needs "give me the bytes of this path".
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>
#include <limine.h>

#include "initrd.h"
#include "kmalloc.h"
#include "string.h"
#include "serial.h"

#define TAR_BLOCK        512u
#define INITRD_MAX_FILES 64u

struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];      /* octal ASCII */
    char mtime[12];
    char chksum[8];     /* octal ASCII, header sum with this field as spaces */
    char typeflag;      /* '0' or '\0' = regular file, '5' = directory, ... */
    char linkname[100];
    char magic[6];      /* "ustar\0" (POSIX) or "ustar " (old GNU) */
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};

_Static_assert(sizeof(struct tar_header) == TAR_BLOCK,
               "a ustar header is exactly one 512-byte block");

/* The module request lives here, not in kernel.c, because this file owns
 * the concept. `used` keeps the compiler from dropping it before the
 * linker script KEEP()s the .limine_requests section. */
__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0,
    .response = NULL,
};

static struct initrd_file *files;
static unsigned file_count;
static const char *module_path = "(none)";
static uint64_t module_bytes;

/* Fixed-width ASCII field -> integer. Returns -1 if the field holds no
 * octal digit at all (which is how we detect garbage, not empty files:
 * a real size field is at least "00000000000"). */
static int parse_octal(const char *field, size_t len, uint64_t *out)
{
    uint64_t value = 0;
    size_t i = 0;
    int digits = 0;

    while (i < len && (field[i] == ' ' || field[i] == '\0'))
        i++;
    for (; i < len && field[i] >= '0' && field[i] <= '7'; i++) {
        value = (value << 3) + (uint64_t)(field[i] - '0');
        digits++;
    }
    if (digits == 0)
        return -1;
    *out = value;
    return 0;
}

static int block_is_zero(const uint8_t *block)
{
    for (size_t i = 0; i < TAR_BLOCK; i++)
        if (block[i] != 0)
            return 0;
    return 1;
}

/* Header checksum: sum every byte, but treat the checksum field itself as
 * spaces — it was written as spaces when the sum was computed. */
static int checksum_ok(const struct tar_header *h)
{
    const size_t sum_off = offsetof(struct tar_header, chksum);
    const size_t sum_len = sizeof h->chksum;
    const uint8_t *raw = (const uint8_t *)h;
    uint64_t want;

    if (parse_octal(h->chksum, sum_len, &want) != 0)
        return 0;

    uint64_t got = 0;
    for (size_t i = 0; i < TAR_BLOCK; i++)
        got += (i >= sum_off && i < sum_off + sum_len) ? ' ' : raw[i];

    return got == want;
}

/* prefix/name -> "etc/motd", stripping the leading "./" that
 * `tar -C rootfs -cf ... .` writes. Both source fields are fixed-width
 * and may fill their buffer without a NUL, so the copy is bounded. */
static void entry_name(const struct tar_header *h, char *out, size_t cap)
{
    size_t n = 0;
    size_t i = (h->name[0] == '.' && h->name[1] == '/') ? 2 : 0;

    if (h->prefix[0] != '\0') {
        for (size_t k = 0; k < sizeof h->prefix && h->prefix[k] && n + 1 < cap; k++)
            out[n++] = h->prefix[k];
        if (n + 1 < cap)
            out[n++] = '/';
    }
    for (; i < sizeof h->name && h->name[i] && n + 1 < cap; i++)
        out[n++] = h->name[i];

    out[n] = '\0';
}

int initrd_init(void)
{
    file_count = 0;
    module_bytes = 0;
    module_path = "(none)";
    files = NULL;

    if (module_request.response == NULL ||
        module_request.response->module_count == 0)
        return -1;

    const struct limine_file *mod = module_request.response->modules[0];
    if (mod == NULL || mod->address == NULL || mod->size < TAR_BLOCK)
        return -1;

    module_path = (mod->path != NULL) ? mod->path : "(unnamed)";
    module_bytes = mod->size;

    files = kcalloc(INITRD_MAX_FILES, sizeof *files);
    if (files == NULL)
        return -1;

    const uint8_t *base = (const uint8_t *)mod->address;
    uint64_t off = 0;

    while (off + TAR_BLOCK <= module_bytes) {
        const struct tar_header *h =
            (const struct tar_header *)(const void *)(base + off);

        if (block_is_zero(base + off))
            break;                          /* end-of-archive marker */
        if (memcmp(h->magic, "ustar", 5) != 0) {
            serial_write("initrd: not a ustar header at offset ");
            serial_write_dec(off);
            serial_write(", stopping\n");
            break;
        }
        if (!checksum_ok(h)) {
            serial_write("initrd: header checksum mismatch at offset ");
            serial_write_dec(off);
            serial_write(", stopping\n");
            break;
        }

        uint64_t size;
        if (parse_octal(h->size, sizeof h->size, &size) != 0)
            break;

        const uint64_t data_off = off + TAR_BLOCK;
        if (size > module_bytes - data_off) {
            serial_write("initrd: entry runs past the module, stopping\n");
            break;
        }

        /* Only regular files are addressable; directories and metadata
         * entries still have to be stepped over to reach the next one. */
        if (h->typeflag == '0' || h->typeflag == '\0') {
            if (file_count == INITRD_MAX_FILES) {
                serial_write("initrd: file table full, ignoring the rest\n");
                break;
            }
            struct initrd_file *f = &files[file_count];
            entry_name(h, f->name, sizeof f->name);
            if (f->name[0] != '\0') {
                f->data = base + data_off;
                f->size = size;
                file_count++;
            }
        }

        const uint64_t padded = (size + TAR_BLOCK - 1) & ~(uint64_t)(TAR_BLOCK - 1);
        if (padded > module_bytes - data_off)
            break;
        off = data_off + padded;
    }

    if (file_count == 0)
        return -1;

    return 0;
}

unsigned initrd_count(void)
{
    return file_count;
}

const struct initrd_file *initrd_at(unsigned index)
{
    if (index >= file_count)
        return NULL;
    return &files[index];
}

const struct initrd_file *initrd_find(const char *path)
{
    if (path == NULL || files == NULL)
        return NULL;
    if (path[0] == '/')
        path++;

    for (unsigned i = 0; i < file_count; i++)
        if (strcmp(files[i].name, path) == 0)
            return &files[i];

    return NULL;
}

const char *initrd_module_path(void)
{
    return module_path;
}

uint64_t initrd_module_size(void)
{
    return module_bytes;
}
