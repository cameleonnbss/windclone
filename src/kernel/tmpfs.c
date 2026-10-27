/* WindClone — tmpfs implementation. See tmpfs.h for the contract and for what
 * is and is not real about a RAM file store.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>

#include "tmpfs.h"
#include "wcabi.h"
#include "string.h"

struct tfile {
    int      used;
    uint32_t gen;        /* bumped on every create; 0 means this slot is free */
    char     name[TMPFS_NAME_MAX];
    uint64_t size;
    uint8_t  data[TMPFS_FILE_MAX];
};

/* One pool, in .bss. The size is the whole design decision: 8 x 4 KiB is what a
 * text editor's open documents and a couple of scratch files need, and it buys
 * that without a byte of runtime allocation on a syscall path. */
static struct tfile pool[TMPFS_MAX_FILES];
static unsigned live;

void tmpfs_init(void)
{
    memset(pool, 0, sizeof pool);
    live = 0;
}

int tmpfs_norm(const char *path, char *out, size_t outsz)
{
    if (path == NULL || out == NULL || outsz == 0)
        return WC_EINVAL;
    if (path[0] == '/')
        path++;                       /* the store keeps names without a slash */

    size_t i = 0;
    for (; path[i] != '\0'; i++) {
        /* One byte of slack for the NUL. A name that exactly fills the buffer
         * is ENAMETOOLONG, not silently truncated: a truncated name would
         * create a *different* file from the one the program asked for, and a
         * save that lands under the wrong name is data loss with a smile on. */
        if (i + 1 >= outsz)
            return WC_ENAMETOOLONG;
        out[i] = path[i];
    }
    out[i] = '\0';
    if (i == 0)
        return WC_EINVAL;             /* "/" or "" is not a file */
    return 0;
}

static int find_by_name(const char *name)
{
    for (int i = 0; i < TMPFS_MAX_FILES; i++)
        if (pool[i].used && strcmp(pool[i].name, name) == 0)
            return i;
    return -1;
}

int tmpfs_lookup(const char *name)
{
    if (name == NULL)
        return -1;
    return find_by_name(name);
}

int tmpfs_create(const char *name)
{
    if (name == NULL || name[0] == '\0')
        return WC_EINVAL;
    int already = find_by_name(name);
    if (already >= 0)
        return WC_EEXIST;

    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (pool[i].used)
            continue;
        pool[i].used = 1;
        /* A fresh generation: a descriptor that has been open on this slot
         * since before the file was removed must not get to read *this* file's
         * bytes. The counter never goes backwards, and it is never 0 for a live
         * file — see tmpfs_gen(). */
        pool[i].gen++;
        size_t l = strlen(name);
        if (l >= TMPFS_NAME_MAX)          /* refused in norm(); belt and braces */
            l = TMPFS_NAME_MAX - 1;
        memcpy(pool[i].name, name, l);
        pool[i].name[l] = '\0';
        pool[i].size = 0;
        live++;
        return i;
    }
    return WC_ENOSPC;
}

int tmpfs_remove(const char *name)
{
    int idx = find_by_name(name);
    if (idx < 0)
        return WC_ENOENT;
    pool[idx].used = 0;
    pool[idx].name[0] = '\0';
    pool[idx].size = 0;
    /* gen is deliberately left alone: it is the slot's history, not the file's,
     * and forgetting it here is what would let the next create hand a recycled
     * slot the same generation an old descriptor is still holding. */
    live--;
    return 0;
}

unsigned tmpfs_count(void)
{
    return live;
}

/* Every accessor checks the slot rather than trusting the caller's index: a
 * descriptor is a number that outlives the gesture that made it, and a stale one
 * must not be able to read or write whichever file happens to sit there now. */
static struct tfile *slot(int idx)
{
    if (idx < 0 || idx >= TMPFS_MAX_FILES || !pool[idx].used)
        return NULL;
    return &pool[idx];
}

const char *tmpfs_name(int idx)
{
    struct tfile *f = slot(idx);
    return f ? f->name : NULL;
}

uint8_t *tmpfs_data(int idx)
{
    struct tfile *f = slot(idx);
    return f ? f->data : NULL;
}

uint64_t tmpfs_size(int idx)
{
    struct tfile *f = slot(idx);
    return f ? f->size : 0;
}

uint32_t tmpfs_gen(int idx)
{
    const struct tfile *f = slot(idx);
    return f ? f->gen : 0;      /* 0 = nobody holds that slot any more */
}

void tmpfs_truncate(int idx)
{
    struct tfile *f = slot(idx);
    if (f)
        f->size = 0;
}

int64_t tmpfs_write(int idx, uint64_t off, const void *buf, uint64_t len)
{
    struct tfile *f = slot(idx);
    if (f == NULL)
        return WC_ENOENT;
    if (buf == NULL)
        return WC_EFAULT;
    if (len == 0)
        return 0;

    /* Clip to capacity, then compute again from the clipped length: writing 9
     * KiB into a 4 KiB file stores 4 KiB and says so, instead of failing the
     * whole call and leaving the program guessing how much landed. */
    if (off >= TMPFS_FILE_MAX)
        return 0;
    uint64_t room = TMPFS_FILE_MAX - off;
    uint64_t n = len < room ? len : room;

    if (off > f->size)                      /* the gap a seek-over-EOF leaves */
        memset(f->data + f->size, 0, (size_t)(off - f->size));

    memcpy(f->data + off, buf, (size_t)n);

    uint64_t end = off + n;
    if (end > f->size)
        f->size = end;
    return (int64_t)n;
}

int64_t tmpfs_dirent(uint64_t index, struct wc_dirent *out)
{
    if (out == NULL)
        return WC_EINVAL;

    /* Walk the pool counting live slots, so the enumeration has no holes: index
     * 2 is the third *file*, not pool[2], which may be free. */
    unsigned seen = 0;
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (!pool[i].used)
            continue;
        if (seen++ != (unsigned)index)
            continue;

        memset(out, 0, sizeof *out);
        size_t l = strlen(pool[i].name);
        if (l > WC_NAME_MAX - 1)
            l = WC_NAME_MAX - 1;            /* same rule as the archive's dirent */
        memcpy(out->name, pool[i].name, l);
        out->name[l] = '\0';
        out->namelen = l;
        out->size = pool[i].size;
        return 0;
    }
    return WC_ENOENT;
}
