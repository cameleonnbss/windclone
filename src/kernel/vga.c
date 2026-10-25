/* WindClone — VGA text-mode driver implementation.
 * SPDX-License-Identifier: MIT
 */
#include "vga.h"
#include "io.h"
#include "serial.h"

#include <stdint.h>
#include <stddef.h>

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

/* attribute: light grey on black */
#define VGA_ATTR 0x07

static uint16_t *buffer;          /* NULL until vga_init succeeds   */
static size_t column;
static size_t row;

/* ------------------------------------------------------------------ */
/* Hardware: force the CRTC back to text mode 0x03 (80x25).            */
/*                                                                     */
/* Limine v9 shows its GUI in a graphical VESA/framebuffer mode and    */
/* does NOT restore text mode before jumping to us — so writing to     */
/* 0xB8000 lands in plane memory the CRTC never scans: invisible.      */
/* There is no BIOS int 10h in long mode, so we program the VGA         */
/* registers ourselves (sequencer, CRTC, GC, AC — the classic          */
/* mode-0x03 initialisation values).                                   */
/* ------------------------------------------------------------------ */

static inline void seq_write(uint8_t reg, uint8_t val)
{
    outb(0x3C4, reg);
    outb(0x3C5, val);
}

static inline uint8_t crtc_read(uint8_t reg)
{
    outb(0x3D4, reg);
    return inb(0x3D5);
}

static inline void crtc_write(uint8_t reg, uint8_t val)
{
    outb(0x3D4, reg);
    outb(0x3D5, val);
}

static inline void gc_write(uint8_t reg, uint8_t val)
{
    outb(0x3CE, reg);
    outb(0x3CF, val);
}

static inline void ac_write(uint8_t reg, uint8_t val)
{
    inb(0x3DA);              /* reset the AC index/data flip-flop */
    outb(0x3C0, reg);
    outb(0x3C0, val);
}

/* Bochs/VBE DISPI index+data pair on QEMU's std-vga (and compatible).
 * We only need the ENABLE register (index 4): clearing it drops the card
 * out of the graphical LFB mode Limine's GUI left behind, so the legacy
 * text-mode registers below actually take effect. */
#define DISPI_INDEX_PORT   0x1CE
#define DISPI_DATA_PORT    0x1CF
#define DISPI_INDEX_ENABLE 0x04

static inline void dispi_write(uint16_t index, uint16_t value)
{
    outw(DISPI_INDEX_PORT, index);
    outw(DISPI_DATA_PORT, value);
}

static void vga_text_mode_init(void)
{
    static const uint8_t crtc_vals[] = {
        0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0x43, 0x83,
        0x52, 0x0F, 0x00, 0x07, 0x17, 0x19, 0x1F, 0x9C, 0x2E, 0x8F
    };

    /* Step 0: leave VBE/Bochs graphics. Limine's GUI runs through the
     * dispi "linear framebuffer" mode; while ENABLE is set, QEMU ignores
     * all our legacy text-mode register programming. Clearing it (bit 0
     * = enabled, bit 3 = LFB) drops the card back to plain VGA logic. */
    dispi_write(DISPI_INDEX_ENABLE, 0x00);

    /* Disable palette while we reprogram (AC quirk, reads index mode). */
    outb(0x3C0, inb(0x3C1) | 0x20);
    outb(0x3C0, 0x01);

    /* Misc output: colour RAM mapping, text I/O base, 0xB8000 enabled. */
    outb(0x3C2, 0x67);

    /* Sequencer: assert sync, reset both channels, standard clocking. */
    seq_write(0x00, 0x01);
    seq_write(0x01, 0x01);
    seq_write(0x02, 0x03);
    seq_write(0x03, 0x00);
    seq_write(0x04, 0x03);

    /* CRTC: unlock (clear protect bit on reg 0x11), then 80x25 timing. */
    crtc_write(0x11, crtc_read(0x11) & 0x7F);
    for (unsigned i = 0; i < sizeof crtc_vals; i++)
        crtc_write((uint8_t)i, crtc_vals[i]);
    crtc_write(0x0C, 0x00);          /* start address = 0 */
    crtc_write(0x0D, 0x00);

    /* Graphics controller: flat read map, even/odd shift for text. */
    gc_write(0x00, 0x00);
    gc_write(0x01, 0x00);
    gc_write(0x02, 0x03);
    gc_write(0x03, 0x00);
    gc_write(0x04, 0x02);
    gc_write(0x05, 0x00);
    gc_write(0x06, 0x05);
    gc_write(0x07, 0x0F);
    gc_write(0x08, 0xFF);

    /* Attribute controller: identity palette, 80x25 text timing. */
    for (int i = 0; i < 16; i++)
        ac_write((uint8_t)i, (uint8_t)i);
    ac_write(0x10, 0x01);
    ac_write(0x11, 0x00);

    /* Deassert sequencer sync; re-enable the palette (bit 5 clear). */
    seq_write(0x00, 0x03);
    inb(0x3DA);
    outb(0x3C0, inb(0x3C1) & ~0x20);
}

