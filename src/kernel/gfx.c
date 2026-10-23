/* WindClone — linear framebuffer graphics (Phase 8).
 *
 * Three things make this less trivial than it looks:
 *
 *  1. The framebuffer is device memory, outside the higher-half direct map
 *     (that covers RAM only). We map it ourselves, uncached, in a fixed low
 *     window, so the *physical* address is what matters — Limine hands us a
 *     virtual one, which we translate back through the HHDM offset.
 *  2. `pitch` is bytes per scanline, not width*4. Getting that wrong gives
 *     a picture that looks like it was drawn on a rubber sheet.
 *  3. Every write goes through a volatile pointer: the compiler must not
 *     cache or reorder what is ultimately MMIO.
 *
 * The font is 5x7, uppercase, compiled in. A real typographic stack is not
 * what Phase 8 is about; readable text on a window is.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>
#include <limine.h>

#include "gfx.h"
#include "paging.h"
#include "serial.h"

/* Where we map the device buffer. Any low address we own will do; keeping
 * it away from the heap (0x4000000000) and userland (4-8 MiB) matters. */
#define FB_VIRT 0x10000000ULL

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
    .response = NULL,
};

static volatile uint32_t *fb;
static int      width, height;
static uint32_t stride_px;      /* pixels per scanline, not bytes */

int gfx_ready(void)
{
    return fb != NULL;
}

int gfx_width(void)  { return width; }
int gfx_height(void) { return height; }

uint32_t gfx_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

int gfx_init(uint64_t hhdm_offset)
{
    if (framebuffer_request.response == NULL ||
        framebuffer_request.response->framebuffer_count == 0) {
        serial_write("gfx: no framebuffer from the bootloader - staying in text mode\n");
        return -1;
    }

    struct limine_framebuffer *f = framebuffer_request.response->framebuffers[0];

    if (f->bpp != 32) {
        serial_write("gfx: unsupported bpp ");
        serial_write_dec(f->bpp);
        serial_write(" (only 32-bit direct colour is supported)\n");
        return -1;
    }

    uint64_t vaddr = (uint64_t)f->address;
    uint64_t paddr = (hhdm_offset != 0 && vaddr >= hhdm_offset) ? vaddr - hhdm_offset : vaddr;
    uint64_t bytes = (uint64_t)f->pitch * f->height;

    for (uint64_t off = 0; off < bytes; off += 4096)
        paging_map_page(FB_VIRT + off, paddr + off, PTE_WRITE | PTE_PCD);

    fb        = (volatile uint32_t *)(uintptr_t)FB_VIRT;
    width     = (int)f->width;
    height    = (int)f->height;
    stride_px = (uint32_t)(f->pitch / 4);

    serial_write("gfx: framebuffer ");
    serial_write_dec((uint64_t)width);
    serial_write("x");
    serial_write_dec((uint64_t)height);
    serial_write(" bpp=32 pitch=");
    serial_write_dec(f->pitch);
    serial_write(" physical=");
    serial_write_hex(paddr);
    serial_write("\n");
    return 0;
}

void gfx_pixel(int x, int y, uint32_t rgb)
{
    if (fb == NULL || x < 0 || y < 0 || x >= width || y >= height)
        return;
    fb[(uint32_t)y * stride_px + (uint32_t)x] = rgb;
}

void gfx_fill(int x, int y, int w, int h, uint32_t rgb)
{
    if (fb == NULL)
        return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > width ? width : x + w;
    int y1 = y + h > height ? height : y + h;

    for (int row = y0; row < y1; row++) {
        volatile uint32_t *line = fb + (uint32_t)row * stride_px;
        for (int col = x0; col < x1; col++)
            line[col] = rgb;
    }
}

void gfx_gradient_v(int x, int y, int w, int h, uint32_t top, uint32_t bottom)
{
    if (fb == NULL || h <= 0)
        return;

    int r0 = (int)((top >> 16) & 0xFF), g0 = (int)((top >> 8) & 0xFF), b0 = (int)(top & 0xFF);
    int r1 = (int)((bottom >> 16) & 0xFF), g1 = (int)((bottom >> 8) & 0xFF), b1 = (int)(bottom & 0xFF);

    for (int row = 0; row < h; row++) {
        int r = r0 + (r1 - r0) * row / h;
        int g = g0 + (g1 - g0) * row / h;
        int b = b0 + (b1 - b0) * row / h;
        gfx_fill(x, y + row, w, 1, gfx_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b));
    }
}

void gfx_frame(int x, int y, int w, int h, uint32_t rgb)
{
    gfx_fill(x, y, w, 1, rgb);
    gfx_fill(x, y + h - 1, w, 1, rgb);
    gfx_fill(x, y, 1, h, rgb);
    gfx_fill(x + w - 1, y, 1, h, rgb);
}

/* --- 5x7 font ---------------------------------------------------------- */

struct glyph {
    char    c;
    uint8_t rows[7];   /* bit 4 = leftmost column */
};

static const struct glyph font[] = {
    {'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'B', {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C', {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
    {'D', {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
    {'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    {'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'G', {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}},
    {'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'I', {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}},
    {'J', {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}},
    {'K', {0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
    {'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
    {'N', {0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
    {'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'Q', {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
    {'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S', {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
    {'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'V', {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
    {'W', {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}},
    {'X', {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
    {'Y', {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
    {'Z', {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
    {'0', {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    {'1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2', {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
    {'3', {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},
    {'4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    {'5', {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6', {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
    {'7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    {'9', {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    {' ', {0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
    {':', {0x00,0x04,0x04,0x00,0x04,0x04,0x00}},
    {'-', {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
    {'.', {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
    {'!', {0x04,0x04,0x04,0x04,0x04,0x00,0x04}},
};

static const uint8_t *glyph_for(char c)
{
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');       /* uppercase-only font */

    for (size_t i = 0; i < sizeof font / sizeof font[0]; i++)
        if (font[i].c == c)
            return font[i].rows;

    return font[36].rows;                /* unknown -> space */
}

void gfx_text(int x, int y, const char *s, uint32_t rgb, int scale)
{
    if (scale < 1)
        scale = 1;

    int cx = x;
    for (; *s != '\0'; s++) {
        const uint8_t *rows = glyph_for(*s);
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (rows[row] & (0x10 >> col))
                    gfx_fill(cx + col * scale, y + row * scale, scale, scale, rgb);
            }
        }
        cx += 6 * scale;                 /* 5 columns + 1 of spacing */
    }
}

int gfx_text_width(const char *s, int scale)
{
    if (scale < 1)
        scale = 1;
    int n = 0;
    for (; *s != '\0'; s++)
        n++;
    return n == 0 ? 0 : (n * 6 - 1) * scale;
}
