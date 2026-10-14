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

void panic(const char *why)
{
    serial_write("\n[WINDCLONE] KERNEL PANIC: ");
    serial_write(why);
    serial_write("\n");
    if (vga_available())
        kprint("KERNEL PANIC — system halted.");
    for (;;)
        __asm__ volatile("cli; hlt");
}

/* Total usable RAM, reported by the boot memory map. */
static uint64_t usable_ram_bytes(void)
{
    struct limine_memmap_response *m =
        (struct limine_memmap_response *)memmap_request.response;
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
/* kmain — the kernel entry point (ELF e_entry)                        */
/* ------------------------------------------------------------------ */

void kmain(void)
{
    /* 1. Serial first: it works on BIOS *and* UEFI, and gives us
     *    a log in QEMU / CI even without a display. */
    serial_init();
    serial_write("WindClone serial console online (COM1)\n");

    /* 2. Verify the bootloader understood the API revision we use. */
    if (!LIMINE_BASE_REVISION_SUPPORTED)
        panic("bootloader does not support Limine base revision 3");
    serial_write("step: base revision ok\n");

    if (hhdm_request.response == NULL)
        panic("bootloader did not provide the higher-half direct map");
    serial_write("step: hhdm ok\n");

    /* 3. Console: serial only for now.
     *    The VGA text buffer (0xB8000) is MMIO, and Limine's HHDM maps
     *    only RAM — touching it here triple-faults. vga.c activates
     *    itself as soon as the kernel maps it with its own page tables
     *    (docs/ROADMAP.md phase 2). */
    vga_init(NULL);
    serial_write("step: console ready\n");

    /* 4. Report what the firmware and memory map told us. */
    const char *fw = "unknown";
    if (firmware_type_request.response != NULL) {
        switch (firmware_type_request.response->firmware_type) {
        case LIMINE_FIRMWARE_TYPE_X86BIOS: fw = "x86 BIOS"; break;
        case LIMINE_FIRMWARE_TYPE_UEFI32:  fw = "UEFI 32";  break;
        case LIMINE_FIRMWARE_TYPE_UEFI64:  fw = "UEFI 64";  break;
        }
    }

    uint64_t ram = usable_ram_bytes();
    uint64_t ram_mb = ram / (1024 * 1024);
    serial_write("step: memmap ok\n");

    serial_write("firmware: ");
    serial_write(fw);
    serial_write("\nRAM (usable): ");
    serial_write_dec(ram_mb);
    serial_write(" MiB\n");

    serial_write("boot complete - CPU is in long mode, paging active\n");
    serial_write("next milestone: GDT/IDT, physical page allocator (see docs/ROADMAP.md)\n");

    /* 5. Park the CPU forever. */
    for (;;)
        __asm__ volatile("hlt");
}
