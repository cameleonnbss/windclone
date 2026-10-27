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
#include "vfs.h"
#include "tmpfs.h"
#include "syscall.h"
#include "usermode.h"
#include "version.h"
#include "proc.h"
#include "gfx.h"
#include "desktop.h"
#include "mouse.h"
#include "shell.h"
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
     * since Phase 0 (Limine's HHDM covers RAM only, never MMIO).
     *
     * Phase 8: if the bootloader handed us a linear framebuffer, that is
     * what the screen is actually showing, so the text plane must NOT
     * re-program the CRTC into text mode behind our back. */
    int gfx_up = (gfx_init(hhdm_request.response->offset) == 0);
    vga_init((void *)(uintptr_t)0xB8000, gfx_up ? 0 : 1);
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
    klog("boot: windclone %s, firmware %s, usable RAM %u MiB",
         WC_VERSION, fw, (unsigned)ram_mb);

    /* 6. CPU plumbing: audit the GDT Limine gave us, install our own
     *    IDT (exceptions -> readable serial dump, IRQs -> handlers),
     *    remap the PIC and start the PIT timer at 100 Hz. */
    /* Phase 4 needs what Phase 1 only audited: our own GDT (ring-3
     * selectors) and a TSS (the kernel stack an interrupt uses when it
     * arrives from CPL 3). Both must exist before the IDT is built, since
     * every gate points at a code selector by number. */
    gdt_init();
    gdt_report();
    serial_write("step: gdt audited\n");

    idt_init();
    serial_write("step: idt loaded, pic remapped\n");

    /* The one gate ring 3 is allowed to call. */
    syscall_init();

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

        /* The seam Phase 4 was built for: from here on, anything that wants
         * the archive's bytes opens a path instead of holding a pointer into
         * the module. That is what lets the ELF loader, the shell and ring 3
         * read files through one door - and it is why `motd` above, taken
         * straight out of the tar, is now only useful as a second opinion. */
        vfs_init();

        int64_t fd = vfs_open("/etc/motd");
        kassert(fd >= 3);
        int64_t motd_size = vfs_size((int)fd);
        kassert(motd_size == (int64_t)motd->size);

        char probe[3];
        int64_t got = vfs_read((int)fd, probe, sizeof probe);
        kassert(got == 3 && probe[0] == (char)motd->data[0]);
        /* Reading on must continue where the last read stopped, and the way to
         * prove it is to compare the next byte against the archive's own byte
         * three - a descriptor that restarted would read the first one again. */
        if (motd_size > 3) {
            kassert(vfs_read((int)fd, probe, 1) == 1);
            kassert(probe[0] == (char)motd->data[3]);
        }
        kassert(vfs_close((int)fd) == 0);
        /* A closed slot answers EBADF rather than reading the next file. */
        kassert(vfs_size((int)fd) == WC_EBADF);
        kassert(vfs_open("/no/such/file") == WC_ENOENT);
        kassert(vfs_open("/etc/motd") == fd);  /* the slot came back for reuse */
        kassert(vfs_open_count() == 1);
        kassert(vfs_close((int)fd) == 0);
        kassert(vfs_open_count() == 0);
        serial_write("vfs: read-back, offset, ebadf and enoent checks passed\n");

        /* The two calls Phase 5's shell is built on: a yes/no path lookup,
         * and one directory entry per index until the archive says ENOENT.
         * The count is checked against initrd_count() because that is how
         * many files the archive was parsed into: an enumeration that stopped
         * early would show a /bin with missing programs, and one that never
         * ended would hang the shell inside ring 3. */
        kassert(vfs_exists("/etc/motd") == 1);
        kassert(vfs_exists("/no/such/file") == 0);
        struct wc_dirent dent;
        unsigned listed = 0;
        while (vfs_dirent(listed, &dent) == 0) {
            kassert(dent.namelen > 0 && dent.namelen < WC_NAME_MAX);
            kassert(dent.name[dent.namelen] == '\0');   /* safe to print */
            listed++;
        }
        kassert(listed == initrd_count());
        kassert(vfs_dirent(0, NULL) == WC_EINVAL);
        serial_write("vfs: exists + dirent enumeration matches the archive\n");

        /* v0.1.18: the writable half. Everything above is what a boot archive
         * can honestly answer; this is what a filesystem has to answer on top of
         * it, and every claim here is checked against a number the kernel
         * computed rather than a number written down in the test.
         *
         * It runs at boot, before a single program exists, for the same reason
         * the archive checks do: a file layer that only works when a C program
         * is polite about it is a file layer a program can break. */
        static const char note[] = "windclone writes files now\n";
        const uint64_t note_len = sizeof note - 1;

        int64_t tfd = vfs_open_flags("/tmp/selftest.txt",
                                     WC_O_WRONLY | WC_O_CREAT | WC_O_TRUNC);
        kassert(tfd >= 3);
        kassert(vfs_write((int)tfd, note, note_len) == (int64_t)note_len);
        /* A second write on the same descriptor lands after the first, because
         * the position lives in the descriptor and moving it is what a save
         * does: two chunks, one file, no overwrite of the header. */
        kassert(vfs_write((int)tfd, "twice\n", 6) == 6);
        kassert(vfs_size((int)tfd) == (int64_t)(note_len + 6));
        kassert(vfs_close((int)tfd) == 0);

        /* Read it back the way a program would: a plain read-only open of the
         * same path, with nothing but the name to go on. */
        int64_t rfd = vfs_open("/tmp/selftest.txt");
        kassert(rfd >= 3);
        kassert(vfs_size((int)rfd) == (int64_t)(note_len + 6));
        char rb[64];
        int64_t note_got = vfs_read((int)rfd, rb, sizeof rb);
        kassert(note_got == (int64_t)(note_len + 6));
        kassert(memcmp(rb, note, (size_t)note_len) == 0);
        kassert(memcmp(rb + note_len, "twice\n", 6) == 0);
        /* A descriptor that was opened read-only refuses writes, whatever store
         * is behind it — and refuses them with EBADF, the answer Linux gives, so
         * a program written against either behaves the same way. */
        kassert(vfs_write((int)rfd, "x", 1) == WC_EBADF);

        /* O_EXCL next to O_CREAT is how a program says "do not quietly clobber
         * what is already there", and the archive is read-only whatever the
         * caller asked for. */
        kassert(vfs_open_flags("/tmp/selftest.txt",
                               WC_O_WRONLY | WC_O_CREAT | WC_O_EXCL) == WC_EEXIST);
        kassert(vfs_open_flags("/bin/hello", WC_O_WRONLY) == WC_EPERM);
        kassert(vfs_open("/tmp/no-such-note.txt") == WC_ENOENT);

        /* One listing, two stores. The writable files come first, so the count
         * is the archive's plus the one file this check just made, and entry 0
         * is a name that exists nowhere in the tar. */
        unsigned merged = 0;
        while (vfs_dirent(merged, &dent) == 0)
            merged++;
        kassert(merged == initrd_count() + 1);
        kassert(vfs_dirent(0, &dent) == 0);
        kassert(strcmp(dent.name, "tmp/selftest.txt") == 0);
        kassert(vfs_exists("/tmp/selftest.txt") == 1);
        serial_write("vfs: merged enumeration lists the store and the archive\n");

        /* The stale-descriptor check, and the reason tmpfs hands out a
         * generation as well as a slot: this descriptor is open on a file that
         * is about to stop existing, and a *different* file is about to be
         * created in the very same pool slot. Reading through rfd after that
         * must not return the new file's bytes - an fd that outlives its file
         * and starts reporting somebody else's is not a bug in a corner case,
         * it is a data leak through a number a program still thinks is valid. */
        kassert(tmpfs_remove("tmp/selftest.txt") == 0);
        kassert(vfs_read((int)rfd, rb, sizeof rb) == WC_EBADF);
        kassert(vfs_size((int)rfd) == WC_EBADF);
        kassert(vfs_open("/tmp/selftest.txt") == WC_ENOENT);
        kassert(tmpfs_create("tmp/recycled.txt") >= 0);
        kassert(vfs_read((int)rfd, rb, sizeof rb) == WC_EBADF);
        /* Closing still works: a descriptor has to be releasable even when the
         * file behind it is gone, or the slot leaks. */
        kassert(vfs_close((int)rfd) == 0);
        kassert(tmpfs_remove("tmp/recycled.txt") == 0);
        kassert(tmpfs_count() == 0);

        /* Back to the archive's own number, which is the claim the release makes
         * about its own footprint: the self-check leaves the store empty, so no
         * listing, menu row or program's behaviour changes because this ran. */
        unsigned after = 0;
        while (vfs_dirent(after, &dent) == 0)
            after++;
        kassert(after == initrd_count());
        serial_write("tmpfs: write, read-back, eexist, eperm and stale-fd checks passed\n");

        klog("vfs: %u files behind %u descriptor slots, plus %u tmpfs slots",
             initrd_count(), 8u, (unsigned)TMPFS_MAX_FILES);
        serial_write("step: vfs ok\n");
    } else {
        serial_write("initrd: no tar module found - ISO built without "
                     "build/initrd.tar?\n");
    }

    serial_write("boot complete - CPU is in long mode, paging active\n");
    kprint("\nboot complete: interrupts + timer + heap + pci + kbd online.\n");

    /* Phase 8 - the screen. Painted once, right before we idle: the desktop
     * is a static frame until there is a mouse to move something. */
    if (gfx_up) {
        /* The pointer starts centred, so the first frame already has a
         * cursor in it even before a single packet arrives. */
        mouse_init();
        shell_init();
        /* Compose into a RAM back buffer, then flip it to the screen, so a
         * frame lands whole and only the region that changed is redrawn. The
         * heap is already up (kmalloc_init ran earlier in boot); if the buffer
         * cannot be had, gfx draws straight to the framebuffer and desktop.c
         * falls back to whole-screen repaints - it never clips without one. */
        gfx_alloc_backbuffer();
        desktop_frame();
        serial_write("gfx: desktop composited ");
        serial_write_dec((uint64_t)gfx_width());
        serial_write("x");
        serial_write_dec((uint64_t)gfx_height());
        serial_write(" (wallpaper, taskbar, one window)\n");
        klog("gfx: desktop up");
    } else {
        serial_write("gfx: no framebuffer - text console only\n");
    }

    /* 9. Idle loop: hlt lets the CPU sleep until the next interrupt;
     *    every 100 ticks (1 second) we print a heartbeat so QEMU/CI
     *    can prove the timer interrupt really fires. Every 5 seconds
     *    we also replay the tail of the log ring — proof Phase 3's
     *    buffered boot messages are readable long after they fired. */
    uint64_t last_second = 0;
    int dump_at_second = 5;
    int user_ran = 0;
    for (;;) {
        hlt();

        /* Input-driven redraw: the drivers only queue state, the idle loop
         * decides when to repaint. It has to happen on every wake-up, not
         * once a second — a cursor that updates at 1 Hz is worse than no
         * cursor at all. A live process's output sets the shell's own dirty
         * flag, so it joins the reasons to paint: the terminal a program is
         * streaming into has to turn over between visits, not only when the
         * next key happens to arrive. */

        /* Finalise any program window whose process just stopped *before* the
         * repaint decision, so a program that finishes quietly still gets its
         * "finished code" line and its EXIT row painted even when nothing else
         * set a dirty flag. It runs from the idle loop, not desktop_frame(), for
         * exactly that reason: the frame is only drawn when redraw is already
         * true, and the death of a process is one of the things that must make
         * it true. */
        shell_program_pump();

        int redraw = mouse_take_dirty() || shell_dirty()
                                     || shell_program_dirty_any();
        int ch;
        while ((ch = keyboard_take_char()) >= 0) {
            shell_key(ch);
            redraw = 1;
        }
        if (gfx_up && redraw) {
            desktop_frame();
            shell_clear_dirty();
            shell_program_clear_dirty_all();
            /* The witness that the compositor actually turned over. Written
             * straight to the serial line rather than the log ring so it is
             * ordered against a live process's own output: when a [hold] round
             * and this line interleave in the log, the loop painted *while* the
             * process was still running — the screen no longer freezes behind it. */
            serial_write("wm: frame painted\n");
        }

        /* A program the shell launched is live, not an excursion. Give the
         * runnable set one bounded slice here and come back, so the next
         * iteration repaints and polls input before the process advances again.
         * proc_live_count() is the cheap guard: while only the boot demos or the
         * idle shell exist it is zero and this does nothing at all. The boot
         * demo block below still runs its own synchronous proc_run_all() to
         * completion — it finishes before a visit ever has something to run. */
        if (proc_live_count() > 0)
            proc_sched_visit();

        uint64_t ticks = timer_ticks();
        if (ticks >= last_second + 100) {
            last_second = ticks;
            serial_write("uptime: ");
            serial_write_dec(last_second / 100);
            serial_write("s (ticks ");
            serial_write_dec(last_second);
            serial_write(")\n");

            /* Run the user programs once the timer has proven the machine is
             * fully up — a fault in ring 3 borrows the same exception path the
             * boot used, so it is equally legible. Two of them, in order,
             * because a second program is the test that the loader really put
             * the first one's frames back: hello is a two-segment asm image,
             * motd is a gcc image with .rodata, .data and .bss that opens a
             * file through the vfs. argc 0 / argv NULL means "run it with just
             * its own name" — the boot path has no command line, and the shell
             * is what supplies a real one from the keyboard. */
            if (!user_ran) {
                user_ran = 1;
                static const char *programs[] = { "/bin/hello", "/bin/motd" };
                for (unsigned p = 0; p < sizeof programs / sizeof programs[0]; p++) {
                    int code = -1;
                    serial_write("user: starting ");
                    serial_write(programs[p]);
                    serial_write(" from the initrd\n");
                    if (usermode_run(programs[p], &code, 0, NULL) == 0) {
                        serial_write("user: program returned, exit code ");
                        serial_write_dec((uint64_t)(code < 0 ? 0 : (unsigned)code));
                        serial_write("\n");
                        klog("user: %s exited with %d", programs[p], code);
                    } else {
                        serial_write("user: could not start ");
                        serial_write(programs[p]);
                        serial_write("\n");
                    }
                }
                serial_write("step: userland ok (elf64 + ring 3 round trip)\n");

                /* Phase 5's real prize: two programs alive at the same time.
                 * The loop above can only ever run one at a time — a second
                 * ELF would overwrite the first at 0x400000 — so a scheduler
                 * needed per-process address spaces before it meant anything.
                 * Now proc_create() gives each /bin/tick its own page tables,
                 * and proc_run_all() interleaves them: their output comes out
                 * A-1 B-1 A-2 B-2, each resuming its own loop counter after
                 * the other has had the CPU. Cooperative round-robin — the
                 * timer can't preempt yet — but two contexts truly alternate,
                 * which is the thing a single-program runner cannot fake. */
                {
                    const char *argv_a[2] = { "tick", "A" };
                    const char *argv_b[2] = { "tick", "B" };
                    serial_write("sched: building two processes\n");
                    proc_create("/bin/tick", 2, argv_a, 0);
                    proc_create("/bin/tick", 2, argv_b, 0);
                    serial_write("sched: two processes live, interleaving now\n");
                    proc_run_all();
                    serial_write("step: scheduler ok (two processes, round-robin)\n");
                }

                /* Preemption: the cooperative test above only switches where a
                 * program asks. These two /bin/spin processes never yield —
                 * they burn CPU in a hot loop — so the only way their output
                 * can interleave is the timer interrupt forcing a switch out
                 * from under a running process. If preemption were broken, one
                 * would spin all the way to done before the other printed its
                 * first line, and CI's order check catches exactly that. */
                {
                    const char *argv_s1[2] = { "spin", "A" };
                    const char *argv_s2[2] = { "spin", "B" };
                    serial_write("sched: two compute-heavy procs, no yields\n");
                    proc_create("/bin/spin", 2, argv_s1, 0);
                    proc_create("/bin/spin", 2, argv_s2, 0);
                    proc_run_all();
                    serial_write("step: preemption ok (timer switched a busy process)\n");
                }

                /* The trilogy's last act: a parent that sleeps. /bin/waiter
                 * spawns two /bin/exit children with real argv (7 and 9, shuttled
                 * through kernel bounce buffers into a private address space),
                 * then calls wait() — the first syscall in this kernel that does
                 * not return into its caller. The parent goes BLOCKED, the CPU
                 * goes to the children, and each child's exit wakes the parent
                 * with that child's pid and code. The printed spawn/reap pairs
                 * are the proof; CI checks them order-free, because whether the
                 * parent blocks before or after the first child dies is tick
                 * luck, while the reap pairs themselves are not. */
                {
                    const char *argv_w[1] = { "waiter" };
                    serial_write("proc: one parent, two children, a blocking wait\n");
                    proc_create("/bin/waiter", 1, argv_w, 0);
                    proc_run_all();
                    serial_write("step: wait ok (spawn with argv + blocking wait + reaping)\n");
                }

                /* The fourth verb. wait() lets a parent react to a death; kill()
                 * lets it cause one. /bin/reaper spawns /bin/bell — a child that
                 * runs for seconds and never quits by itself — yields so the bell
                 * demonstrably lives, and kills it: the victim is off-CPU (READY
                 * or BLOCKED) by definition, so the kernel retires it where it
                 * stands, frees its address space, marks the zombie EXITED with
                 * code -SIGKILL and wakes the parent's wait(). The reaped pid and
                 * the -9 are the proof, and the missing "[bell] done" is the
                 * other half of it. /bin/vanish covers the case that cannot be
                 * done in place — killing yourself holds the CPU and the frame,
                 * so it routes through the exit arm and never returns; its
                 * second line must therefore never appear. */
                {
                    const char *argv_r[1] = { "reaper" };
                    const char *argv_v[1] = { "vanish" };
                    serial_write("proc: a parent that kills, and a process that ends itself\n");
                    proc_create("/bin/reaper", 1, argv_r, 0);
                    proc_create("/bin/vanish", 1, argv_v, 0);
                    proc_run_all();
                    serial_write("step: kill ok (SIGKILL ends a process, its parent reaps it)\n");
                }

                /* The direction the four verbs never covered. spawn, exit, wait
                 * and kill all describe what a parent does *to* a child; none of
                 * them says what happens to a child when the parent dies first.
                 * Until now the answer was: nothing good. /bin/orphan spawns
                 * /bin/nest and exits without waiting, so nest outlives the
                 * process that made it, and the pair (its pid, its exit code 7)
                 * used to sit in a slot whose parent pid pointed at somebody who
                 * had already been reaped — unwaitable, unkillable, freed in
                 * silence by the sweep at the end of the run.
                 *
                 * The scheduler does not stop for this: nest stays READY and runs
                 * to its own ending, which is why the run is a proof and not a
                 * hang. What changed is ownership. The shell's slot (pid 1, the
                 * only process here that outlives every program it started) is
                 * the adopter, so the kernel prints the re-parenting, nest reads
                 * the new parent back through getppid() from ring 3, and the code
                 * 7 arrives as "shell: reaped pid N code 7" — a sentence this
                 * kernel could not write before. tools/local-test.sh ties the four
                 * numbers together rather than trusting any one of them. */
                {
                    const char *argv_o[1] = { "orphan" };
                    serial_write("proc: a parent that dies before its child does\n");
                    proc_create("/bin/orphan", 1, argv_o, 0);
                    proc_run_all();
                    serial_write("step: adoption ok (an orphan is re-parented and its exit code comes back)\n");
                }
            }
            if ((int)(last_second / 100) >= dump_at_second) {
                serial_write("log ring (most recent): ");
                log_dump_tail(4);
                serial_write("\n");
                dump_at_second += 5;
            }
        }
    }
}
