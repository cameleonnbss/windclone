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

/* The entry that covers `virt`, flag bits included, or 0 if absent. Needed
 * before the kernel touches an address a user program supplied: the page may
 * exist and still be read-only, and writing to it would then fault inside the
 * kernel instead of answering -WC_EFAULT. */
uint64_t paging_flags(uint64_t virt);

/* --- per-process address spaces (Phase 5) ----------------------------
 *
 * A process needs its own mapping of the *user window* while still seeing
 * the kernel: text, data, the HHDM, the heap at 256 GiB and the VGA window
 * must all stay put, or the kernel faults the moment it runs under the
 * process's CR3. paging_clone_address_space() gives exactly that: a fresh
 * P4 that copies the boot/kernel table wholesale (so every kernel mapping is
 * shared by reference) and then makes the user range 0x400000..2 GiB private
 * and empty, ready for the loader to fill.
 *
 * The kernel walks and edits ONE table at a time through the `p4` pointer.
 * These calls make that pointer a parameter of the scheduler's trade:
 *   - paging_set_working() points the walker at a table (by virtual address);
 *   - paging_get_working() reads it back;
 *   - paging_switch() loads a *physical* P4 base into CR3;
 *   - paging_kernel_cr3() is the address the boot table lives at, so the
 *     scheduler can leave a process and return to the kernel's own view. */
uint64_t paging_clone_address_space(void);        /* -> physical base of new P4 */
void paging_set_working(uint64_t *virt_p4);
uint64_t *paging_get_working(void);
void paging_switch(uint64_t cr3_phys);
uint64_t paging_kernel_cr3(void);
/* Point the walker back at the boot table and load its CR3: how a scheduler
 * leaves the last process and returns the kernel to its own view. */
void paging_use_kernel(void);

/* Every intermediate table frame allocated by a clone or a later map is
 * reported here so the caller can free the whole private tree on exit without
 * walking it (and without ever touching a shared kernel table). A NULL sink
 * turns reporting off. The P4/PDPT/PD frames the clone itself allocates are
 * reported too, so the list owns the process's entire private page tree. */
void paging_set_table_sink(uint64_t *sink, unsigned *count, unsigned cap);

/* Free every table frame in `tables` (the sink list). Leaf frames are the
 * caller's to free separately; this only releases the page-tree nodes. */
void paging_free_table_frames(const uint64_t *tables, unsigned count);

#endif
