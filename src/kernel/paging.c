/* WindClone — page table management implementation.
 * SPDX-License-Identifier: MIT
 */
#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "string.h"

void panic(const char *why);

#define VGA_PHYS 0xB8000ULL          /* classic text-mode MMIO window */

static uint64_t *p4;                 /* our P4, reachable via HHDM */

static inline uint64_t idx(uint64_t virt, int level)
{
    /* level 4 = P4 slot .. level 1 = P1 (leaf) table slot */
    return (virt >> (12 + 9 * (level - 1))) & 0x1FF;
}

/* Return a zeroed, freshly allocated table; hang it on `slot`. */
static uint64_t *next_table(uint64_t *slot, int level)
{
    if (*slot & PTE_PRESENT) {
        if (*slot & PTE_PS)
            panic("paging: a huge page blocks our 4 KiB mapping path");
        return pmm_phys_to_virt(*slot & PTE_ADDR_MASK);
    }
    uint64_t phys = pmm_alloc_frame();
    if (phys == 0)
        panic("paging: out of frames while building page tables");
    uint64_t *tbl = pmm_phys_to_virt(phys);
    memset(tbl, 0, FRAME_SIZE);
    *slot = phys | PTE_PRESENT | PTE_WRITE;
    (void)level;
    return tbl;
}

void paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags)
{
    virt &= ~0xFFFULL;
    phys &= PTE_ADDR_MASK;

    uint64_t *tbl = p4;
    tbl = next_table(&tbl[idx(virt, 4)], 4);
    tbl = next_table(&tbl[idx(virt, 3)], 3);
    tbl = next_table(&tbl[idx(virt, 2)], 2);
    tbl[idx(virt, 1)] = phys | flags | PTE_PRESENT;
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

void paging_unmap_page(uint64_t virt)
{
    virt &= ~0xFFFULL;
    uint64_t *tbl = p4;
    /* if any level is missing there is nothing to unmap */
    for (int level = 4; level >= 2; level--) {
        uint64_t slot = tbl[idx(virt, level)];
        if (!(slot & PTE_PRESENT))
            return;
        tbl = pmm_phys_to_virt(slot & PTE_ADDR_MASK);
    }
    tbl[idx(virt, 1)] = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

uint64_t paging_translate(uint64_t virt)
{
    uint64_t *tbl = p4;
    /* descend P4 -> P3 -> P2 (levels 4..2); level 1 is the leaf table */
    for (int level = 4; level >= 2; level--) {
        uint64_t slot = tbl[idx(virt, level)];
        if (!(slot & PTE_PRESENT))
            return 0;
        if (slot & PTE_PS) {
            /* huge page (1 GiB at P3, 2 MiB at P2): resolve in place */
            uint64_t shift = 12 + 9 * (level - 1);
            uint64_t mask  = (1ULL << shift) - 1;
            return (slot & ~mask & PTE_ADDR_MASK) | (virt & mask);
        }
        tbl = pmm_phys_to_virt(slot & PTE_ADDR_MASK);
    }
    uint64_t leaf = tbl[idx(virt, 1)];
    if (!(leaf & PTE_PRESENT))
        return 0;
    return (leaf & PTE_ADDR_MASK) | (virt & 0xFFF);
}

void paging_init(void)
{
    /* 1. Where is Limine's P4 right now? CR3 holds its physical base. */
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t *limine_p4 = pmm_phys_to_virt(cr3 & PTE_ADDR_MASK);

    /* 2. Our own P4. Copy ONLY the higher half (slots 256..511): that
     *    keeps the kernel + HHDM mapped exactly as Limine set them up,
     *    while the whole lower half becomes ours to hand out. Leaving
     *    virtual page 0 unmapped gives us a free NULL-pointer trap. */
    uint64_t p4_phys = pmm_alloc_frame();
    if (p4_phys == 0)
        panic("paging: no frame for the new P4");
    p4 = pmm_phys_to_virt(p4_phys);
    memset(p4, 0, FRAME_SIZE);
    for (int i = 256; i < 512; i++)
        p4[i] = limine_p4[i];

    /* 3. Go live with our tables. */
    __asm__ volatile("mov %0, %%cr3" : : "r"(p4_phys) : "memory");

    /* 4. Map the VGA text buffer at virtual 0xB8000. It is MMIO, not
     *    RAM: the Limine HHDM deliberately does not cover it, so this
     *    is the first device we page in ourselves (PCD = uncached). */
    paging_map_page(VGA_PHYS, VGA_PHYS, PTE_PRESENT | PTE_WRITE | PTE_PCD);

    serial_write("paging: own P4 live; vga window mapped at 0xB8000\n");

    /* 5. Prove the machinery works before anyone trusts it. */
    if (paging_translate(VGA_PHYS) != VGA_PHYS)
        panic("paging: self-check of the VGA mapping failed");
    if (paging_translate(0) != 0)
        panic("paging: NULL page unexpectedly mapped");
    serial_write("paging: self-checks passed (VGA translates, NULL page traps)\n");
}
