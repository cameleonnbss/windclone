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
#include "kmalloc.h"
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

/* The back buffer and the current clip box. Every primitive draws into `bb`
 * (a plain RAM framebuffer) instead of straight into the MMIO `fb`, so a frame
 * is composed invisibly and pushed to the display in one gfx_flip() blit - the
 * visible screen is never caught half-drawn. `bb == NULL` (no heap, or the
 * allocation failed) drops every primitive back to writing `fb` directly, so a
 * machine that cannot give us 3 MB still boots and still paints. The clip box
 * bounds what a recompose touches, which is what makes a small damage region
 * cost a small amount of work instead of the whole 1024x768. */
static uint32_t *bb;
static int cl_x0, cl_y0, cl_x1, cl_y1;   /* current clip, in pixels */

/* Where the primitives actually write. Normally the back buffer (`tgt = bb`,
 * `tgt_stride = stride_px`), but gfx_set_target() can point them at a plain RAM
 * surface of its own size instead — that is how the wallpaper gets painted once
 * into a cached surface and copied from it ever after, instead of being
 * recomputed by the compositor on every frame. With no back buffer at all
 * `tgt` is NULL and the writes go straight to the framebuffer, as they always
 * did. `bb` itself stays the flip source: pointing the pen elsewhere never
 * changes what gets pushed to the screen. */
static uint32_t *tgt;
static int tgt_stride, tgt_w, tgt_h;

void gfx_set_target(uint32_t *surf, int w, int h)
{
    if (surf != NULL) {
        tgt = surf; tgt_stride = w; tgt_w = w; tgt_h = h;
        gfx_set_clip(0, 0, w, h);      /* the surface is its own whole world */
        return;
    }
    tgt = bb;
    tgt_stride = (int)stride_px;
    tgt_w = width;
    tgt_h = height;
    gfx_reset_clip();                  /* back to the screen, clip and all */
}

int gfx_has_backbuffer(void)
{
    return bb != NULL;
}

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
    bb        = NULL;                 /* direct-to-fb until a back buffer is got */
    tgt       = NULL;                 /* same: the pen points at the framebuffer */
    tgt_stride = (int)stride_px;
    tgt_w     = width;
    tgt_h     = height;
    cl_x0 = cl_y0 = 0;                /* clip starts as the whole screen         */
    cl_x1 = width;
    cl_y1 = height;

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
    if (fb == NULL || x < cl_x0 || y < cl_y0 || x >= cl_x1 || y >= cl_y1)
        return;
    if (tgt != NULL)
        tgt[(uint32_t)y * (uint32_t)tgt_stride + (uint32_t)x] = rgb;
    else
        fb[(uint32_t)y * stride_px + (uint32_t)x] = rgb;
}

void gfx_fill(int x, int y, int w, int h, uint32_t rgb)
{
    if (fb == NULL)
        return;
    /* Clip first, then write: this one bounds check is what every other
     * primitive (frame, text, glyph pixels, the gradient's per-row fill)
     * inherits, so a recompose of a small damage box draws only that box. */
    int x0 = x < cl_x0 ? cl_x0 : x;
    int y0 = y < cl_y0 ? cl_y0 : y;
    int x1 = x + w > cl_x1 ? cl_x1 : x + w;
    int y1 = y + h > cl_y1 ? cl_y1 : y + h;

    for (int row = y0; row < y1; row++) {
        if (tgt != NULL) {
            uint32_t *line = tgt + (uint32_t)row * (uint32_t)tgt_stride;
            for (int col = x0; col < x1; col++)
                line[col] = rgb;
        } else {
            volatile uint32_t *line = fb + (uint32_t)row * stride_px;
            for (int col = x0; col < x1; col++)
                line[col] = rgb;
        }
    }
}

void gfx_gradient_v(int x, int y, int w, int h, uint32_t top, uint32_t bottom)
{
    if (fb == NULL || h <= 0)
        return;

    int r0 = (int)((top >> 16) & 0xFF), g0 = (int)((top >> 8) & 0xFF), b0 = (int)(top & 0xFF);
    int r1 = (int)((bottom >> 16) & 0xFF), g1 = (int)((bottom >> 8) & 0xFF), b1 = (int)(bottom & 0xFF);

    for (int row = 0; row < h; row++) {
        int sy = y + row;
        if (sy < cl_y0 || sy >= cl_y1)   /* the wallpaper's only real cost is
                                          * the rows it touches: skip the rest */
            continue;
        int r = r0 + (r1 - r0) * row / h;
        int g = g0 + (g1 - g0) * row / h;
        int b = b0 + (b1 - b0) * row / h;
        gfx_fill(x, sy, w, 1, gfx_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b));
    }
}

