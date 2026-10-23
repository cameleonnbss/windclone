/* WindClone — IDT construction, exception reporting, 8259 PIC driver.
 *
 * The IDT is the table the CPU consults on every interrupt/exception:
 * entry n holds the address of stub n. Limine leaves the BIOS/UEFI
 * identity of the two 8259 PIC chips intact, so IRQ 0-7 map onto the
 * same vectors as CPU exceptions — we MUST remap them (to 32-47)
 * before enabling interrupts, or a timer tick looks like a double fault.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>

#include "idt.h"
#include "io.h"
#include "serial.h"
#include "string.h"
#include "timer.h"
#include "keyboard.h"
#include "mouse.h"

/* --- IDT descriptor layout (AMD64 vol.2, "Interrupt Descriptor") ---- */

struct idt_entry {
    uint16_t base_low;    /* handler bits  0-15  */
    uint16_t selector;    /* code segment        */
    uint8_t  ist;         /* interrupt stack (0 = none) */
    uint8_t  type_attr;   /* 0x8E = 64-bit int gate, ring 0 */
    uint16_t base_mid;    /* handler bits 16-31  */
    uint32_t base_high;   /* handler bits 32-63  */
    uint32_t zero;        /* reserved            */
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* NOTE: the kernel is built -mno-sse (see Makefile). Early-boot code must
 * not contain SSE instructions: until we set up our own exception handling
 * (and CR4.XMME state is not guaranteed), a stray movdqa faults straight
 * into a triple fault. This whole file is therefore plain scalar code. */
static struct idt_entry idt_entries[256];
static struct idt_ptr   idt_pointer;

/* Kernel code selector: NOT hardcoded to the textbook 0x08 — Limine's
 * GDT keeps its own descriptors first (live cs was 0x28 on our boot).
 * We ask the CPU what segment it is actually executing in. */
static uint16_t kernel_cs;

/* Stubs defined in stubs.asm. */
extern void isr_stub_0(void);   extern void irq_stub_0(void);
#define ISR(n)  extern void isr_stub_##n(void)
#define IRQ(n)  extern void irq_stub_##n(void)
ISR(1);  ISR(2);  ISR(3);  ISR(4);  ISR(5);  ISR(6);  ISR(7);  ISR(8);
ISR(9);  ISR(10); ISR(11); ISR(12); ISR(13); ISR(14); ISR(15); ISR(16);
ISR(17); ISR(18); ISR(19); ISR(20); ISR(21); ISR(22); ISR(23); ISR(24);
ISR(25); ISR(26); ISR(27); ISR(28); ISR(29); ISR(30); ISR(31);
IRQ(1);  IRQ(2);  IRQ(3);  IRQ(4);  IRQ(5);  IRQ(6);  IRQ(7);  IRQ(8);
IRQ(9);  IRQ(10); IRQ(11); IRQ(12); IRQ(13); IRQ(14); IRQ(15);

static const void *const isr_stubs[32] = {
    isr_stub_0,  isr_stub_1,  isr_stub_2,  isr_stub_3,
    isr_stub_4,  isr_stub_5,  isr_stub_6,  isr_stub_7,
    isr_stub_8,  isr_stub_9,  isr_stub_10, isr_stub_11,
    isr_stub_12, isr_stub_13, isr_stub_14, isr_stub_15,
    isr_stub_16, isr_stub_17, isr_stub_18, isr_stub_19,
    isr_stub_20, isr_stub_21, isr_stub_22, isr_stub_23,
    isr_stub_24, isr_stub_25, isr_stub_26, isr_stub_27,
    isr_stub_28, isr_stub_29, isr_stub_30, isr_stub_31,
};

static const void *const irq_stubs[16] = {
    irq_stub_0,  irq_stub_1,  irq_stub_2,  irq_stub_3,
    irq_stub_4,  irq_stub_5,  irq_stub_6,  irq_stub_7,
    irq_stub_8,  irq_stub_9,  irq_stub_10, irq_stub_11,
    irq_stub_12, irq_stub_13, irq_stub_14, irq_stub_15,
};

void idt_set_gate_dpl(uint8_t n, const void *handler, uint8_t dpl)
{
    uint64_t addr = (uint64_t)handler;
    idt_entries[n].base_low   = (uint16_t)(addr & 0xFFFF);
    idt_entries[n].selector   = kernel_cs;
    idt_entries[n].ist        = 0;
    /* 0x8E = present, DPL 0, 64-bit interrupt gate. A gate ring 3 must be
     * able to invoke (int 0x80) carries its DPL in bits 5-6 instead. */
    idt_entries[n].type_attr  = (uint8_t)(0x8E | (uint8_t)((dpl & 3) << 5));
    idt_entries[n].base_mid   = (uint16_t)((addr >> 16) & 0xFFFF);
    idt_entries[n].base_high  = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    idt_entries[n].zero       = 0;
}

static void idt_set_gate(uint8_t n, const void *handler)
{
    idt_set_gate_dpl(n, handler, 0);
}

/* --- 8259 PIC ----------------------------------------------------------
 * Two chips, ports 0x20/0x21 (master) and 0xA0/0xA1 (slave).
 * Default vector mapping 0-7 / 8-15 collides with CPU exceptions. */

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20        /* end-of-interrupt command   */
#define ICW4_8086 0x01        /* final init word for modern CPUs */

void pic_remap(void)
{
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    outb(PIC1_CMD, 0x11); io_wait();   /* ICW1: init + cascade expected */
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, 32);  io_wait();   /* ICW2: IRQ0-7  -> vector 32-39 */
    outb(PIC2_DATA, 40);  io_wait();   /*       IRQ8-15 -> vector 40-47 */
    outb(PIC1_DATA, 0x04); io_wait();  /* ICW3: master has slave on IR2 */
    outb(PIC2_DATA, 0x02); io_wait();  /*       slave identity = 2      */
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    /* Start quiet: mask every line, the timer unmask comes from timer.c */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
    (void)mask1; (void)mask2;
}

