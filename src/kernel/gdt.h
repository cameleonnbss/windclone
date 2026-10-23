/* WindClone — descriptor tables.
 *
 * gdt_init() installs our own GDT (ring-0 code/data, ring-3 code/data,
 * TSS) and loads the TSS; gdt_report() dumps whatever table is live, which
 * stays useful as an audit even after we own it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_GDT_H
#define WINDCLONE_GDT_H

/* Selector indices (byte offsets into the GDT). */
#define GDT_KERNEL_CS 0x08
#define GDT_KERNEL_DS 0x10
#define GDT_USER_DS   0x18      /* | 3 at use time -> 0x1B */
#define GDT_USER_CS   0x20      /* | 3 at use time -> 0x23 */
#define GDT_TSS_SELECTOR 0x28

void gdt_init(void);     /* install our GDT + TSS, then verify CS */
void gdt_report(void);   /* dump the live GDT + selectors to serial */

/* The stack the CPU switches to on an interrupt taken from ring 3.
 * Set by gdt_init(); read by the userland entry code, which re-points it
 * at a per-process stack when there is more than one process. */
uint64_t gdt_irq_stack_top(void);

#endif
