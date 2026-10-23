/* WindClone — GDT audit + our own descriptor tables (Phase 4).
 *
 * Limine installs a valid 64-bit GDT before jumping to kmain, and until
 * Phase 3 all we did was *look* at it (`gdt_report`). That is enough for a
 * kernel that only ever runs in ring 0: one code selector, one data
 * selector, no privilege boundaries.
 *
 * The moment a user program exists, we need more:
 *   - a ring-3 code and data selector (DPL=3) the CPU will jump to,
 *   - a TSS, because on an interrupt taken *from ring 3* the CPU does not
 *     know which kernel stack to use — it reads RSP0 out of the TSS. No
 *     TSS means the first timer tick in userland triple-faults.
 *
 * Selector layout (kept deliberately textbook so `objdump`-style reading
 * of a crash dump still works):
 *   0x00 null | 0x08 kcode | 0x10 kdata | 0x18 udata | 0x20 ucode | 0x28 tss
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>
#include "gdt.h"
#include "serial.h"
#include "string.h"

/* --- descriptor formats ---------------------------------------------- */

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

/* A TSS descriptor is 16 bytes: the second half carries base bits 32..63. */
struct gdt_tss_entry {
    struct gdt_entry low;
    uint32_t base_high;
    uint32_t reserved;
} __attribute__((packed));

/* 64-bit Task State Segment. We use exactly one field: RSP0, the stack
 * the CPU switches to when an interrupt arrives while CPL=3. */
struct tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

_Static_assert(sizeof(struct tss) == 104, "64-bit TSS is 104 bytes");

/* One object, not two: the TSS descriptor MUST sit at the byte offset the
 * GDTR limit spans (index 5), and separate statics are free to be laid out
 * in any order the linker likes. Nested in a struct, the layout is fixed. */
struct gdt_table {
    struct gdt_entry     entries[5];
    struct gdt_tss_entry tss;
} __attribute__((packed));

static struct gdt_table gdt;
static struct tss       tss;
static struct gdtr      gdtr;

/* 16 KiB of kernel stack for interrupts that arrive while a process runs.
 * It must be static: at the moment we switch to it, the user stack is the
 * only other stack in play and it is not ours to trust. */
static uint8_t kernel_irq_stack[16 * 1024] __attribute__((aligned(16)));

/* Provided by stubs.asm. */
extern void gdt_flush(struct gdtr *gdtr);
extern void tss_flush(uint16_t selector);

/* from kernel.c */
void panic(const char *why);

static void set_entry(struct gdt_entry *e, uint8_t access, uint8_t flags)
{
    e->limit_low   = 0xFFFF;   /* flat: base 0, limit 4 GiB */
    e->base_low    = 0;
    e->base_mid    = 0;
    e->access      = access;
    e->granularity = (uint8_t)(flags | 0x0F);   /* limit high nibble */
    e->base_high   = 0;
}

void gdt_init(void)
{
    memset(&tss, 0, sizeof tss);
    tss.iomap_base = (uint16_t)sizeof(struct tss);   /* no I/O bitmap */

    set_entry(&gdt.entries[0], 0x00, 0x00);   /* 0x00: null — the CPU requires it */
    set_entry(&gdt.entries[1], 0x9A, 0xA0);   /* 0x08: ring-0 code, long mode     */
    set_entry(&gdt.entries[2], 0x92, 0xC0);   /* 0x10: ring-0 data                */
    set_entry(&gdt.entries[3], 0xF2, 0xC0);   /* 0x18: ring-3 data (DPL 3)        */
    set_entry(&gdt.entries[4], 0xFA, 0xA0);   /* 0x20: ring-3 code (DPL 3)        */

    /* 0x28: TSS descriptor — a *system* segment, so the layout (and the
     * access byte 0x89 = present, DPL 0, 64-bit TSS available) differs
     * from the code/data entries above, and it is twice as long. */
    struct gdt_entry *t = &gdt.tss.low;
    uint64_t base  = (uint64_t)(uintptr_t)&tss;
    uint32_t limit = (uint32_t)sizeof(struct tss) - 1;
    t->limit_low   = (uint16_t)(limit & 0xFFFF);
    t->base_low    = (uint16_t)(base & 0xFFFF);
    t->base_mid    = (uint8_t)((base >> 16) & 0xFF);
    t->access      = 0x89;
    t->granularity = (uint8_t)((limit >> 16) & 0x0F);
    t->base_high   = (uint8_t)((base >> 24) & 0xFF);
    gdt.tss.base_high = (uint32_t)(base >> 32);
    gdt.tss.reserved  = 0;

    gdtr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdtr.base  = (uint64_t)(uintptr_t)&gdt;

    gdt_flush(&gdtr);      /* lgdt + far return so CS becomes 0x08 */
    tss_flush(GDT_TSS_SELECTOR);

    tss.rsp0 = (uint64_t)(uintptr_t)(kernel_irq_stack + sizeof kernel_irq_stack);

    /* Read the selector back: if the far return did not take, every
     * interrupt gate built later would point at a descriptor we do not
     * own, and the first timer tick would triple-fault for no visible
     * reason. Cheap check, saves hours. */
    uint16_t cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    if (cs != GDT_KERNEL_CS)
        panic("gdt: far return did not load the kernel code selector");

    serial_write("gdt: our own table live, ring-3 selectors + TSS ready\n");
}

uint64_t gdt_irq_stack_top(void)
{
    return tss.rsp0;
}

/* --- audit of the table that is currently loaded ---------------------- */

void gdt_report(void)
{
    struct gdtr live;
    __asm__ volatile("sgdt %0" : "=m"(live));

    serial_write("gdt: base=");
    serial_write_hex(live.base);
    serial_write(" limit=");
    serial_write_hex(live.limit);
    serial_write("\n");

    const struct gdt_entry *table = (const struct gdt_entry *)live.base;
    uint64_t count = live.limit / sizeof(struct gdt_entry);
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
        serial_write_hex(table[i].access);
        if ((table[i].access & 0x80) == 0)
            serial_write(" (absent)");
        else if (table[i].access & 0x08)
            serial_write(" (code)");   /* bit 3 of type = executable */
        else
            serial_write(" (data)");
        if ((table[i].access & 0x60) == 0x60)
            serial_write(" DPL3");
        serial_write("\n");
    }
}