void gfx_frame(int x, int y, int w, int h, uint32_t rgb)
{
    gfx_fill(x, y, w, 1, rgb);
    gfx_fill(x, y + h - 1, w, 1, rgb);
    gfx_fill(x, y, 1, h, rgb);
    gfx_fill(x + w - 1, y, 1, h, rgb);
}

/* What the composition currently holds at this pixel. Reading is allowed
 * because the back buffer is plain RAM: a blend that works on the framebuffer
 * directly would be reading uncached MMIO, which is slow but still correct, so
 * the no-back-buffer path degrades instead of breaking. */
static uint32_t gfx_at(int x, int y)
{
    if (tgt != NULL)
        return tgt[(uint32_t)y * (uint32_t)tgt_stride + (uint32_t)x];
    return fb[(uint32_t)y * stride_px + (uint32_t)x];
}

void gfx_blend_rect(int x, int y, int w, int h, uint32_t rgb, int alpha)
{
    if (fb == NULL || w <= 0 || h <= 0)
        return;
    if (alpha <= 0)
        return;
    if (alpha >= 255) { gfx_fill(x, y, w, h, rgb); return; }

    int sr = (int)((rgb >> 16) & 0xFF), sg = (int)((rgb >> 8) & 0xFF), sb = (int)(rgb & 0xFF);

    int x0 = x < cl_x0 ? cl_x0 : x;
    int y0 = y < cl_y0 ? cl_y0 : y;
    int x1 = x + w > cl_x1 ? cl_x1 : x + w;
    int y1 = y + h > cl_y1 ? cl_y1 : y + h;

    for (int row = y0; row < y1; row++) {
        for (int col = x0; col < x1; col++) {
            uint32_t d = gfx_at(col, row);
            int dr = (int)((d >> 16) & 0xFF);
            int dg = (int)((d >> 8) & 0xFF);
            int db = (int)(d & 0xFF);
            /* Integer "over" compositing: dst*(1-a) + src*a, with the rounding
             * bias added so a 50% blend of two greys does not come out dark. */
            int r = (dr * (255 - alpha) + sr * alpha + 127) / 255;
            int g = (dg * (255 - alpha) + sg * alpha + 127) / 255;
            int b = (db * (255 - alpha) + sb * alpha + 127) / 255;
            gfx_pixel(col, row, gfx_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b));
        }
    }
}

/* Is the pixel `dx`,`dy` from the outer tip of a corner box of radius `r`
 * inside the quarter disc? The centre sits at (r-1, r-1) and the comparison is
 * made against r*r + r, which rounds the arc through the pixel centres rather
 * than clipping it short: the same rule for every corner, rotated by the
 * caller's own choice of dx/dy. */
static int in_corner(int dx, int dy, int r)
{
    int cx = r - 1 - dx, cy = r - 1 - dy;
    return cx * cx + cy * cy <= r * r + r;
}

/* How many pixels the first row of a rounded box gives up on each side. Beyond
 * the corner box nothing is given up at all, so `dy >= r` means a full-width row. */
static int corner_inset(int dy, int r)
{
    int inset = 0;
    while (inset < r && !in_corner(inset, dy, r))
        inset++;
    return inset;
}

/* `top` and `bottom` pick which corners are rounded: the title bar's pair, a
 * panel's four, or neither (which is then just a fill). The rows in the middle
 * of the box are always full width. */
static void round_box(int x, int y, int w, int h, int r, int top, int bottom,
                      uint32_t rgb)
{
    if (fb == NULL || w <= 0 || h <= 0)
        return;
    if (r <= 0) { gfx_fill(x, y, w, h, rgb); return; }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r <= 0) { gfx_fill(x, y, w, h, rgb); return; }

    for (int row = 0; row < h; row++) {
        int inset = 0;
        if (top && row < r)
            inset = corner_inset(row, r);
        else if (bottom && row >= h - r)
            inset = corner_inset(h - 1 - row, r);
        if (inset * 2 >= w)
            continue;                       /* nothing of this row survives */
        gfx_fill(x + inset, y + row, w - 2 * inset, 1, rgb);
    }
}

