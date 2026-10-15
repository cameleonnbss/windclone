/* WindClone — GDT audit.
 *
 * Limine already installed a valid 64-bit GDT (code at 0x08, data at
 * 0x10) before jumping to kmain, so the right first move is NOT to
 * reload it — it's to *look at it* and understand what a segments
 * register actually references. Phase 5 (userland) will install our
 * own GDT with ring-3 selectors; until then this file reports the
 * live table to the serial console.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "serial.h"

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* One AMD64 GDT entry: 8 bytes of flags/base packed into 16 bytes. */
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;      /* P|DPL|S|Type                    */
    uint8_t  granularity; /* G|DB|L|AVL|limit_hi             */
    uint8_t  base_high;
} __attribute__((packed));

void gdt_report(void)
{
    struct gdtr gdtr;
    __asm__ volatile("sgdt %0" : "=m"(gdtr));

    serial_write("gdt: base=");
    serial_write_hex(gdtr.base);
    serial_write(" limit=");
    serial_write_hex(gdtr.limit);
    serial_write("\n");

    const struct gdt_entry *gdt = (const struct gdt_entry *)gdtr.base;
    uint64_t count = gdtr.limit / sizeof(struct gdt_entry);
    if (count > 8)
        count = 8;   /* the first few are what we care about */

    /* Which selectors does the CPU actually use right now? */
    uint16_t cs, ds, ss;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    __asm__ volatile("mov %%ds, %0" : "=r"(ds));
    __asm__ volatile("mov %%ss, %0" : "=r"(ss));

    serial_write("live selectors: cs=");
    serial_write_hex(cs);
    serial_write(" ds=");
    serial_write_hex(ds);
    serial_write(" ss=");
    serial_write_hex(ss);
    serial_write("\n");

    for (uint64_t i = 0; i < count; i++) {
        serial_write("  gdt[");
        serial_write_dec(i);
        serial_write("] access=");
        serial_write_hex(gdt[i].access);
        if ((gdt[i].access & 0x80) == 0)
            serial_write(" (absent)");
        else if (gdt[i].access & 0x08)
            serial_write(" (code)");   /* bit 3 of type = executable */
        else
            serial_write(" (data)");
        serial_write("\n");
    }
}
