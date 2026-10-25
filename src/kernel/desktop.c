/* WindClone — the desktop shell (Phase 8).
 *
 * This is a window manager, not a drawing demo: three windows, a z-order
 * you can change with the mouse, and title-bar buttons that actually do
 * something. What a window needs to be a window:
 *
 *   - focus    clicking anywhere on a window raises it (focus is the
 *              position in `order[]`, so raising is an array rotation)
 *   - minimize hidden but still open; the taskbar button brings it back
 *   - maximize fill the desktop above the taskbar; geometry is saved so
 *              restoring gives the exact window back
 *   - close    gone until a desktop icon opens it again
 *
 * Hit testing runs front to back and stops at the first window that owns
 * the click — that single rule is what makes overlapping windows behave
 * the way people expect. There is still no damage tracking and no double
 * buffer: a mouse move repaints everything, which is the next thing to
 * fix, not the thing to hide.
 *
 * The DEMARRER button is a start menu, and the menu is a launcher: its rows
 * are the /bin entries of the initrd, read back through the VFS every time
 * the panel opens, and clicking one really runs that program. Nothing in
 * this file knows a program's name.
 *
 * The font is uppercase A-Z 0-9 with ":-.!" only, so interface strings
 * stay clear of apostrophes: an unknown glyph silently becomes a space.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "desktop.h"
#include "gfx.h"
#include "log.h"
#include "mouse.h"
#include "serial.h"
#include "shell.h"
#include "vfs.h"
#include "version.h"

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

#define TASKBAR_H   44
#define TITLE_H     34
#define BTN_W       26
#define BTN_H       18
#define BTN_DY      8          /* buttons inside the title bar */
#define START_X     12
#define START_W     110
#define TASK_X      132        /* first taskbar window button */
#define TASK_W      168
#define TASK_GAP    6

enum { WIN_STATUS, WIN_TRASH, WIN_TERM, WIN_COUNT };
enum { BTN_MIN, BTN_MAX, BTN_CLOSE, BTN_NONE };

struct window {
    int x, y, w, h;            /* current geometry                     */
    int rx, ry, rw, rh;        /* geometry to restore when un-maximising */
    const char *title;         /* title bar                            */
    const char *task;          /* taskbar button                       */
    int kind;
    int open;                  /* closed windows leave until an icon reopens them */
    int min;                   /* minimised: open, hidden, taskbar only */
    int max;                   /* maximised: fills the desktop          */
};

static struct window wins[WIN_COUNT];
static int order[WIN_COUNT] = { WIN_STATUS, WIN_TERM, WIN_TRASH }; /* back -> front */
static int dragging, grab_dx, grab_dy, drag_win;
static int prev_left;
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

/* --- window bookkeeping ------------------------------------------------ */

/* Raising is a rotation: the focused window is always the last one drawn. */
static void raise_window(int id)
{
    for (int i = 0; i < WIN_COUNT; i++) {
        if (order[i] != id)
            continue;
        for (int j = i; j < WIN_COUNT - 1; j++)
            order[j] = order[j + 1];
        order[WIN_COUNT - 1] = id;
        return;
    }
}

static int focused(void)
{
    return order[WIN_COUNT - 1];
}

static void clamp_to_desktop(struct window *w, int W, int H)
{
    if (w->x < 0) w->x = 0;
    if (w->y < 0) w->y = 0;
    if (w->x + w->w > W) w->x = W - w->w;
    if (w->y + w->h > H - TASKBAR_H) w->y = H - TASKBAR_H - w->h;
}

static void set_maximised(struct window *w, int on, int W, int H)
{
    if (on == w->max)
        return;
    if (on) {
        w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
        w->x = 0; w->y = 0; w->w = W; w->h = H - TASKBAR_H;
        w->max = 1;
        klog("wm: window maximised");
    } else {
        w->x = w->rx; w->y = w->ry; w->w = w->rw; w->h = w->rh;
        w->max = 0;
        klog("wm: window restored");
    }
}

/* --- painting ---------------------------------------------------------- */

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

/* One title-bar button, painted from its index. BTN_CLOSE is red (and
 * lighter on hover); the other two are neutral chrome. */
