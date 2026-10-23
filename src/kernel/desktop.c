/* WindClone — the first thing that earns the *Wind* in the name (Phase 8).
 *
 * This is a compositor in the only sense that matters right now: it paints
 * a frame directly into the framebuffer, back to front — wallpaper, taskbar,
 * then a window on top. There is no damage tracking, no double buffer and
 * no cursor yet; those arrive with the window server, and they only make
 * sense once something can move.
 *
 * Everything is drawn from primitives in gfx.c so the layout below reads
 * like the description of a screen instead of pixel arithmetic.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "desktop.h"
#include "gfx.h"

/* One palette, so the UI cannot drift apart one call site at a time. */
#define C_WALL_TOP    gfx_rgb(0x0B, 0x2A, 0x63)
#define C_WALL_BOTTOM gfx_rgb(0x1F, 0x6B, 0xD8)
#define C_TASKBAR     gfx_rgb(0x0D, 0x11, 0x17)
#define C_ACCENT      gfx_rgb(0xEF, 0x44, 0x44)
#define C_TITLE       gfx_rgb(0x10, 0x18, 0x27)
#define C_BODY        gfx_rgb(0xF8, 0xFA, 0xFC)
#define C_INK         gfx_rgb(0x0F, 0x17, 0x2A)
#define C_MUTED       gfx_rgb(0x64, 0x74, 0x8B)
#define C_WHITE       gfx_rgb(0xFF, 0xFF, 0xFF)

#define TASKBAR_H 44

static void draw_close_button(int x, int y, int size)
{
    gfx_fill(x, y, size, size, C_ACCENT);
    for (int i = 0; i < size; i++) {
        gfx_pixel(x + i, y + i, C_WHITE);
        gfx_pixel(x + size - 1 - i, y + i, C_WHITE);
    }
}

/* Windows-ish mark: four panes, our accent colour instead of blue. */
static void draw_logo(int x, int y, int pane, int gap)
{
    gfx_fill(x, y, pane, pane, C_WHITE);
    gfx_fill(x + pane + gap, y, pane, pane, C_WHITE);
    gfx_fill(x, y + pane + gap, pane, pane, C_WHITE);
    gfx_fill(x + pane + gap, y + pane + gap, pane, pane, C_WHITE);
}

static void draw_window(int x, int y, int w, int h)
{
    /* Drop shadow first: two offset rectangles are enough to lift the
     * window off the wallpaper without an alpha channel. */
    gfx_fill(x + 6, y + 6, w, h, gfx_rgb(0x0A, 0x1E, 0x45));

    gfx_fill(x, y, w, h, C_BODY);
    gfx_fill(x, y, w, 34, C_TITLE);

    /* Accent stripe: the one bit of the palette that is recognisably ours. */
    gfx_fill(x, y, 4, 34, C_ACCENT);

    gfx_text(x + 16, y + 10, "WINDCLONE - BUREAU", C_WHITE, 2);

    draw_close_button(x + w - 34, y + 8, 18);

    int tx = x + 28;
    int ty = y + 62;

    gfx_text(tx, ty, "PREMIER BUREAU GRAPHIQUE", C_INK, 2);
    ty += 34;

    gfx_text(tx, ty, "CE TEXTE EST DESSINE PAR LE KERNEL:", C_MUTED, 1);
    ty += 14;
    gfx_text(tx, ty, "PAS DE LIBRAIRIE, PAS DE FICHIER DE POLICE,", C_INK, 1);
    ty += 12;
    gfx_text(tx, ty, "UN FRAMEBUFFER LINEAIRE ET 40 GLYPHES 5X7.", C_INK, 1);
    ty += 26;

    gfx_text(tx, ty, "- ALLOCATEUR DE PAGES: OK", C_INK, 1);
    ty += 12;
    gfx_text(tx, ty, "- GDT + TSS ET RING 3: OK", C_INK, 1);
    ty += 12;
    gfx_text(tx, ty, "- INITRD TAR + SYSCALLS: OK", C_INK, 1);
    ty += 26;

    gfx_text(tx, ty, "PROCHAINE ETAPE: SOURIS, FENETRES DEPLACABLES,", C_MUTED, 1);
    ty += 12;
    gfx_text(tx, ty, "ET UNE INVITE DE COMMANDES DANS UNE FENETRE.", C_MUTED, 1);

    /* Footer: the state of the machine, in the place a status bar would
     * go. It is the only part of this window that is not decoration. */
    int fy = y + h - 26;
    gfx_fill(x + 1, fy - 8, w - 2, 1, gfx_rgb(0xE2, 0xE8, 0xF0));
    gfx_text(x + 28, fy, "RING 3 OK - INITRD OK -", C_MUTED, 1);
    gfx_text(x + 28 + gfx_text_width("RING 3 OK - INITRD OK -", 1) + 6,
             fy, "1024X768", C_ACCENT, 1);

    gfx_frame(x, y, w, h, gfx_rgb(0xCE, 0xD5, 0xE0));
}

/* A desktop icon: a tinted tile with a glyph-like mark and a label, the
 * way a desktop icon reads at a glance without needing a real image. */
static void draw_icon(int x, int y, const char *label, uint32_t tint)
{
    gfx_fill(x, y, 52, 44, tint);
    gfx_frame(x, y, 52, 44, gfx_rgb(0xBF, 0xDB, 0xFF));
    gfx_fill(x + 8, y + 10, 36, 4, C_WHITE);
    gfx_fill(x + 8, y + 20, 24, 4, C_WHITE);
    gfx_text(x + 26 - gfx_text_width(label, 1) / 2, y + 50, label, C_WHITE, 1);
}

void desktop_draw(void)
{
    if (!gfx_ready())
        return;

    int w = gfx_width();
    int h = gfx_height();

    /* 1. Wallpaper. */
    gfx_gradient_v(0, 0, w, h, C_WALL_TOP, C_WALL_BOTTOM);

    /* 2. Desktop icons. Two is enough to prove the layout: a column that
     *    a launcher will fill from the initrd in the next step. */
    draw_icon(28, 28, "ORDINATEUR", gfx_rgb(0x14, 0x3A, 0x78));
    draw_icon(28, 108, "CORBEILLE", gfx_rgb(0x14, 0x3A, 0x78));

    /* 3. Window, centred above the taskbar. */
    int ww = w * 62 / 100;
    int wh = h * 46 / 100;
    if (ww > 760) ww = 760;
    if (wh > 420) wh = 420;
    draw_window((w - ww) / 2, (h - TASKBAR_H - wh) / 2, ww, wh);

    /* 4. Taskbar. */
    gfx_fill(0, h - TASKBAR_H, w, TASKBAR_H, C_TASKBAR);
    gfx_fill(0, h - TASKBAR_H, w, 2, C_ACCENT);

    draw_logo(16, h - TASKBAR_H + 14, 7, 3);
    gfx_text(44, h - TASKBAR_H + 18, "DEMARRER", C_WHITE, 1);

    /* 5. Status area. */
    gfx_text(w - 56, h - TASKBAR_H + 18, "12:00", C_WHITE, 1);
    gfx_text(w - 170, h - TASKBAR_H + 18, "WINDCLONE 0.0.4", C_MUTED, 1);
}