void gfx_round_rect(int x, int y, int w, int h, int r, uint32_t rgb)
{
    round_box(x, y, w, h, r, 1, 1, rgb);
}

void gfx_round_top(int x, int y, int w, int h, int r, uint32_t rgb)
{
    round_box(x, y, w, h, r, 1, 0, rgb);
}

void gfx_blit_surface(const uint32_t *src, int x, int y, int w, int h, int stride)
{
    if (fb == NULL || src == NULL || w <= 0 || h <= 0)
        return;

    int x0 = x < cl_x0 ? cl_x0 : x;
    int y0 = y < cl_y0 ? cl_y0 : y;
    int x1 = x + w > cl_x1 ? cl_x1 : x + w;
    int y1 = y + h > cl_y1 ? cl_y1 : y + h;

    for (int row = y0; row < y1; row++) {
        const uint32_t *s = src + (uint32_t)(row - y) * (uint32_t)stride;
        for (int col = x0; col < x1; col++)
            gfx_pixel(col, row, s[col - x]);
    }
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
    /* Appended after ' ' on purpose: glyph_for() falls back to font[36],
     * which must stay the space. Every character the UI writes must be in
     * this table — an unknown one silently becomes a blank, which is how
     * "/bin/hello" ended up printed as "  BIN  HELLO".
     * Punctuation the kernel actually prints: , / + ' ( )
     */
    {',', {0x00,0x00,0x00,0x00,0x0C,0x04,0x08}},
    {'/', {0x01,0x02,0x02,0x04,0x08,0x08,0x10}},
    {'+', {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}},
    {'\'', {0x04,0x04,0x08,0x00,0x00,0x00,0x00}},
    {'(', {0x02,0x04,0x08,0x08,0x08,0x04,0x02}},
    {')', {0x08,0x04,0x02,0x02,0x02,0x04,0x08}},
    /* The accents French words actually carry. The cell is seven rows and a
     * capital fills all seven, so an accented letter spends its top two rows on
     * the mark and squeezes the body into five: readable at scale 1 and 2, which
     * is the only scale the chrome draws at. Keyed by the uppercase Latin-1 code
     * point — decode_utf8() hands that back for the two bytes the source file
     * holds, and fold_accent() brings a lowercase one to the same row. Without
     * these the interface says "SYSTEME ECRIT": a desktop that announces, in the
     * middle of its own French labels, that it is not really French. */
    {(char)0xC9, {0x08,0x10,0x1F,0x10,0x1E,0x10,0x1F}},   /* É é */
    {(char)0xC8, {0x10,0x08,0x1F,0x10,0x1E,0x10,0x1F}},   /* È è */
    {(char)0xCA, {0x04,0x0A,0x1F,0x10,0x1E,0x10,0x1F}},   /* Ê ê */
    {(char)0xC0, {0x10,0x08,0x0E,0x11,0x1F,0x11,0x11}},   /* À à */
    {(char)0xD9, {0x10,0x08,0x11,0x11,0x11,0x11,0x0E}},   /* Ù ù */
    {(char)0xC7, {0x00,0x0E,0x11,0x10,0x11,0x0E,0x06}},   /* Ç ç */
};

/* Latin-1 puts an accented capital exactly 0x20 below its lowercase, the same
 * offset ASCII uses for a-z; '÷' and 'ð' fold too and simply come out blank, as
 * they already did. One rule, and every accent the table holds can be typed in
 * either case. */
static char fold_accent(char c)
{
    unsigned char u = (unsigned char)c;
    if (u >= 0xE0 && u <= 0xFC)
        u = (unsigned char)(u - 0x20);
    return (char)u;
}

static const uint8_t *glyph_for(char c)
{
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');       /* uppercase-only font */
    else
        c = fold_accent(c);

    for (size_t i = 0; i < sizeof font / sizeof font[0]; i++)
        if (font[i].c == c)
            return font[i].rows;

    return font[36].rows;                /* unknown -> space */
}

/* The source files are UTF-8, so an accented letter reaches the kernel as two
 * bytes: a 110xxxxx lead followed by a 10xxxxxx continuation. Decode the pair
 * back to the code point the table above is keyed by and step over both; any
 * other byte is passed through as itself, which is what it always was. Three-
 * byte sequences are not passed through as one glyph — they land as the blanks
 * they used to land as, and nothing in this interface needs them.
 *
 * The point is that a label in this repository can be spelled the way French
 * spells it. The alternative was writing "SYST\xC8ME" in the source and calling
 * it readable. */
