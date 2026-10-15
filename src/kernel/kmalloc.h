/* WindClone — kernel heap: kmalloc / kfree.
 *
 * The heap is a fixed window of virtual addresses in the low half
 * (HEAP_BASE, far from both the kernel and the HHDM). Each page we
 * actually need gets one physical frame mapped on demand through
 * paging_map_page() — so the heap grows page by page, never back.
 *
 * Allocation strategy is the classic first-fit free list with block
 * splitting and forward coalescing. Simplicity beats speed here: the
 * numbers we handle at boot are KiB, not GiB.
 *
 * NOT interrupt-safe: do not call kmalloc() from an IRQ handler yet.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_KMALLOC_H
#define WINDCLONE_KMALLOC_H

#include <stdint.h>
#include <stddef.h>

/* Map + initialise the heap arena. Requires paging_init() first. */
void kmalloc_init(void);

void *kmalloc(size_t bytes);
void  kfree(void *ptr);
void *kcalloc(size_t count, size_t bytes);   /* zeroed, one block */

/* Bytes of heap that are currently mapped into physical frames. */
size_t heap_mapped_bytes(void);

#endif
