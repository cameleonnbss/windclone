/* WindClone — the desktop shell (Phase 8).
 *
 * A compositor in the only sense that matters right now: every frame is
 * painted back to front into the framebuffer — wallpaper, icons, window,
 * taskbar, cursor. There is no damage tracking and no double buffer, so a
 * mouse that moves repaints the whole screen. That is honest for a first
 * window server and it is the next thing to fix, not the thing to hide.
 *
 * The window is not a data structure yet (no list, no z-order, no focus) —
 * it is one window with a position, because one window that can be dragged
 * proves the input path end to end.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "desktop.h"
#include "gfx.h"
#include "mouse.h"

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
#define TITLE_H   34

/* Window geometry, kept in one place because dragging moves it. */
static int win_x = -1, win_y = -1, win_w, win_h;
static int dragging;
static int grab_dx, grab_dy;

/* 12x16 pointer: '#' outline, 'W' fill, '.' transparent. A filled arrow
 * without an outline disappears over the light window body. */
static const char *const cursor_shape[] = {
    "#...........",
    "##..........",
    "#W#.........",
    "#WW#........",
    "#WWW#.......",
    "#WWWW#......",
    "#WWWWW#.....",
    "#WWWWWW#....",
    "#WWWWWWW#...",
    "#WWWWWWWW#..",
    "#WWWWW#####.",
    "#WW#W#......",
    "#W#.#W#.....",
    "##..#W#.....",
    "#....#W#....",
    "......##....",
};

static void draw_cursor(int x, int y)
{
    for (int row = 0; row < 16; row++) {
        const char *line = cursor_shape[row];
        for (int col = 0; line[col] != '\0'; col++) {
            if (line[col] == '#')
                gfx_pixel(x + col, y + row, gfx_rgb(0x00, 0x00, 0x00));
            else if (line[col] == 'W')
                gfx_pixel(x + col, y + row, C_WHITE);
        }
    }
}

static void draw_close_button(int x, int y, int size)
{
    gfx_fill(x, y, size, size, C_ACCENT);
    for (int i = 0; i < size; i++) {
        gfx_pixel(x + i, y + i, C_WHITE);
        gfx_pixel(x + size - 1 - i, y + i, C_WHITE);
    }
}

static void draw_logo(int x, int y, int pane, int gap)
{
    gfx_fill(x, y, pane, pane, C_WHITE);
    gfx_fill(x + pane + gap, y, pane, pane, C_WHITE);
    gfx_fill(x, y + pane + gap, pane, pane, C_WHITE);
    gfx_fill(x + pane + gap, y + pane + gap, pane, pane, C_WHITE);
}

static void draw_icon(int x, int y, const char *label)
{
    gfx_fill(x, y, 52, 44, gfx_rgb(0x14, 0x3A, 0x78));
    gfx_frame(x, y, 52, 44, gfx_rgb(0xBF, 0xDB, 0xFF));
    gfx_fill(x + 8, y + 10, 36, 4, C_WHITE);
    gfx_fill(x + 8, y + 20, 24, 4, C_WHITE);
    gfx_text(x + 26 - gfx_text_width(label, 1) / 2, y + 50, label, C_WHITE, 1);
}

static void draw_window(void)
{
    int x = win_x, y = win_y, w = win_w, h = win_h;

    gfx_fill(x + 6, y + 6, w, h, gfx_rgb(0x0A, 0x1E, 0x45));
    gfx_fill(x, y, w, h, C_BODY);
    gfx_fill(x, y, w, TITLE_H, C_TITLE);
    gfx_fill(x, y, 4, TITLE_H, C_ACCENT);

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

    gfx_text(tx, ty, "- PAGES, GDT + TSS, RING 3: OK", C_INK, 1);
    ty += 12;
    gfx_text(tx, ty, "- INITRD TAR + SYSCALLS: OK", C_INK, 1);
    ty += 12;
    gfx_text(tx, ty, "- SOURIS PS/2 + FENETRE DEPLACABLE: OK", C_INK, 1);
    ty += 26;

    gfx_text(tx, ty, "ATTRAPE LA BARRE DE TITRE ET DEPLACE.", C_MUTED, 1);

    int fy = y + h - 26;
    gfx_fill(x + 1, fy - 8, w - 2, 1, gfx_rgb(0xE2, 0xE8, 0xF0));
    gfx_text(x + 28, fy, "RING 3 - INITRD - SOURIS", C_MUTED, 1);
    gfx_text(x + w - 96, fy, "1024X768", C_ACCENT, 1);

    gfx_frame(x, y, w, h, gfx_rgb(0xCE, 0xD5, 0xE0));
}

void desktop_frame(void)
{
    if (!gfx_ready())
        return;

    int w = gfx_width();
    int h = gfx_height();

    if (win_x < 0) {                       /* first frame: centre it */
        win_w = w * 62 / 100;
        win_h = h * 46 / 100;
        if (win_w > 760) win_w = 760;
        if (win_h > 420) win_h = 420;
        win_x = (w - win_w) / 2;
        win_y = (h - TASKBAR_H - win_h) / 2;
    }

    /* Drag: grab the title bar, keep the offset under the pointer, and let
     * the window follow. The clamp keeps the title bar reachable. */
    int mx = mouse_x();
    int my = mouse_y();

    if (mouse_left()) {
        if (!dragging &&
            mx >= win_x && mx < win_x + win_w &&
            my >= win_y && my < win_y + TITLE_H) {
            dragging = 1;
            grab_dx = mx - win_x;
            grab_dy = my - win_y;
        }
        if (dragging) {
            win_x = mx - grab_dx;
            win_y = my - grab_dy;
            if (win_x < 0) win_x = 0;
            if (win_y < 0) win_y = 0;
            if (win_x + win_w > w) win_x = w - win_w;
            if (win_y + win_h > h - TASKBAR_H) win_y = h - TASKBAR_H - win_h;
        }
    } else {
        dragging = 0;
    }

    gfx_gradient_v(0, 0, w, h, C_WALL_TOP, C_WALL_BOTTOM);
    draw_icon(28, 28, "ORDINATEUR");
    draw_icon(28, 108, "CORBEILLE");

    draw_window();

    gfx_fill(0, h - TASKBAR_H, w, TASKBAR_H, C_TASKBAR);
    gfx_fill(0, h - TASKBAR_H, w, 2, C_ACCENT);
    draw_logo(16, h - TASKBAR_H + 14, 7, 3);
    gfx_text(44, h - TASKBAR_H + 18, "DEMARRER", C_WHITE, 1);
    gfx_text(w - 170, h - TASKBAR_H + 18, "WINDCLONE 0.0.4", C_MUTED, 1);
    gfx_text(w - 56, h - TASKBAR_H + 18, "12:00", C_WHITE, 1);

    draw_cursor(mx, my);
}
