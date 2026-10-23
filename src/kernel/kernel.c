/* WindClone — microkernel skeleton, x86_64 long mode via Limine
 * SPDX-License-Identifier: MIT
 *
 * This is the first code that runs after the Limine bootloader has:
 *   - switched the CPU to 64-bit long mode
 *   - enabled paging (4-level)
 *   - mapped the kernel into the higher half (0xffffffff80100000)
 *   - set up the HHDM (higher-half direct map) so we can reach
 *     any physical address (like the VGA buffer at 0xB8000)
 */

#include <stdint.h>
#include <stddef.h>
#include <limine.h>

#include "serial.h"
#include "vga.h"
#include "io.h"
#include "gdt.h"
#include "idt.h"
#include "timer.h"
#include "pmm.h"
#include "paging.h"
#include "kmalloc.h"
#include "kassert.h"
#include "log.h"
#include "pci.h"
#include "keyboard.h"
#include "initrd.h"
#include "string.h"

/* Boundaries of our own image, straight from the linker script. */
extern char __kernel_start[];
extern char __kernel_end[];

/* ------------------------------------------------------------------ */
/* Limine boot requests — these are filled in by the bootloader       */
/* before kmain is ever called. The section attributes + linker       */
/* script KEEP() lines are what make Limine find them.                */
/* ------------------------------------------------------------------ */