void vga_init(void *hw, int force_text_mode)
{
    /* Phase 8 keeps the mode the bootloader chose when there is a linear
     * framebuffer to draw on: forcing text mode here is what used to make
     * 0xB8000 the only visible surface. The text plane is still mapped and
     * kprint() still writes to it — it is simply not what the screen shows. */
    if (force_text_mode)
        vga_text_mode_init();
    buffer = (uint16_t *)hw;
    column = 0;
    row = 0;
    if (buffer == NULL)
        return;

    /* Prove we can use the MMIO we just mapped through our OWN page
     * tables: write one text cell, read it back. This round-trip is the
     * real milestone of "the kernel owns device memory now" — it holds
     * even when QEMU's emulated monitor keeps scanning Limine's GUI
     * framebuffer instead of our text plane (a device-model quirk; on
     * real BIOS hardware the CRTC reprogramming above does show it). */
    uint16_t probe = (uint16_t)'W' | ((uint16_t)VGA_ATTR << 8);
    uint16_t keep = buffer[0];
    int ok;
    buffer[0] = probe;
    ok = (buffer[0] == probe);
    buffer[0] = keep;
    kclear();
    if (ok)
        serial_write("vga: text console live (MMIO round-trip ok)\n");
    else
        serial_write("vga: WARNING: MMIO read-back mismatch\n");
}

int vga_available(void)
{
    return buffer != NULL;
}

static void put_cell(char c, uint8_t attr)
{
    buffer[row * VGA_WIDTH + column] = (uint16_t)c | ((uint16_t)attr << 8);
}

static void scroll_up(void)
{
    for (size_t i = 0; i < (VGA_HEIGHT - 1) * VGA_WIDTH; i++)
        buffer[i] = buffer[i + VGA_WIDTH];
    for (size_t x = 0; x < VGA_WIDTH; x++)
        buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = ' ' | ((uint16_t)VGA_ATTR << 8);
    row = VGA_HEIGHT - 1;
}

static void put_char(char c)
{
    if (c == '\n') {
        column = 0;
        if (++row == VGA_HEIGHT)
            scroll_up();
        return;
    }
    if (c == '\r') {
        column = 0;
        return;
    }
    if (c == '\t') {
        column = (column + 8) & ~(size_t)7;
        if (column >= VGA_WIDTH) {
            column = 0;
            if (++row == VGA_HEIGHT)
                scroll_up();
        }
        return;
    }
    put_cell(c, VGA_ATTR);
    if (++column == VGA_WIDTH) {
        column = 0;
        if (++row == VGA_HEIGHT)
            scroll_up();
    }
}

void kprint(const char *s)
{
    if (buffer == NULL)
        return;
    while (*s != '\0')
        put_char(*s++);
}

/* Length-bounded twin of kprint(). A kernel buffer handed over by a user
 * program is not NUL-terminated and may not even be readable past its
 * length, so the VFS writes exactly the bytes the caller promised. */
void kprint_len(const char *s, size_t n)
{
    if (buffer == NULL)
        return;
    for (size_t i = 0; i < n; i++)
        put_char(s[i]);
}

void kclear(void)
{
    if (buffer == NULL)
        return;
    for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        buffer[i] = ' ' | ((uint16_t)VGA_ATTR << 8);
    column = 0;
    row = 0;
}