static void draw_title_button(const struct window *w, int btn, int hover)
{
    int bx = w->x + w->w - 4 - BTN_W * (btn + 1);
    int by = w->y + BTN_DY;
    uint32_t bg;

    if (btn == BTN_CLOSE)
        bg = hover ? gfx_rgb(0xF8, 0x71, 0x71) : C_ACCENT;
    else
        bg = hover ? gfx_rgb(0x33, 0x41, 0x55) : C_TITLE_HOT;

    gfx_fill(bx, by, BTN_W, BTN_H, bg);

    int cx = bx + BTN_W / 2;
    int cy = by + BTN_H / 2;
    if (btn == BTN_MIN) {
        gfx_fill(cx - 5, cy + 4, 10, 2, C_WHITE);
    } else if (btn == BTN_MAX) {
        gfx_frame(cx - 5, cy - 5, 11, 11, C_WHITE);
    } else {
        for (int i = 0; i < 8; i++) {
            gfx_pixel(cx - 4 + i, cy - 4 + i, C_WHITE);
            gfx_pixel(cx + 4 - i, cy - 4 + i, C_WHITE);
        }
    }
}

/* Which button of this window's title bar is under (mx,my), BTN_NONE if
 * none. Layout is close, max, min packed to the right edge. */
static int title_button_at(const struct window *w, int mx, int my)
{
    if (my < w->y + BTN_DY || my >= w->y + BTN_DY + BTN_H)
        return BTN_NONE;
    for (int i = 0; i < 3; i++) {
        int bx = w->x + w->w - 4 - BTN_W * (i + 1);
        if (mx >= bx && mx < bx + BTN_W)
            return BTN_CLOSE - i;
    }
    return BTN_NONE;
}

static void draw_logo(int x, int y, int pane, int gap)
{
    gfx_fill(x, y, pane, pane, C_WHITE);
    gfx_fill(x + pane + gap, y, pane, pane, C_WHITE);
    gfx_fill(x, y + pane + gap, pane, pane, C_WHITE);
    gfx_fill(x + pane + gap, y + pane + gap, pane, pane, C_WHITE);
}

#define ICON_X   28
#define ICON_W   52
#define ICON_H   74            /* tile (44) + label underneath */
#define ICON_DY  80

static const struct {
    int y;
    const char *label;
    int win;
} icons[] = {
    { 28,  "ORDINATEUR", WIN_STATUS },
    { 108, "CORBEILLE",  WIN_TRASH  },
};

static void draw_icon(const char *label, int y)
{
    gfx_fill(ICON_X, y, ICON_W, 44, gfx_rgb(0x14, 0x3A, 0x78));
    gfx_frame(ICON_X, y, ICON_W, 44, gfx_rgb(0xBF, 0xDB, 0xFF));
    gfx_fill(ICON_X + 8, y + 10, 36, 4, C_WHITE);
    gfx_fill(ICON_X + 8, y + 20, 24, 4, C_WHITE);
    gfx_text(ICON_X + ICON_W / 2 - gfx_text_width(label, 1) / 2,
             y + 50, label, C_WHITE, 1);
}

static void draw_status_content(const struct window *w)
{
    int tx = w->x + 24;
    int ty = w->y + TITLE_H + 22;

    gfx_text(tx, ty, "WINDCLONE " WC_VERSION, C_INK, 2);
    ty += 34;
    gfx_text(tx, ty, "UN SYSTEME ECRIT DE ZERO", C_MUTED, 1);
    ty += 22;
    gfx_text(tx, ty, "- PAGES, GDT + TSS, RING 3", C_INK, 1);
    ty += 13;
    gfx_text(tx, ty, "- INITRD TAR + SYSCALLS", C_INK, 1);
    ty += 13;
    gfx_text(tx, ty, "- FRAMEBUFFER + SOURIS", C_INK, 1);
    ty += 13;
    gfx_text(tx, ty, "- FENETRES : REDUIRE, AGRANDIR,", C_INK, 1);
    ty += 13;
    gfx_text(tx, ty, "  FERMER", C_INK, 1);
    ty += 20;
    gfx_text(tx, ty, "LA BARRE DES TACHES REDUIT ET", C_MUTED, 1);
    ty += 12;
    gfx_text(tx, ty, "RESTAURE. LES ICONES OUVERT LES", C_MUTED, 1);
    ty += 12;
    gfx_text(tx, ty, "FENETRES FERMEES.", C_MUTED, 1);
    ty += 12;
    /* Said on screen because it is the part a user looks for and cannot see
     * from the window list: the menu is a launcher, not a decoration. */
    gfx_text(tx, ty, "DEMARRER OUVRIR UN MENU QUI", C_MUTED, 1);
    ty += 12;
    gfx_text(tx, ty, "LANCE LES PROGRAMMES DE /BIN.", C_MUTED, 1);
}

