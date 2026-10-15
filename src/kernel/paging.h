/* WindClone — x86_64 4-level page tables (ours, not Limine's).
 *
 * paging_init() builds a fresh P4 but *reuses* Limine's higher-half
 * entries (P4 slots 256..511: kernel + HHDM stay exactly as mapped),
 * then hands the low half to us:
 *   - virtual 0x000000..0x200000 is left unmapped -> NULL-page guard
 *   - the VGA MMIO window is mapped at its own physical address
 *   - the kmalloc heap gets a fixed low-region window (see kmalloc.c)
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_PAGING_H
#define WINDCLONE_PAGING_H

#include <stdint.h>
#include <stddef.h>

/* page-table entry bits we care about */
#define PTE_PRESENT   0x001ULL
#define PTE_WRITE     0x002ULL
#define PTE_USER      0x004ULL
#define PTE_PCD       0x010ULL   /* cache-disable: mandatory for MMIO     */
#define PTE_PS        0x080ULL   /* "huge page" (2 MiB / 1 GiB) marker    */
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

/* Build our P4 (copying Limine's higher-half entries), map the VGA
 * text window, and load the new CR3. Requires pmm_init() first. */
void paging_init(void);

/* Map one 4 KiB page. Allocates intermediate tables on demand.
 * Panics if a huge page blocks the path (would need splitting). */
void paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

/* Remove a mapping (leaf cleared + TLB slot flushed). */
void paging_unmap_page(uint64_t virt);

/* Walk the live tables: physical address of `virt`, or 0 if absent. */
uint64_t paging_translate(uint64_t virt);

#endif