__attribute__((used, section(".limine_requests_start_marker")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0,
    .response = NULL,
};

__attribute__((section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
    .response = NULL,
};

__attribute__((section(".limine_requests")))
static volatile struct limine_firmware_type_request firmware_type_request = {
    .id = LIMINE_FIRMWARE_TYPE_REQUEST,
    .revision = 0,
    .response = NULL,
};

__attribute__((used, section(".limine_requests_end_marker")))
static volatile LIMINE_REQUESTS_END_MARKER;

/* ------------------------------------------------------------------ */
/* Kernel diagnostics                                                  */
/* ------------------------------------------------------------------ */

/* Once our own page tables are live we can at least check that a
 * candidate rbp points at mapped memory before dereferencing it —
 * a backtrace must never be the thing that crashes us further. */
static int paging_is_live;

static void backtrace(uint64_t rbp, uint64_t top_rip)
{
    serial_write("stack walk (rip first, decode with `nm build/kernel.elf`):\n  #0 rip=");
    serial_write_hex(top_rip);
    serial_write("\n");
    for (int i = 1; i < 12; i++) {
        /* SysV frame: [rbp] = saved rbp, [rbp+8] = return address.
         * Require 8-alignment and ascending frames; stop at anything
         * that smells like garbage or an unmapped page. */
        if (rbp < 0x1000 || (rbp & 7) != 0)
            break;
        if (paging_is_live && paging_translate(rbp) == 0)
            break;
        uint64_t *frame = (uint64_t *)(uintptr_t)rbp;
        uint64_t ret = frame[1];
        uint64_t next = frame[0];
        if (ret < 0x1000 || next <= rbp)
            break;
        serial_write("  #");
        serial_write_dec((uint64_t)i);
        serial_write(" rip=");
        serial_write_hex(ret);
        serial_write("\n");
        rbp = next;
    }
}

void panic(const char *why)
{
    serial_write("\n[WINDCLONE] KERNEL PANIC: ");
    serial_write(why);
    serial_write("\n");
    if (vga_available()) {
        kprint("KERNEL PANIC: ");
        kprint(why);
        kprint("\n");
    }
    backtrace((uint64_t)(uintptr_t)__builtin_frame_address(0),
              (uint64_t)(uintptr_t)&panic);
    for (;;)
        __asm__ volatile("cli; hlt");
}

void kassert_fail(const char *file, int line, const char *expr)
{
    serial_write("\n[WINDCLONE] ASSERT FAILED: ");
    serial_write(expr);
    serial_write("\n  at ");
    serial_write(file);
    serial_write(":");
    serial_write_dec((uint64_t)line);
    serial_write("\n");
    if (vga_available()) {
        kprint("ASSERT FAILED: ");
        kprint(expr);
        kprint("\n");
    }
    backtrace((uint64_t)(uintptr_t)__builtin_frame_address(0),
              (uint64_t)(uintptr_t)&kassert_fail);
    for (;;)
        __asm__ volatile("cli; hlt");
}

/* Total usable RAM, reported by the boot memory map. */
static uint64_t usable_ram_bytes(struct limine_memmap_response *m)
{
    uint64_t total = 0;
    if (m == NULL)
        return 0;
    for (uint64_t i = 0; i < m->entry_count; i++) {
        struct limine_memmap_entry *e = m->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE)
            total += e->length;
    }
    return total;
}

/* ------------------------------------------------------------------ */
/* Phase 2 self-tests                                                  */
/* ------------------------------------------------------------------ */

/* VGA-side decimal printer (serial already has serial_write_dec). */
static void kprint_dec(uint64_t v)
{
    char buf[24];
    int n = 0;
    if (v == 0)
        buf[n++] = '0';
    while (v != 0) {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    char out[24];
    for (int i = 0; i < n; i++)
        out[i] = buf[n - 1 - i];
    out[n] = '\0';
    kprint(out);
}

static void heap_selftest(void)
{
    /* allocate -> pattern -> verify -> free -> reuse */
    uint8_t *a = kmalloc(100);
    uint8_t *b = kmalloc(250);
    uint8_t *c = kmalloc(4096);
    kassert(a != NULL && b != NULL && c != NULL);
    kassert(((uintptr_t)a & 7) == 0);           /* alignment sanity */

    memset(a, 0xAA, 100);
    memset(b, 0x55, 250);
    memset(c, 0x33, 4096);
    for (int i = 0; i < 100; i++) kassert(a[i] == 0xAA);
    for (int i = 0; i < 250; i++) kassert(b[i] == 0x55);
    for (int i = 0; i < 4096; i++) kassert(c[i] == 0x33);

    /* blocks must not overlap: a's pattern would trash b if they did */
    kassert(memset(b, 0x55, 250) == b);
    kassert(a[99] == 0xAA);

    void *d = kcalloc(64, 64);                    /* zero-filled */
    kassert(d != NULL);
    for (int i = 0; i < 64 * 64; i++)
        kassert(((uint8_t *)d)[i] == 0);

    kfree(b);
    kfree(a);
    void *e = kmalloc(300);   /* must reuse the coalesced a+b hole */
    kassert(e != NULL);
    kfree(e);
    kfree(c);
    kfree(d);

    serial_write("kmalloc selftest ok (heap ");
    serial_write_dec(heap_mapped_bytes() / 1024);
    serial_write(" KiB mapped, free frames ");
    serial_write_dec(pmm_free_frames());
    serial_write(")\n");
}

/* ------------------------------------------------------------------ */
/* kmain — the kernel entry point (ELF e_entry)                        */
/* ------------------------------------------------------------------ */

void kmain(void)
{
    /* 1. Serial first: it works on BIOS *and* UEFI, and gives us
     *    a log in QEMU / CI even without a display. */
    serial_init();
    serial_write("WindClone serial console online (COM1)\n");
    log_init();   /* everything notable from here also lands in the ring */

    /* 2. Verify the bootloader understood the API revision we use. */
    if (!LIMINE_BASE_REVISION_SUPPORTED)
        panic("bootloader does not support Limine base revision 3");
    serial_write("step: base revision ok\n");

    if (hhdm_request.response == NULL)
        panic("bootloader did not provide the higher-half direct map");
    serial_write("step: hhdm ok\n");

    struct limine_memmap_response *memmap =
        (struct limine_memmap_response *)memmap_request.response;

    /* 3. Phase 2 — memory: frame allocator first (everything else
     *    needs frames), then OUR page tables, which finally make MMIO
     *    reachable: the VGA text buffer is mapped and kprint wakes up. */
    pmm_init(memmap, hhdm_request.response->offset,
             (uint64_t)(uintptr_t)__kernel_start,
             (uint64_t)(uintptr_t)__kernel_end);
    serial_write("step: pmm ok\n");

    paging_init();
    paging_is_live = 1;
    serial_write("step: paging ok\n");

    /* 0xB8000 is now mapped by our own P4 — vga.c has waited for this
     * since Phase 0 (Limine's HHDM covers RAM only, never MMIO). */
    vga_init((void *)(uintptr_t)0xB8000);
    kprint("WindClone: kernel owns its page tables now.\n");

    kmalloc_init();
    heap_selftest();
    serial_write("step: heap ok\n");

    /* 4. Console: serial is the ground truth, VGA the friendly face. */
    serial_write("step: console ready\n");

    /* 5. Report what the firmware and memory map told us. */
    const char *fw = "unknown";
    if (firmware_type_request.response != NULL) {
        switch (firmware_type_request.response->firmware_type) {
        case LIMINE_FIRMWARE_TYPE_X86BIOS: fw = "x86 BIOS"; break;
        case LIMINE_FIRMWARE_TYPE_UEFI32:  fw = "UEFI 32";  break;
        case LIMINE_FIRMWARE_TYPE_UEFI64:  fw = "UEFI 64";  break;
        }
    }

    uint64_t ram = usable_ram_bytes(memmap);
    uint64_t ram_mb = ram / (1024 * 1024);
    serial_write("step: memmap ok\n");

    serial_write("firmware: ");
    serial_write(fw);
    serial_write("\nRAM (usable): ");
    serial_write_dec(ram_mb);
    serial_write(" MiB\n");

    kprint("firmware: ");
    kprint(fw);
    kprint("\nusable RAM: ");
    kprint_dec(ram_mb);
    kprint(" MiB\n");
    klog("boot: firmware %s, usable RAM %u MiB", fw, (unsigned)ram_mb);

    /* 6. CPU plumbing: audit the GDT Limine gave us, install our own
     *    IDT (exceptions -> readable serial dump, IRQs -> handlers),
     *    remap the PIC and start the PIT timer at 100 Hz. */
    gdt_report();
    serial_write("step: gdt audited\n");

    idt_init();
    serial_write("step: idt loaded, pic remapped\n");

    timer_init(100);

    /* 7. Phase 3 - input & devices. PCI first: pure port I/O, no
     *    interrupts needed. Then the 8042 keyboard init arms IRQ1, and
     *    the single sti below unleashes PIT *and* PS/2 together. */
    pci_init();
    keyboard_init();

    sti();
    serial_write("step: interrupts armed (PIT 100Hz + PS/2 keyboard)\n");

    /* 8. Phase 4 - initrd. Limine dropped a tar archive into RAM beside
     *    us; we parse it where it lies and read /etc/motd back out. This
     *    is the first data in the system that is *not* part of the kernel
     *    image - the seam the ELF loader plugs into next. */
    if (initrd_init() == 0) {
        serial_write("initrd: ");
        serial_write(initrd_module_path());
        serial_write(" (");
        serial_write_dec(initrd_module_size());
        serial_write(" bytes), ");
        serial_write_dec(initrd_count());
        serial_write(" files\n");

        for (unsigned i = 0; i < initrd_count(); i++) {
            const struct initrd_file *f = initrd_at(i);
            serial_write("initrd: /");
            serial_write(f->name);
            serial_write(" (");
            serial_write_dec(f->size);
            serial_write(" bytes)\n");
        }

        const struct initrd_file *motd = initrd_find("/etc/motd");
        if (motd != NULL) {
            serial_write("initrd: /etc/motd, straight out of the archive:\n");
            serial_write("---8<---\n");
            serial_write_len((const char *)motd->data, (size_t)motd->size);
            serial_write("---8<---\n");
        }

        /* Negative lookup: a miss must be a NULL, never a wild pointer. */
        kassert(initrd_find("/no/such/file") == NULL);
        serial_write("initrd: missing path lookup returns NULL (ok)\n");

        kprint("initrd: ");
        kprint_dec(initrd_count());
        kprint(" files read from the tar module.\n");
        klog("initrd: %u files from %s", initrd_count(), initrd_module_path());
        serial_write("step: initrd ok\n");
    } else {
        serial_write("initrd: no tar module found - ISO built without "
                     "build/initrd.tar?\n");
    }

    serial_write("boot complete - CPU is in long mode, paging active\n");
    kprint("\nboot complete: interrupts + timer + heap + pci + kbd online.\n");

    /* 9. Idle loop: hlt lets the CPU sleep until the next interrupt;
     *    every 100 ticks (1 second) we print a heartbeat so QEMU/CI
     *    can prove the timer interrupt really fires. Every 5 seconds
     *    we also replay the tail of the log ring — proof Phase 3's
     *    buffered boot messages are readable long after they fired. */
    uint64_t last_second = 0;
    int dump_at_second = 5;
    for (;;) {
        hlt();
        uint64_t ticks = timer_ticks();
        if (ticks >= last_second + 100) {
            last_second = ticks;
            serial_write("uptime: ");
            serial_write_dec(last_second / 100);
            serial_write("s (ticks ");
            serial_write_dec(last_second);
            serial_write(")\n");
            if ((int)(last_second / 100) >= dump_at_second) {
                serial_write("log ring (most recent): ");
                log_dump_tail(4);
                serial_write("\n");
                dump_at_second += 5;
            }
        }
    }
}