static void draw_trash_content(const struct window *w)
{
    int tx = w->x + 24;
    int ty = w->y + TITLE_H + 22;

    gfx_text(tx, ty, "CORBEILLE", C_INK, 2);
    ty += 34;
    gfx_text(tx, ty, "ELLE EST VIDE.", C_MUTED, 1);
    ty += 22;
    gfx_text(tx, ty, "- RIEN A SUPPRIMER.", C_INK, 1);
    ty += 13;
    gfx_text(tx, ty, "- RIEN A RESTAURER.", C_INK, 1);
    ty += 13;
    gfx_text(tx, ty, "- OUVERTE DEPUIS SON ICONE.", C_INK, 1);
}

static void draw_window(const struct window *w)
{
    int mx = mouse_x();
    int my = mouse_y();
    int front = (w == &wins[focused()]);
    uint32_t border = w->kind == WIN_TERM ? gfx_rgb(0x2B, 0x36, 0x48)
                                          : gfx_rgb(0xCE, 0xD5, 0xE0);

    /* Shadow only when there is room behind the window: a maximised one
     * has no behind, and the extra 6 px would poke into the taskbar. */
    if (!w->max)
        gfx_fill(w->x + 6, w->y + 6, w->w, w->h, gfx_rgb(0x0A, 0x1E, 0x45));

    gfx_fill(w->x, w->y, w->w, w->h, w->kind == WIN_TERM ? C_CONSOLE : C_BODY);
    gfx_fill(w->x, w->y, w->w, TITLE_H, front ? C_TITLE_HOT : C_TITLE);
    gfx_fill(w->x, w->y, 4, TITLE_H, front ? C_ACCENT : gfx_rgb(0x47, 0x55, 0x69));

    gfx_text(w->x + 16, w->y + 10, w->title, C_WHITE, 2);
    for (int btn = BTN_MIN; btn <= BTN_CLOSE; btn++)
        draw_title_button(w, btn,
                          title_button_at(w, mx, my) == btn && front);

    if (w->kind == WIN_STATUS)
        draw_status_content(w);
    else if (w->kind == WIN_TRASH)
        draw_trash_content(w);
    else
        shell_draw(w->x + 14, w->y + TITLE_H + 12, w->w - 28);

    gfx_frame(w->x, w->y, w->w, w->h, border);
}

/* --- start menu -------------------------------------------------------- */

#define MENU_X        START_X
#define MENU_W        236
#define MENU_HEADER_H 30
#define MENU_ITEM_H   22
#define MENU_FOOTER_H 26
#define MENU_GAP      6          /* air between the panel and the taskbar */
#define MENU_MAX      16         /* rows the array can hold; menu_scan() also
                                  * caps that by what the screen has room for */
#define MENU_NAME_MAX 24

/* Not a list written in this file: every row is a /bin entry of the initrd,
 * read back through the VFS each time the panel opens. Add a program to the
 * archive and it is launchable with the mouse without a line of desktop code
 * changing — which is the only honest meaning of "a launcher for programs
 * from the initrd". A hardcoded menu would be a screenshot of one. */
static char menu_names[MENU_MAX][MENU_NAME_MAX];
static int menu_count;    /* rows that hold a program */
static int menu_hidden;   /* entries /bin has no row for */
static int menu_open;

/* Archive names come from initrd.c with no leading slash: "bin/hello",
 * "etc/motd". Only files under bin/ are programs. */
