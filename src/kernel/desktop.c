/* WindClone — the desktop shell (Phase 8).
 *
 * Two overlapping windows is the smallest arrangement that forces the two
 * ideas a window system actually needs: z-order (a click brings a window to
 * the front) and hit testing (only the title bar drags). There is still no
 * damage tracking and no double buffer — a mouse move repaints everything —
 * which is the next thing to fix, not the thing to hide.
 *
 * Windows are painted in `order[]`, so raising one is a swap in that array
 * rather than a sort. Content is drawn from the window's kind: one is a
 * status card, the other is the live command line in shell.c.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "desktop.h"
#include "gfx.h"
#include "mouse.h"
#include "shell.h"

#define C_WALL_TOP    gfx_rgb(0x0B, 0x2A, 0x63)
#define C_WALL_BOTTOM gfx_rgb(0x1F, 0x6B, 0xD8)
#define C_TASKBAR     gfx_rgb(0x0D, 0x11, 0x17)
#define C_ACCENT      gfx_rgb(0xEF, 0x44, 0x44)
#define C_TITLE       gfx_rgb(0x10, 0x18, 0x27)
#define C_TITLE_HOT   gfx_rgb(0x1E, 0x29, 0x3B)
#define C_BODY        gfx_rgb(0xF8, 0xFA, 0xFC)
#define C_CONSOLE     gfx_rgb(0x0B, 0x12, 0x1B)
#define C_INK         gfx_rgb(0x0F, 0x17, 0x2A)
#define C_MUTED       gfx_rgb(0x64, 0x74, 0x8B)
#define C_WHITE       gfx_rgb(0xFF, 0xFF, 0xFF)

#define TASKBAR_H 44
#define TITLE_H   34

enum { WIN_STATUS, WIN_TERM };

struct window {
    int x, y, w, h;
    const char *title;
    int kind;
};

static struct window wins[2];
static int order[2] = { WIN_STATUS, WIN_TERM };   /* painted back to front */
static int dragging, grab_dx, grab_dy, drag_win;
static int laid_out;

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

static void draw_status_content(const struct window *w)
{
    int tx = w->x + 24;
    int ty = w->y + TITLE_H + 22;

    gfx_text(tx, ty, "WINDCLONE 0.0.4", C_INK, 2);
    ty += 34;
    gfx_text(tx, ty, "UN SYSTEME ECRIT DE ZERO", C_MUTED, 1);
    ty += 22;
    gfx_text(tx, ty, "- PAGES, GDT + TSS, RING 3", C_INK, 1);
    ty += 13;
    gfx_text(tx, ty, "- INITRD TAR + SYSCALLS", C_INK, 1);
    ty += 13;
    gfx_text(tx, ty, "- FRAMEBUFFER + SOURIS + FENETRES", C_INK, 1);
    ty += 13;
    gfx_text(tx, ty, "- INVITE DE COMMANDES (FENETRE)", C_INK, 1);
    ty += 24;
    gfx_text(tx, ty, "CLIQUE UNE BARRE DE TITRE POUR METTRE", C_MUTED, 1);
    ty += 12;
    gfx_text(tx, ty, "UNE FENETRE AU PREMIER PLAN, PUIS", C_MUTED, 1);
    ty += 12;
    gfx_text(tx, ty, "DEPLACE-LA.", C_MUTED, 1);
}

static void draw_window(const struct window *w, int front)
{
    /* Shadow, body, title bar. The front window gets a brighter title so
     * focus is readable at a glance. */
    gfx_fill(w->x + 6, w->y + 6, w->w, w->h, gfx_rgb(0x0A, 0x1E, 0x45));
    gfx_fill(w->x, w->y, w->w, w->h, w->kind == WIN_TERM ? C_CONSOLE : C_BODY);
    gfx_fill(w->x, w->y, w->w, TITLE_H, front ? C_TITLE_HOT : C_TITLE);
    gfx_fill(w->x, w->y, 4, TITLE_H, front ? C_ACCENT : gfx_rgb(0x47, 0x55, 0x69));

    gfx_text(w->x + 16, w->y + 10, w->title, C_WHITE, 2);
    draw_close_button(w->x + w->w - 34, w->y + 8, 18);

    if (w->kind == WIN_STATUS)
        draw_status_content(w);
    else
        shell_draw(w->x + 14, w->y + TITLE_H + 12, w->w - 28);

    gfx_frame(w->x, w->y, w->w, w->h,
              w->kind == WIN_TERM ? gfx_rgb(0x2B, 0x36, 0x48) : gfx_rgb(0xCE, 0xD5, 0xE0));
}

