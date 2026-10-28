/* WindClone — linear framebuffer graphics (Phase 8).
 *
 * Limine hands us the video mode it already programmed: a linear buffer of
 * 32-bit pixels with a pitch we must respect (it is not always width*4).
 * Everything here is "write a pixel" plus the few primitives a desktop
 * needs — rectangles, a vertical gradient, and text from a 5x7 bitmap font
 * compiled into the kernel (no font files, no freetype, no libc).
 *
 * The buffer is device memory: it is NOT in the higher-half direct map
 * (that covers RAM only), so gfx_init() maps it explicitly, uncached.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDCLONE_GFX_H
#define WINDCLONE_GFX_H

#include <stdint.h>

/* 0 = no framebuffer (text mode only); callers must check gfx_ready().
 * The HHDM offset turns Limine's virtual framebuffer address back into the
 * physical one we map into our own low window. */
int  gfx_init(uint64_t hhdm_offset);
int  gfx_ready(void);

int  gfx_width(void);
int  gfx_height(void);

uint32_t gfx_rgb(uint8_t r, uint8_t g, uint8_t b);

void gfx_pixel(int x, int y, uint32_t rgb);
void gfx_fill(int x, int y, int w, int h, uint32_t rgb);

/* Vertical gradient: useful enough to be a primitive, and a 1024x768
 * wallpaper is 786 432 pixels — the inner loop is what makes it instant
 * instead of a noticeable pause at boot. */
void gfx_gradient_v(int x, int y, int w, int h, uint32_t top, uint32_t bottom);

/* 1-pixel frame, cheap window borders. */
void gfx_frame(int x, int y, int w, int h, uint32_t rgb);

/* Alpha blend: mix `rgb` (0xRRGGBB) over whatever is already in the composition
 * at `alpha` (0..255). That single line of maths is all an acrylic panel is: the
 * taskbar and the start menu keep the wallpaper underneath them and let part of
 * it through, instead of hiding it behind a flat colour. */
void gfx_blend_rect(int x, int y, int w, int h, uint32_t rgb, int alpha);

/* Filled rectangle with corners that give up `r` pixels of their own box. The
 * compositor paints back to front into the back buffer, so the pixels a rounded
 * rect declines to write still hold whatever was drawn there before: a rounded
 * window corner needs no mask and no knowledge of what is behind it.
 * gfx_round_top() rounds the two corners a title bar shows and leaves the
 * bottom square, because that bottom edge joins the body it sits on. */
void gfx_round_rect(int x, int y, int w, int h, int r, uint32_t rgb);
void gfx_round_top(int x, int y, int w, int h, int r, uint32_t rgb);

/* Copy a RAM surface (same 0xRRGGBB layout, `stride` pixels per row) into the
 * composition at (x,y), honouring the clip. This is how the wallpaper costs one
 * blit per frame instead of a gradient and a few hundred shape tests: it is
 * painted once into a cached surface and copied from there ever after. */
void gfx_blit_surface(const uint32_t *src, int x, int y, int w, int h, int stride);

/* 5x7 font, `scale` multiplies the glyph (2 = 10x14). Uppercase only:
 * the font is ASCII A-Z, 0-9 and the punctuation the UI actually uses. */
void gfx_text(int x, int y, const char *s, uint32_t rgb, int scale);
int  gfx_text_width(const char *s, int scale);

/* --- double buffer, clip, and damage ---------------------------------
 * Everything above draws into a RAM back buffer (once gfx_alloc_backbuffer()
 * has taken it) rather than the screen, so a frame is composed invisibly and
 * pushed to the display in a single flip. gfx_set_clip bounds what the next
 * draws touch, so recomposing a small damage region costs a small region. With
 * no back buffer (allocation failed, or before it is taken) the primitives
 * write the framebuffer directly and gfx_has_backbuffer() returns 0 - the
 * caller then repaints whole frames and never clips. */

/* Take the back buffer from the heap. Must be called after kmalloc_init().
 * 0 = back buffer in use, -1 = none (callers fall back to direct drawing). */
int  gfx_alloc_backbuffer(void);
int  gfx_has_backbuffer(void);

/* A standalone RAM surface of w*h pixels, laid out exactly like the back buffer
 * (row-major 0xRRGGBB, tightly packed), or NULL when the heap says no. Callers
 * paint into it once and blit from it often — that is the whole reason the
 * wallpaper can be elaborate and the desktop still be cheap to recompose. */
uint32_t *gfx_alloc_surface(int w, int h);

/* Point the primitives at a surface instead of the screen, and back at the
 * screen with NULL. The clip follows the target — drawing on a surface can never
 * write outside that surface — so painting a wallpaper into its own buffer and
 * blitting it per frame needs no new drawing code at all. Flip is unaffected:
 * it always pushes the back buffer, whatever the pen was last doing. */
void gfx_set_target(uint32_t *surf, int w, int h);

void gfx_set_clip(int x, int y, int w, int h);
void gfx_reset_clip(void);

/* Copy the back buffer to the screen, whole or by rectangle, and return how
 * many pixels that region covers (the work the frame did). */
uint64_t gfx_flip(void);
uint64_t gfx_flip_rect(int x, int y, int w, int h);

#endif