static int menu_is_program(const char *name)
{
    const char *p = "bin/";
    for (int i = 0; p[i] != '\0'; i++)
        if (name[i] != p[i])
            return 0;
    return name[4] != '\0';
}

static void menu_scan(int H)
{
    /* Rows the screen can actually hold, the header, the action row, the
     * footer and the taskbar subtracted. A menu that overflowed the desktop
     * would put its last programs where no click can reach them, and a
     * launcher that quietly loses programs is worse than one that says so in
     * the footer. */
    int room = H - TASKBAR_H - MENU_GAP - MENU_HEADER_H - MENU_FOOTER_H
               - MENU_ITEM_H;
    room /= MENU_ITEM_H;
    if (room > MENU_MAX)
        room = MENU_MAX;
    if (room < 0)
        room = 0;

    menu_count = 0;
    menu_hidden = 0;

    for (uint64_t i = 0; ; i++) {
        struct wc_dirent de;
        if (vfs_dirent(i, &de) < 0)
            break;                     /* past the last entry: that is the end */
        if (!menu_is_program(de.name))
            continue;

        const char *base = de.name + 4;
        int l = 0;
        while (base[l] != '\0')
            l++;
        /* A name with no room in a row stays out rather than being truncated
         * into a name that does not exist: a menu that lies about what it
         * will launch is worse than one that admits it is missing something. */
        if (l >= MENU_NAME_MAX || menu_count >= room) {
            menu_hidden++;
            continue;
        }
        for (int k = 0; k <= l; k++)
            menu_names[menu_count][k] = base[k];
        /* One line per program on the serial port, not in the log ring: the
         * ring is 48 messages deep and a menu of a dozen would flush the boot
         * evidence out of it, while the file a test greps keeps everything. */
        serial_write("wm: menu row ");
        serial_write_dec((uint64_t)menu_count);
        serial_write(" ");
        serial_write(base);
        serial_write("\n");
        menu_count++;
    }

    /* Both halves of the number, so a test can prove the panel showed the
     * whole of /bin rather than the part that was convenient to draw. */
    serial_write("wm: menu rows ");
    serial_write_dec((uint64_t)menu_count);
    serial_write(", hidden ");
    serial_write_dec((uint64_t)menu_hidden);
    serial_write("\n");
}

/* The panel grows upward from the button it came from. Drawing and hit
 * testing both go through these two helpers, so a row can never be clickable
 * somewhere other than where it is painted. */
static int menu_height(void)
{
    return MENU_HEADER_H + (menu_count + 1) * MENU_ITEM_H + MENU_FOOTER_H;
}

static int menu_top(int H)
{
    return H - TASKBAR_H - menu_height() - MENU_GAP;
}

/* 0..menu_count-1 = a program, menu_count = the show-desktop row, -1 = inside
 * the panel but not on a row, -2 = outside it. */
static int menu_row_at(int mx, int my, int H)
{
    int top = menu_top(H);
    if (mx < MENU_X || mx >= MENU_X + MENU_W ||
        my < top || my >= top + menu_height())
        return -2;
    for (int i = 0; i <= menu_count; i++) {
        int ry = top + MENU_HEADER_H + i * MENU_ITEM_H;
        if (my >= ry && my < ry + MENU_ITEM_H)
            return i;
    }
    return -1;
}

/* The 5x7 font takes strings, not numbers, so a count has to be spelled into
 * a buffer before it can be drawn. Returns the new length. */