static void layout(void)
{
    int w = gfx_width();
    int h = gfx_height();

    /* Clear of the icon column: windows overlapping icons looks like a
     * bug, windows overlapping each other looks like a desktop. */
    wins[WIN_STATUS].x = 116;
    wins[WIN_STATUS].y = 88;
    wins[WIN_STATUS].w = 400;
    wins[WIN_STATUS].h = 300;
    wins[WIN_STATUS].title = "WINDCLONE";
    wins[WIN_STATUS].kind = WIN_STATUS;

    wins[WIN_TERM].w = 600;
    wins[WIN_TERM].h = 380;
    if (wins[WIN_TERM].w > w - 40) wins[WIN_TERM].w = w - 40;
    wins[WIN_TERM].x = w - wins[WIN_TERM].w - 48;
    wins[WIN_TERM].y = h - TASKBAR_H - wins[WIN_TERM].h - 60;
    wins[WIN_TERM].title = "INVITE DE COMMANDES";
    wins[WIN_TERM].kind = WIN_TERM;

    laid_out = 1;
}

void desktop_frame(void)
{
    if (!gfx_ready())
        return;

    int w = gfx_width();
    int h = gfx_height();

    if (!laid_out)
        layout();

    int mx = mouse_x();
    int my = mouse_y();

    /* Hit test front to back: the first title bar under the pointer wins,
     * and that window comes to the front (the swap is what "focus" is). */
    if (mouse_left() && !dragging) {
        for (int i = 1; i >= 0; i--) {
            struct window *win = &wins[order[i]];
            if (mx >= win->x && mx < win->x + win->w &&
                my >= win->y && my < win->y + TITLE_H) {
                drag_win = order[i];
                if (i == 0) {
                    order[0] = order[1];
                    order[1] = drag_win;
                }
                dragging = 1;
                grab_dx = mx - win->x;
                grab_dy = my - win->y;
                break;
            }
        }
    }
    if (!mouse_left())
        dragging = 0;

    if (dragging) {
        struct window *win = &wins[drag_win];
        win->x = mx - grab_dx;
        win->y = my - grab_dy;
        if (win->x < 0) win->x = 0;
        if (win->y < 0) win->y = 0;
        if (win->x + win->w > w) win->x = w - win->w;
        if (win->y + win->h > h - TASKBAR_H) win->y = h - TASKBAR_H - win->h;
    }

    gfx_gradient_v(0, 0, w, h, C_WALL_TOP, C_WALL_BOTTOM);
    draw_icon(28, 28, "ORDINATEUR");
    draw_icon(28, 108, "CORBEILLE");

    for (int i = 0; i < 2; i++)
        draw_window(&wins[order[i]], i == 1);

    gfx_fill(0, h - TASKBAR_H, w, TASKBAR_H, C_TASKBAR);
    gfx_fill(0, h - TASKBAR_H, w, 2, C_ACCENT);
    draw_logo(16, h - TASKBAR_H + 14, 7, 3);
    gfx_text(44, h - TASKBAR_H + 18, "DEMARRER", C_WHITE, 1);
    gfx_text(w - 170, h - TASKBAR_H + 18, "WINDCLONE 0.0.4", C_MUTED, 1);
    gfx_text(w - 56, h - TASKBAR_H + 18, "12:00", C_WHITE, 1);

    draw_cursor(mx, my);
}