static const char *decode_utf8(const char *s, char *cp)
{
    unsigned char c = (unsigned char)*s;

    if ((c & 0xE0) == 0xC0 && s[1] != '\0' &&
        ((unsigned char)s[1] & 0xC0) == 0x80) {
        int v = ((c & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
        *cp = (char)((v >= 0x80 && v <= 0xFF) ? v : ' ');
        return s + 2;
    }
    *cp = (char)c;
    return s + 1;
}

void gfx_text(int x, int y, const char *s, uint32_t rgb, int scale)
{
    if (scale < 1)
        scale = 1;

    int cx = x;
    for (; *s != '\0'; ) {
        char cp;
        s = decode_utf8(s, &cp);
        const uint8_t *rows = glyph_for(cp);
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
    /* Count cells, not bytes: an accent is one glyph wide, and a label centred
     * on a byte count would sit a pixel or two off its icon. */
    for (; *s != '\0'; ) {
        char cp;
        s = decode_utf8(s, &cp);
        n++;
    }
    return n == 0 ? 0 : (n * 6 - 1) * scale;
}

/* --- back buffer, clip, and flip ------------------------------------- */

/* Take the RAM back buffer. It is stride_px*height so it matches the real
 * framebuffer row for row (including any pitch padding), which is what lets
 * gfx_flip_rect copy the same offsets straight across. Called from the kernel
 * once the heap is up, before the first composite; if it fails the primitives
 * fall back to writing the framebuffer directly and nothing blanks. */
int gfx_alloc_backbuffer(void)
{
    if (fb == NULL)
        return -1;

    uint64_t bytes = (uint64_t)stride_px * (uint64_t)height * 4;
    bb = (uint32_t *)kmalloc((size_t)bytes);
    if (bb == NULL) {
        serial_write("gfx: back buffer alloc failed, direct framebuffer\n");
        return -1;
    }
    serial_write("gfx: back buffer ");
    serial_write_dec(bytes);
    serial_write(" bytes\n");
    tgt = bb;                      /* the pen follows the buffer: primitives now
                                    * compose in RAM instead of on the screen */
    return 0;
}

/* A tightly packed w*h surface. Kept in gfx.c rather than the caller's file so
 * the layout it hands back is the layout gfx_blit_surface() reads: one rule for
 * what a pixel is, in one place. Returns NULL when the heap has no room, and a
 * caller with no surface paints the expensive way instead. */
uint32_t *gfx_alloc_surface(int w, int h)
{
    if (fb == NULL || w <= 0 || h <= 0)
        return NULL;
    return (uint32_t *)kmalloc((size_t)w * (size_t)h * 4);
}

void gfx_set_clip(int x, int y, int w, int h)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    /* Bounded by whatever is being drawn on now — the screen, or an offscreen
     * surface that may be smaller than it. A clip that could reach past the end
     * of the surface would be a heap overflow wearing the face of a rectangle. */
    int x1 = x + w > tgt_w ? tgt_w : x + w;
    int y1 = y + h > tgt_h ? tgt_h : y + h;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    cl_x0 = x0; cl_y0 = y0;
    cl_x1 = x1 < x0 ? x0 : x1;    /* never invert the clip */
    cl_y1 = y1 < y0 ? y0 : y1;
}

void gfx_reset_clip(void)
{
    cl_x0 = cl_y0 = 0;
    cl_x1 = tgt_w;
    cl_y1 = tgt_h;
}

/* Push one rectangle from the back buffer to the screen. Returns the pixels
 * in that (clamped) rectangle - the honest measure of how much a frame touched,
 * whether we copied them (bb present) or already wrote them straight to fb
 * (no back buffer, so the flip has nothing to do but the work still happened). */
uint64_t gfx_flip_rect(int x, int y, int w, int h)
{
    if (fb == NULL)
        return 0;

    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > width ? width : x + w;
    int y1 = y + h > height ? height : y + h;
    if (x1 <= x0 || y1 <= y0)
        return 0;

    if (bb != NULL) {
        for (int row = y0; row < y1; row++) {
            volatile uint32_t *d = fb + (uint32_t)row * stride_px;
            uint32_t *s = bb + (uint32_t)row * stride_px;
            for (int col = x0; col < x1; col++)
                d[col] = s[col];
        }
    }
    return (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0);
}

uint64_t gfx_flip(void)
{
    return gfx_flip_rect(0, 0, width, height);
}