static int append_num(char *line, int n, int cap, int v)
{
    char digits[8];
    int d = 0;
    do {
        digits[d++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0 && d < (int)sizeof digits);
    while (d > 0 && n < cap - 1)
        line[n++] = digits[--d];
    return n;
}

static int append_text(char *line, int n, int cap, const char *s)
{
    for (int i = 0; s[i] != '\0' && n < cap - 1; i++)
        line[n++] = s[i];
    return n;
}

static void draw_menu(int H)
{
    int top = menu_top(H);
    int hot = menu_row_at(mouse_x(), mouse_y(), H);

    gfx_fill(MENU_X, top, MENU_W, menu_height(), gfx_rgb(0x0D, 0x11, 0x17));
    gfx_frame(MENU_X, top, MENU_W, menu_height(), gfx_rgb(0x2B, 0x36, 0x48));
    gfx_fill(MENU_X, top, MENU_W, 2, C_ACCENT);

    draw_logo(MENU_X + 12, top + 10, 6, 3);
    gfx_text(MENU_X + 36, top + 12, "PROGRAMMES /BIN", C_WHITE, 1);

    for (int i = 0; i < menu_count; i++) {
        int ry = top + MENU_HEADER_H + i * MENU_ITEM_H;
        if (i == hot)
            gfx_fill(MENU_X + 3, ry, MENU_W - 6, MENU_ITEM_H, C_TITLE_HOT);
        gfx_text(MENU_X + 16, ry + 8, menu_names[i], C_WHITE, 1);
    }

    /* One action under the programs: the button that used to BE this menu
     * still does what it did, it just lives in the panel now. */
    int ay = top + MENU_HEADER_H + menu_count * MENU_ITEM_H;
    gfx_fill(MENU_X + 3, ay - 1, MENU_W - 6, 1, gfx_rgb(0x2B, 0x36, 0x48));
    if (hot == menu_count)
        gfx_fill(MENU_X + 3, ay, MENU_W - 6, MENU_ITEM_H, C_TITLE_HOT);
    gfx_text(MENU_X + 16, ay + 8, "AFFICHER LE BUREAU", C_WHITE, 1);

    /* The count is the claim: this panel is /bin, not a handful of names a
     * desktop happened to be written with. */
    char line[48];
    int n = append_num(line, 0, (int)sizeof line, menu_count);
    n = append_text(line, n, (int)sizeof line, " PROGRAMMES");
    if (menu_hidden > 0) {
        n = append_text(line, n, (int)sizeof line, "  +");
        n = append_num(line, n, (int)sizeof line, menu_hidden);
        n = append_text(line, n, (int)sizeof line, " CACHES");
    }
    line[n] = '\0';
    gfx_text(MENU_X + 16, ay + MENU_ITEM_H + 8, line, C_MUTED, 1);
}

static void draw_taskbar(int W, int H)
{
    int y = H - TASKBAR_H;
    int mx = mouse_x();
    int my = mouse_y();

    gfx_fill(0, y, W, TASKBAR_H, C_TASKBAR);
    gfx_fill(0, y, W, 2, C_ACCENT);

    /* Open means lit: a taskbar button whose menu is on screen has to look
     * like it, or the panel appears to belong to nothing. */
    int start_hot = menu_open ||
                    (mx >= START_X && mx < START_X + START_W && my >= y);
    gfx_fill(START_X, y + 8, START_W, 28,
             start_hot ? C_TITLE_HOT : gfx_rgb(0x16, 0x1B, 0x22));
    draw_logo(START_X + 8, y + 14, 7, 3);
    gfx_text(START_X + 34, y + 17, "DEMARRER", C_WHITE, 1);

    int x = TASK_X;
    for (int id = 0; id < WIN_COUNT; id++) {
        struct window *w = &wins[id];
        if (!w->open)
            continue;
        int active = !w->min && id == focused();
        gfx_fill(x, y + 8, TASK_W, 28,
                 active ? C_TITLE_HOT : gfx_rgb(0x13, 0x18, 0x20));
        gfx_fill(x, y + 8, 3, 28, active ? C_ACCENT : gfx_rgb(0x2B, 0x36, 0x48));
        gfx_text(x + 12, y + 17, w->task, w->min ? C_MUTED : C_WHITE, 1);
        x += TASK_W + TASK_GAP;
    }

    /* Same macro as the status window: one number, three places that show
     * it, and no way for one of them to fall behind a release. */
    gfx_text(W - 178, y + 17, "WINDCLONE " WC_VERSION, C_MUTED, 1);
    gfx_text(W - 56, y + 17, "12:00", C_WHITE, 1);
}

/* --- input ------------------------------------------------------------- */

static int task_button_at(int mx, int my, int H)
{
    if (my < H - TASKBAR_H || my >= H)
        return -1;
    int x = TASK_X;
    for (int id = 0; id < WIN_COUNT; id++) {
        if (!wins[id].open)
            continue;
        if (mx >= x && mx < x + TASK_W)
            return id;
        x += TASK_W + TASK_GAP;
    }
    return -2;               /* on the bar, but not on a button */
}

/* Show desktop: hide every window, or bring them all back. Reached from the
 * start menu's action row — the DEMARRER button itself opens the menu. */
static void toggle_show_desktop(void)
{
    int anything_shown = 0;
    for (int i = 0; i < WIN_COUNT; i++)
        if (wins[i].open && !wins[i].min)
            anything_shown = 1;

    for (int i = 0; i < WIN_COUNT; i++)
        if (wins[i].open)
            wins[i].min = anything_shown;

    klog(anything_shown ? "wm: show desktop" : "wm: windows restored");
}

/* One mouse press, resolved in the order a desktop user means it: an open
 * menu first, then the taskbar, then title bars, then window bodies, then
 * icons. */
static void handle_press(int mx, int my, int W, int H)
{
    /* The menu owns the screen while it is open. A row runs its action; a
     * click anywhere else closes the panel and stops there — the click that
     * dismisses a menu does not fall through to whatever is underneath it,
     * which is what makes a menu safe to click away from. */
    if (menu_open) {
        int row = menu_row_at(mx, my, H);
        if (row == -2) {
            menu_open = 0;
            klog("wm: start menu closed");
            return;
        }
        if (row < 0)
            return;                        /* frame, header, footer */
        if (row == menu_count) {
            menu_open = 0;
            toggle_show_desktop();
            return;
        }

        /* Copy the name: shell_launch() does not return until the program has
         * exited, and the panel must already be gone by then — a menu left on
         * screen during a run would be a picture of a state that ended. */
        char name[MENU_NAME_MAX];
        for (int i = 0; i < MENU_NAME_MAX; i++) {
            name[i] = menu_names[row][i];
            if (name[i] == '\0')
                break;
        }
        menu_open = 0;

        /* The program's stdout is the terminal window. Showing it is the
         * difference between a launch a user can see and a launch the log
         * alone remembers. */
        wins[WIN_TERM].open = 1;
        wins[WIN_TERM].min = 0;
        raise_window(WIN_TERM);

        shell_launch(name);
        return;
    }

    if (my >= H - TASKBAR_H) {
        if (mx >= START_X && mx < START_X + START_W) {
            /* Scanned on the way in, not cached from boot: the menu is the
             * archive as it is now, so a program added to the initrd appears
             * in the panel without anything here changing. */
            menu_scan(H);
            menu_open = 1;
            klog("wm: start menu opened, %d programs", menu_count);
            return;
        }
        int id = task_button_at(mx, my, H);
        if (id < 0)
            return;
        if (wins[id].min) {
            wins[id].min = 0;
            raise_window(id);
            klog("wm: window restored from taskbar");
        } else if (focused() == id) {
            wins[id].min = 1;
            klog("wm: window minimised");
        } else {
            raise_window(id);
        }
        return;
    }

    /* Title bars, front to back: the first one under the pointer owns the
     * press, buttons included. That is the whole hit test. */
    for (int i = WIN_COUNT - 1; i >= 0; i--) {
        int id = order[i];
        struct window *w = &wins[id];
        if (!w->open || w->min)
            continue;
        if (mx < w->x || mx >= w->x + w->w ||
            my < w->y || my >= w->y + TITLE_H)
            continue;

        raise_window(id);
        switch (title_button_at(w, mx, my)) {
        case BTN_CLOSE:
            w->open = 0; w->min = 0; w->max = 0;
            klog("wm: window closed");
            return;
        case BTN_MIN:
            w->min = 1;
            klog("wm: window minimised");
            return;
        case BTN_MAX:
            set_maximised(w, !w->max, W, H);
            return;
        default:
            break;
        }
        if (w->max)
            return;                     /* a maximised window does not drag */
        dragging = 1;
        drag_win = id;
        grab_dx = mx - w->x;
        grab_dy = my - w->y;
        return;
    }

    /* Window bodies: clicking one raises it. Focus without a drag, which
     * is what makes two overlapping windows usable at all. */
    for (int i = WIN_COUNT - 1; i >= 0; i--) {
        int id = order[i];
        struct window *w = &wins[id];
        if (!w->open || w->min)
            continue;
        if (mx < w->x || mx >= w->x + w->w ||
            my < w->y || my >= w->y + w->h)
            continue;
        raise_window(id);
        return;
    }

    /* Desktop icons: reopen a window that was closed or minimise it back. */
    for (int i = 0; i < (int)(sizeof icons / sizeof icons[0]); i++) {
        if (mx < ICON_X || mx >= ICON_X + ICON_W ||
            my < icons[i].y || my >= icons[i].y + ICON_H)
            continue;
        struct window *w = &wins[icons[i].win];
        w->open = 1;
        w->min = 0;
        raise_window(icons[i].win);
        klog("wm: window opened from icon");
        return;
    }
}

/* --- frame ------------------------------------------------------------- */

static void layout(void)
{
    int W = gfx_width();
    int H = gfx_height();

    /* Clear of the icon column: windows overlapping icons looks like a
     * bug, windows overlapping each other looks like a desktop. */
    wins[WIN_STATUS].x = 116;  wins[WIN_STATUS].y = 88;
    wins[WIN_STATUS].w = 400;  wins[WIN_STATUS].h = 300;
    wins[WIN_STATUS].title = "WINDCLONE";
    wins[WIN_STATUS].task  = "WINDCLONE";
    wins[WIN_STATUS].kind  = WIN_STATUS;
    wins[WIN_STATUS].open  = 1;

    wins[WIN_TERM].w = 600;  wins[WIN_TERM].h = 380;
    if (wins[WIN_TERM].w > W - 40) wins[WIN_TERM].w = W - 40;
    wins[WIN_TERM].x = W - wins[WIN_TERM].w - 48;
    wins[WIN_TERM].y = H - TASKBAR_H - wins[WIN_TERM].h - 60;
    wins[WIN_TERM].title = "INVITE DE COMMANDES";
    wins[WIN_TERM].task  = "INVITE DE COMMANDES";
    wins[WIN_TERM].kind  = WIN_TERM;
    wins[WIN_TERM].open  = 1;

    /* Closed by default: it exists to prove a desktop icon can open a
     * window, which is how a window comes back after being closed. */
    wins[WIN_TRASH].w = 360;  wins[WIN_TRASH].h = 230;
    wins[WIN_TRASH].x = (W - wins[WIN_TRASH].w) / 2;
    wins[WIN_TRASH].y = H - TASKBAR_H - wins[WIN_TRASH].h - 90;
    wins[WIN_TRASH].title = "CORBEILLE";
    wins[WIN_TRASH].task  = "CORBEILLE";
    wins[WIN_TRASH].kind  = WIN_TRASH;
    wins[WIN_TRASH].open  = 0;

    for (int i = 0; i < WIN_COUNT; i++) {
        wins[i].rx = wins[i].x;  wins[i].ry = wins[i].y;
        wins[i].rw = wins[i].w;  wins[i].rh = wins[i].h;
    }

    laid_out = 1;
}

void desktop_frame(void)
{
    if (!gfx_ready())
        return;

    int W = gfx_width();
    int H = gfx_height();

    if (!laid_out)
        layout();

    int mx = mouse_x();
    int my = mouse_y();
    int left = mouse_left();
    int press = left && !prev_left;     /* edge, not level: one action per click */
    prev_left = left;

    if (!left)
        dragging = 0;

    if (dragging) {
        struct window *w = &wins[drag_win];
        w->x = mx - grab_dx;
        w->y = my - grab_dy;
        clamp_to_desktop(w, W, H);
    }

    if (press)
        handle_press(mx, my, W, H);

    gfx_gradient_v(0, 0, W, H, C_WALL_TOP, C_WALL_BOTTOM);
    for (int i = 0; i < (int)(sizeof icons / sizeof icons[0]); i++)
        draw_icon(icons[i].label, icons[i].y);

    for (int i = 0; i < WIN_COUNT; i++) {
        struct window *w = &wins[order[i]];
        if (w->open && !w->min)
            draw_window(w);
    }

    draw_taskbar(W, H);
    if (menu_open)
        draw_menu(H);                  /* above the windows: it asked for them */
    draw_cursor(mx, my);
}
