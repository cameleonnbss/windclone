/* WindClone — physical frame allocator implementation.
 * SPDX-License-Identifier: MIT
 */
#include "pmm.h"
#include "serial.h"
#include "string.h"

void panic(const char *why);

/* Bitmap covers up to PMM_MAX_PHYS bytes of RAM. 1 bit per 4 KiB frame:
 * 4 GiB of RAM -> 1 MiB of bitmap. Anything above the cap is simply not
 * managed yet (we boot QEMU with 512 MiB; raise the cap when needed). */
#define PMM_MAX_PHYS (4ULL * 1024 * 1024 * 1024)
#define PMM_MAX_FRAMES (PMM_MAX_PHYS / FRAME_SIZE)

static uint8_t bitmap[PMM_MAX_FRAMES / 8];

static uint64_t hhdm;            /* HHDM offset, cached for translations   */
static uint64_t top_frame;       /* first frame *above* managed RAM        */
static uint64_t cursor = 1;      /* last-fit allocation: scan downwards    */
static uint64_t free_frames;     /* allocatable frames right now           */
static uint64_t total_frames;

static inline void set_used(uint64_t f)
{
    if (f < top_frame && !(bitmap[f / 8] & (1u << (f % 8)))) {
        bitmap[f / 8] |= (uint8_t)(1u << (f % 8));
        free_frames--;
    }
}

static inline void set_free(uint64_t f)
{
    if (f < top_frame && (bitmap[f / 8] & (1u << (f % 8)))) {
        bitmap[f / 8] &= (uint8_t)~(1u << (f % 8));
        free_frames++;
    }
}

static inline int is_used(uint64_t f)
{
    return (bitmap[f / 8] & (1u << (f % 8))) != 0;
}

void pmm_init(struct limine_memmap_response *memmap,
              uint64_t hhdm_offset,
              uint64_t kernel_virt_start, uint64_t kernel_virt_end)
{
    if (memmap == NULL)
        panic("pmm: bootloader gave us no memory map");
    hhdm = hhdm_offset;

    /* Start from "everything is used" and open holes only where the
     * firmware says the RAM is actually usable. */
    memset(bitmap, 0xFF, sizeof(bitmap));

    uint64_t highest = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->base + e->length > highest)
            highest = e->base + e->length;
    }
    if (highest > PMM_MAX_PHYS)
        highest = PMM_MAX_PHYS;          /* cap; report below via serial */
    top_frame = (highest + FRAME_SIZE - 1) / FRAME_SIZE;
    total_frames = top_frame;

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;                     /* reserved/bad: stay marked used */
        uint64_t base = e->base, end = e->base + e->length;
        if (end > PMM_MAX_PHYS)
            end = PMM_MAX_PHYS;
        /* frame-align the range inward: partial frames stay reserved */
        base = (base + FRAME_SIZE - 1) / FRAME_SIZE * FRAME_SIZE;
        end  = end / FRAME_SIZE * FRAME_SIZE;
        for (uint64_t a = base; a < end; a += FRAME_SIZE)
            set_free(a / FRAME_SIZE);
    }

    /* Reserve the first MiB: real-mode IVT, BIOS data area, the VGA
     * window at 0xB8000, and whatever the 16-bit world left there. */
    for (uint64_t f = 0; f < 0x100000 / FRAME_SIZE; f++)
        set_used(f);

    /* Reserve our own kernel image: virtual -> physical via the HHDM
     * (Limine loads it as one contiguous run, .bss included). */
    uint64_t kphys_start = kernel_virt_start - hhdm;
    uint64_t kphys_end   = kernel_virt_end - hhdm;
    kphys_start = kphys_start / FRAME_SIZE * FRAME_SIZE;
    kphys_end   = (kphys_end + FRAME_SIZE - 1) / FRAME_SIZE * FRAME_SIZE;
    for (uint64_t a = kphys_start; a < kphys_end; a += FRAME_SIZE)
        set_used(a / FRAME_SIZE);

    cursor = top_frame - 1;               /* last-fit: start at the top */

    serial_write("pmm: frames=");
    serial_write_dec(total_frames);
    serial_write(" free=");
    serial_write_dec(free_frames);
    serial_write(" (bitmap covers ");
    serial_write_dec(PMM_MAX_PHYS / (1024 * 1024));
    serial_write(" MiB)\n");
}

uint64_t pmm_alloc_frame(void)
{
    /* scan downwards from the cursor (last-fit) for a free frame */
    for (uint64_t f = cursor; f >= 1; f--) {
        if (!is_used(f)) {
            set_used(f);
            cursor = (f >= 1) ? f - 1 : 1;
            return f * FRAME_SIZE;
        }
    }
    return 0;                             /* out of memory */
}

void pmm_free_frame(uint64_t phys)
{
    if (phys == 0)
        return;
    uint64_t f = phys / FRAME_SIZE;
    if (f >= top_frame)
        return;                           /* outside managed RAM: ignore */
    set_free(f);
    if (f > cursor)
        cursor = f;                       /* let last-fit climb back up */
}

void *pmm_phys_to_virt(uint64_t phys)
{
    return (void *)(uintptr_t)(phys + hhdm);
}

uint64_t pmm_virt_to_phys(void *virt)
{
    return (uint64_t)(uintptr_t)virt - hhdm;
}

uint64_t pmm_total_frames(void) { return total_frames; }
uint64_t pmm_free_frames(void)  { return free_frames; }
