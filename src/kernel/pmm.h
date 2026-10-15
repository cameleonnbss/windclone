/* WindClone — physical frame allocator (bitmap-based).
 *
 * The kernel sees RAM as 4 KiB "frames". This module keeps one bitmap
 * bit per frame: 0 = free, 1 = used. Frames below 1 MiB and the kernel
 * image itself are marked used at init. Allocation is LAST-FIT (from
 * the top of RAM down): keeping low memory free matters later for
 * device DMA (sound cards of the 1990s could only address 16 MiB).
 *
 * All physical addresses handed out are reachable with pmm_phys_to_virt()
 * through the Limine higher-half direct map (HHDM).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_PMM_H
#define WINDCLONE_PMM_H

#include <stdint.h>
#include <stddef.h>

#include <limine.h>

#define FRAME_SIZE 4096ULL

/* Build the free/used bitmap from the Limine memory map. `kernel_virt_start`
 * / `kernel_virt_end` bound the kernel image (linker symbols); its physical
 * pages are reserved through the HHDM offset. Panics on a missing memmap. */
void pmm_init(struct limine_memmap_response *memmap,
              uint64_t hhdm_offset,
              uint64_t kernel_virt_start, uint64_t kernel_virt_end);

/* One free frame's *physical* address, or 0 when out of memory.
 * (Frame 0 is never handed out, so 0 is a safe failure sentinel.) */
uint64_t pmm_alloc_frame(void);

/* Return a frame obtained from pmm_alloc_frame(). No-op on 0. */
void pmm_free_frame(uint64_t phys);

/* HHDM translation: physical -> always-mapped kernel virtual. */
void *pmm_phys_to_virt(uint64_t phys);
uint64_t pmm_virt_to_phys(void *virt);

uint64_t pmm_total_frames(void);   /* frames the bitmap covers      */
uint64_t pmm_free_frames(void);    /* currently allocatable frames  */

#endif