void pic_mask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    outb(port, inb(port) | (1 << (irq & 7)));
}

void pic_unmask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    outb(port, inb(port) & ~(1 << (irq & 7)));
}

static void pic_eoi(uint8_t irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

/* --- exception names -------------------------------------------------- */

static const char *const exception_names[32] = {
    "divide error", "debug", "NMI", "breakpoint", "overflow",
    "bound range", "invalid opcode", "device not available",
    "double fault", "coprocessor segment overrun", "invalid TSS",
    "segment not present", "stack segment fault", "general protection",
    "page fault", "reserved", "x87 FP", "alignment check",
    "machine check", "SIMD FP", "virtualisation", "reserved",
    "reserved", "reserved", "reserved", "reserved", "reserved",
    "reserved", "reserved", "reserved", "hypervisor injection",
    "VMM communication",
};

/* --- C-side dispatch (called from the asm stubs) ----------------------- */

static void dump_reg(const char *name, uint64_t value)
{
    serial_write(name);
    serial_write("=");
    serial_write_hex(value);
    serial_write(" ");
}

__attribute__((noreturn))
void isr_handler(struct regs *r)
{
    cli();
    serial_write("\n[WINDCLONE] CPU EXCEPTION");
    if (r->vector < 32) {
        serial_write(": ");
        serial_write(exception_names[r->vector]);
    }
    serial_write("\n");

    dump_reg("vector", r->vector);
    dump_reg("err", r->error_code);
    serial_write("\nrip=");   serial_write_hex(r->rip);
    serial_write(" cs=");     serial_write_hex(r->cs);
    serial_write(" rflags="); serial_write_hex(r->rflags);
    serial_write("\n");

    dump_reg("rax", r->rax); dump_reg("rbx", r->rbx);
    dump_reg("rcx", r->rcx); dump_reg("rdx", r->rdx);
    serial_write("\n");
    dump_reg("rsi", r->rsi); dump_reg("rdi", r->rdi);
    dump_reg("rbp", r->rbp); dump_reg("rsp", (uint64_t)r);
    serial_write("\n");
    dump_reg("r8", r->r8);   dump_reg("r9", r->r9);
    dump_reg("r10", r->r10); dump_reg("r11", r->r11);
    serial_write("\n");
    dump_reg("r12", r->r12); dump_reg("r13", r->r13);
    dump_reg("r14", r->r14); dump_reg("r15", r->r15);
    serial_write("\n");

    if (r->vector == 14) {   /* page fault: CR2 carries the bad address */
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        dump_reg("cr2", cr2);
        serial_write("\n");
    }

    serial_write("exception is fatal - CPU halted (no reboot loop).\n");
    for (;;)
        hlt();
}

void irq_handler(struct regs *r)
{
    uint8_t irq = (uint8_t)(r->vector - 32);

    /* One-shot marker: proves the whole IRQ path (PIT -> PIC -> stub ->
     * dispatch) works end to end. CI greps the serial log for it. */
    static int first = 1;
    if (first && irq == 0) {
        serial_write("irq0: timer interrupt firing\n");
        first = 0;
    }

    /* Spurious PIC noise: IRQ7/15 can fire without a real device
     * service bit set — ignore them (no handler, no second EOI). */
    if (irq == 7 || irq == 15) {
        uint16_t rad = (irq == 7) ? 0x20 : 0xA0;
        outb(rad, 0x0B); io_wait();
        if ((inb(rad) & 0x80) == 0)
            return;
    }

    if (irq == 0)
        timer_tick();
    else if (irq == 1)
        keyboard_irq();
    else if (irq == 12)
        mouse_irq();          /* PS/2 aux channel */

    pic_eoi(irq);
}

/* --- init --------------------------------------------------------------- */

void idt_init(void)
{
    /* Do NOT trust the firmware/bootloader interrupt flag: the legacy
     * PIC still maps IRQ0 onto vector 8 (double fault!) until pic_remap,
     * so a stray timer tick mid-init would triple-fault us. */
    cli();

    /* Ask the CPU which code selector we actually run under — Limine
     * uses 0x28, not the textbook 0x08. */
    __asm__ volatile("mov %%cs, %0" : "=r"(kernel_cs));

    memset(idt_entries, 0, sizeof(idt_entries));

    for (int i = 0; i < 32; i++)
        idt_set_gate((uint8_t)i, isr_stubs[i]);
    /* IRQ stubs live at vectors 32..47 after the PIC remap. */
    for (int i = 0; i < 16; i++)
        idt_set_gate((uint8_t)(32 + i), irq_stubs[i]);
    /* 48..255 stay NULL gates: an interrupt there is a #GP we can log. */

    idt_pointer.limit = sizeof(idt_entries) - 1;
    idt_pointer.base  = (uint64_t)&idt_entries[0];
    __asm__ volatile("lidt %0" : : "m"(idt_pointer));

    pic_remap();
}
