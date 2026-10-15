/* WindClone — kernel heap implementation (first-fit free list).
 * SPDX-License-Identifier: MIT
 */
#include "kmalloc.h"
#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "string.h"

void panic(const char *why);

/* Virtual window for the heap: 256 GiB mark. Comfortably far from
 * virtual 0 (NULL trap), from the kernel at -2 GiB and from whatever
 * range the HHDM occupies. */
#define HEAP_BASE        0x4000000000ULL
#define HEAP_MAX_SIZE    (16ULL * 1024 * 1024)   /* virtual reservation */
#define HEAP_GROW_STEP   (64ULL * 1024)          /* map in 64 KiB units */

#define BLOCK_MAGIC      0x5743484BU             /* "WCHEK" */
#define HDR_SIZE         32                      /* sizeof(block) padded */
#define ALIGN_UP(x, a)   (((x) + (a) - 1) & ~((uint64_t)(a) - 1))

struct block {
    uint64_t magic;               /* 0  */
    uint64_t size;                /* 8  : payload bytes */
    struct block *next;           /* 16 : always ascending addresses */
    int      used;                /* 24 */
    uint8_t  pad[4];              /* 28 -> exactly HDR_SIZE = 32 */
};

static struct block *list_head;
static uint64_t heap_used;        /* bytes of the arena already carved   */
static uint64_t heap_mapped;      /* bytes actually backed by frames     */

static void add_block(void *addr, uint64_t size)
{
    struct block *b = addr;
    b->magic = BLOCK_MAGIC;
    b->size  = size;
    b->used  = 0;
    b->next  = NULL;
    if (list_head == NULL)
        list_head = b;
    else {
        struct block *t = list_head;
        while (t->next != NULL)
            t = t->next;
        t->next = b;
    }
}

/* Back `want` more bytes of the virtual window with real frames. */
static int heap_grow(uint64_t want)
{
    uint64_t target = heap_used + ALIGN_UP(want, FRAME_SIZE);
    if (target > HEAP_MAX_SIZE)
        return 0;                       /* reservation exhausted */
    while (heap_mapped < target) {
        uint64_t phys = pmm_alloc_frame();
        if (phys == 0)
            return 0;
        paging_map_page(HEAP_BASE + heap_mapped, phys,
                        PTE_PRESENT | PTE_WRITE);
        heap_mapped += FRAME_SIZE;
    }
    return 1;
}

void kmalloc_init(void)
{
    list_head = NULL;
    heap_used = 0;
    heap_mapped = 0;

    /* Seed the arena with one block right at the base. */
    if (!heap_grow(HEAP_GROW_STEP))
        panic("kmalloc: cannot back the first heap pages");
    uint64_t payload = HEAP_GROW_STEP - HDR_SIZE;
    add_block((void *)HEAP_BASE, payload);
    heap_used = HEAP_GROW_STEP;

    serial_write("kmalloc: heap live at 0x4000000000, ");
    serial_write_dec(heap_mapped / 1024);
    serial_write(" KiB mapped\n");
}

void *kmalloc(size_t bytes_v)
{
    uint64_t bytes = ALIGN_UP(bytes_v, 16);
    if (bytes == 0)
        bytes = 16;

    for (struct block *b = list_head; b != NULL; b = b->next) {
        if (b->used || b->magic != BLOCK_MAGIC || b->size < bytes)
            continue;
        /* split the tail off if it is worth a new block */
        if (b->size >= bytes + HDR_SIZE + 16) {
            struct block *fresh = (struct block *)((uint8_t *)b + HDR_SIZE + bytes);
            fresh->magic = BLOCK_MAGIC;
            fresh->size  = b->size - bytes - HDR_SIZE;
            fresh->used  = 0;
            fresh->next  = b->next;
            b->next      = fresh;
            b->size      = bytes;
        }
        b->used = 1;
        return (uint8_t *)b + HDR_SIZE;
    }

    /* nothing free: grow the arena and retry once */
    if (!heap_grow(bytes + HDR_SIZE + HEAP_GROW_STEP))
        return NULL;
    add_block((void *)(HEAP_BASE + heap_used),
              heap_mapped - heap_used - HDR_SIZE);
    heap_used += ALIGN_UP(heap_mapped - heap_used, FRAME_SIZE);
    return kmalloc(bytes_v);
}

void kfree(void *ptr_v)
{
    if (ptr_v == NULL)
        return;
    struct block *b = (struct block *)((uint8_t *)ptr_v - HDR_SIZE);
    if (b->magic != BLOCK_MAGIC)
        panic("kfree: corruption or foreign pointer detected");
    if (!b->used)
        panic("kfree: double free");
    b->used = 0;
    /* forward coalesce: absorb the next block while it is adjacent
     * and already free — the classic trick that beats fragmentation */
    while (b->next != NULL && b->next->used == 0 &&
           b->next == (struct block *)((uint8_t *)b + HDR_SIZE + b->size)) {
        struct block *n = b->next;
        b->size += HDR_SIZE + n->size;
        b->next  = n->next;
    }
}

void *kcalloc(size_t count, size_t bytes)
{
    if (count == 0 || bytes == 0)
        return kmalloc(0);              /* avoid the divide-by-zero below */
    uint64_t total = (uint64_t)count * (uint64_t)bytes;
    if (total / (uint64_t)bytes != (uint64_t)count)
        return NULL;                    /* multiplication overflow */
    void *p = kmalloc((size_t)total);
    if (p != NULL)
        memset(p, 0, (size_t)total);
    return p;
}

size_t heap_mapped_bytes(void)
{
    return (size_t)heap_mapped;
}
