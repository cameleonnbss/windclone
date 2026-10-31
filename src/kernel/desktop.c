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
 * the way people expect. The scene is composed into a RAM back buffer and
 * pushed in one flip, and only the damaged rectangles are recomputed: the
 * band the cursor swept, a title strip it hovered, the terminal body, and
 * each program window whose own output changed. A full-screen repaint is
 * left for the genuinely structural moments - the first frame, a click, a
 * drag, or the start menu opening or closing.
 *
 * The DEMARRER button is a start menu, and the menu is a launcher: its rows
 * are the /bin entries of the initrd, read back through the VFS every time
 * the panel opens, and clicking one starts that program as a *child of the
 * shell's own process slot* — so a click has a pid, a parent that reaps it and
 * an exit code, exactly like the same word typed at the prompt. Nothing in this
 * file knows a program's name.
 *
 * The font is uppercase A-Z 0-9 plus the punctuation the interface really
 * uses (":-,.!/'+()"), and glyph_for() answers an unknown character with a
 * space, so interface strings must stay inside that set: a missing glyph is not
 * a box, it is a blank nobody notices.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "desktop.h"
#include "gfx.h"
#include "log.h"
#include "mouse.h"
#include "proc.h"
#include "rtc.h"
#include "serial.h"
#include "shell.h"
#include "vfs.h"
#include "version.h"

/* --- palette -----------------------------------------------------------
 * The colours are not decoration: they are the difference between a desktop
 * that reads as "a windowing system" and one that reads as Windows. The accent
 * is the blue the operating system people use actually ships with, the close
 * button is the red it shows on hover, and the glass is the wallpaper showing
 * through a panel rather than a flat grey hiding it.
 *
 * One rule this palette keeps: nothing here is a colour copied out of somebody
 * else's screenshot. Every pixel on this desktop is computed by these lines
 * from these numbers, which is the only way the result can be shipped in a
 * release and called yours. */
#define C_WALL_TOP    gfx_rgb(0x05, 0x13, 0x2E)
#define C_WALL_BOTTOM gfx_rgb(0x13, 0x3E, 0x7E)
#define C_TASKBAR     gfx_rgb(0x0D, 0x11, 0x17)
#define C_ACCENT      gfx_rgb(0x4C, 0xC2, 0xFF)   /* the indicator blue       */
#define C_ACCENT_DEEP gfx_rgb(0x00, 0x78, 0xD4)   /* a link, a lit edge       */
#define C_CLOSE       gfx_rgb(0xC4, 0x2B, 0x1C)   /* close on hover, as it is */
#define C_TITLE       gfx_rgb(0x10, 0x18, 0x27)
#define C_TITLE_HOT   gfx_rgb(0x1E, 0x29, 0x3B)
#define C_CAP_GLYPH   gfx_rgb(0xD8, 0xDE, 0xE6)   /* caption marks, unlit     */
#define C_BODY        gfx_rgb(0xF8, 0xFA, 0xFC)
#define C_CONSOLE     gfx_rgb(0x0B, 0x12, 0x1B)
#define C_INK         gfx_rgb(0x0F, 0x17, 0x2A)
#define C_MUTED       gfx_rgb(0x64, 0x74, 0x8B)
#define C_WHITE       gfx_rgb(0xFF, 0xFF, 0xFF)
#define C_BLACK       gfx_rgb(0x00, 0x00, 0x00)

/* How translucent each glass panel is: 0 is a window into the wallpaper, 255 is
 * a flat colour. These are the two numbers that decide whether the desktop looks
 * like it has acrylic in it, so they are named rather than buried in a call. */
#define A_TASKBAR     218
#define A_PANEL       232
#define A_SHADOW      96      /* the soft drop under a window, blended in     */
#define WIN_RADIUS    8       /* corner radius of a window and of every panel */

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
#define RESIZE_EDGE 5          /* px band that counts as a border grab */
#define MIN_W       200        /* floors: keep console/program text usable */
#define MIN_H       120
#define SNAP_MARGIN 24         /* drag the title this close to an edge to snap */

enum { WIN_STATUS, WIN_TRASH, WIN_TERM, WIN_PROGRAM };
enum { BTN_MIN, BTN_MAX, BTN_CLOSE, BTN_NONE };
/* Which borders a press grabbed, as a bitmask: a corner sets two bits. The
 * body pass never produces RS_TOP (the title bar owns that edge this release). */
enum { RS_NONE = 0, RS_LEFT = 1, RS_RIGHT = 2, RS_TOP = 4, RS_BOTTOM = 8 };
/* Which cursor glyph draw_cursor() paints for a border mask (cursor_shape_id). */
enum { CUR_ARROW, CUR_NS, CUR_EW, CUR_NWSE, CUR_NESW };
/* Where a dragged window snapped to, and what it restores from. 0 is floating;
 * 1/2 are the left/right halves, 3..6 the four corners (top-left, top-right,
 * bottom-left, bottom-right). The pre-snap rect lives in rx/ry/rw/rh. */
enum { SNAP_NONE, SNAP_LEFT, SNAP_RIGHT, SNAP_TL, SNAP_TR, SNAP_BL, SNAP_BR };

/* Three windows are fixed (status, trash, terminal) and live at indices 0..2,
 * their kind equalling their index. Beyond them is a pool of program windows —
 * one per launched program, at indices WIN_FIXED.. — that reuse every piece of
 * chrome below (drag, minimise, maximise, close, taskbar) because they live in
 * the very same wins[]/order[] arrays the hit-test and compositing loops walk.
 * WC_PROG_WIN_MAX matches SHELL_PROG_MAX in shell.c: the shell hands out one
 * text slot at a time, and this is where the window that shows it lives. */
#define WIN_FIXED        3
#define WC_PROG_WIN_MAX  6
#define WIN_TOTAL        (WIN_FIXED + WC_PROG_WIN_MAX)

struct window {
    int x, y, w, h;            /* current geometry                     */
    int rx, ry, rw, rh;        /* geometry to restore when un-maximising */
    const char *title;         /* title bar                            */
    const char *task;          /* taskbar button                       */
    int kind;                  /* WIN_STATUS/TRASH/TERM/PROGRAM        */
    int open;                  /* closed windows leave until an icon reopens them */
    int min;                   /* minimised: open, hidden, taskbar only */
    int max;                   /* maximised: fills the desktop          */
    int snap;                  /* SNAP_*: tiled by a drag to a screen edge */
    int slot;                  /* program surface id, or -1 for a fixed window */
    int pid;                   /* the launched program's pid (window marker) */
};

static struct window wins[WIN_TOTAL];
static int order[WIN_TOTAL] = { WIN_STATUS, WIN_TERM, WIN_TRASH,
                                3, 4, 5, 6, 7, 8 };  /* back -> front */
static int dragging, grab_dx, grab_dy, drag_win;
static int resizing, resize_win, resize_mask, resize_changed;
static int prev_left;
static int prev_cursor_mask;   /* last border mask the cursor showed (for the edge) */
static int laid_out;

/* The cursor's previous position, so the box it is leaving is always part of
 * the damage (that is what stops a cursor trail), plus whether a frame has ever
 * been composited (the very first one has no back buffer to update, so it is
 * whole-screen). `prev_menu_open` catches the panel appearing or vanishing. */
static int prev_mx = -1, prev_my = -1;
static int prev_menu_open;
static int painted_once;

#define CURSOR_W 12
#define CURSOR_H 16

/* A frame's damage: a handful of rectangles and the bounding box they union
 * into. The recompose clips to the union (so it redraws only that band), then
 * each rectangle is blitted to the screen. The room is one per window plus the
 * cursor's two boxes and a title strip or two; if a frame ever wants more than
 * fits, dmg_add keeps growing the union but flags the overflow, and the flip
 * then pushes the whole union box as one rect (correct, just blits more). */
struct drect { int x, y, w, h; };
static struct drect dmg[WIN_TOTAL + 4];
static int dmg_n, dmg_overflow, dmg_x0, dmg_y0, dmg_x1, dmg_y1;

static void dmg_add(int W, int H, int x, int y, int w, int h)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > W ? W : x + w;
    int y1 = y + h > H ? H : y + h;
    if (x1 <= x0 || y1 <= y0)
        return;
    if (dmg_n < (int)(sizeof dmg / sizeof dmg[0])) {
        dmg[dmg_n].x = x0; dmg[dmg_n].y = y0;
        dmg[dmg_n].w = x1 - x0; dmg[dmg_n].h = y1 - y0;
        dmg_n++;
    } else {
        /* No room to record it, but the union still has to cover it: the
         * recompose is clipped to the union, so a dropped rect that widened
         * the box is recomposed - the overflow flag makes the flip push the
         * whole box, so it is never left unblitted. */
        dmg_overflow = 1;
    }
    if (x0 < dmg_x0) dmg_x0 = x0;
    if (y0 < dmg_y0) dmg_y0 = y0;
    if (x1 > dmg_x1) dmg_x1 = x1;
    if (y1 > dmg_y1) dmg_y1 = y1;
}


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
    for (int i = 0; i < WIN_TOTAL; i++) {
        if (order[i] != id)
            continue;
        for (int j = i; j < WIN_TOTAL - 1; j++)
            order[j] = order[j + 1];
        order[WIN_TOTAL - 1] = id;
        return;
    }
}

/* The topmost window that is actually on screen. Closed and minimised windows
 * are skipped, so with a pool of program windows — some of them closed — the
 * "front" highlight and the taskbar's toggle still mean what a user sees. -1
 * when nothing is open at all. */
static int focused(void)
{
    for (int i = WIN_TOTAL - 1; i >= 0; i--) {
        struct window *w = &wins[order[i]];
        if (w->open && !w->min)
            return order[i];
    }
    return -1;
}

/* Who should the keyboard go to, in the window manager's opinion: the pid of the
 * program whose window is on top, or 0 for the console.
 *
 * This is half of one rule — *a program keeps the keys only while its window is
 * on top* — and it lives here rather than in the kernel's idle loop because the
 * answer is pure window state: z-order, open, minimised. The other half, that a
 * program must have asked for the keyboard by calling read(0), is the
 * scheduler's business (see keyboard_own()); this function never grants a claim,
 * it only says who is in front, so the idle loop can take the keyboard away from
 * a program the user has simply looked past.
 *
 * A dead program still owns its window until the user closes it — that is how
 * this desktop reports an exit code — so proc_pid_live() is what keeps typing
 * from vanishing into a window whose process is gone. A minimised window is not
 * "on top" in any sense a user would accept, and the fixed windows (status,
 * trash, terminal) have no process to type to: every one of those answers 0,
 * which is the console, which is the prompt. */
int desktop_key_pid(void)
{
    int id = focused();
    if (id < 0)
        return 0;
    struct window *w = &wins[id];
    if (w->kind != WIN_PROGRAM)
        return 0;
    if (!proc_pid_live(w->pid))
        return 0;
    return w->pid;
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
        w->snap = SNAP_NONE;
        klog("wm: window restored");
    }
}

/* Aero Snap: drop a dragged window's title bar near a screen edge and it tiles.
 * The cursor's release point picks the zone — left/right edge for a half, and a
 * top or bottom corner of that edge for a quarter. The pre-snap rect is already
 * in rx/ry/rw/rh (captured when the drag began), so grabbing the title again
 * floats it back. A maximised window is never dragged here. Returns 1 when it
 * re-tiled, which the caller uses to force a full repaint. */
static int try_snap(int id, int mx, int my, int W, int H)
{
    struct window *w = &wins[id];
    int work = H - TASKBAR_H;
    int left  = mx <= SNAP_MARGIN;
    int right = mx >= W - SNAP_MARGIN;
    if (w->max || (!left && !right)) {
        w->snap = SNAP_NONE;          /* dropped in open space: it floats */
        return 0;
    }
    int top = my <= SNAP_MARGIN;
    int bot = my >= work - SNAP_MARGIN;

    w->x = left ? 0 : W / 2;
    w->w = W / 2;
    if (top)      { w->y = 0;      w->h = work / 2; w->snap = left ? SNAP_TL : SNAP_TR; }
    else if (bot) { w->y = work/2; w->h = work / 2; w->snap = left ? SNAP_BL : SNAP_BR; }
    else          { w->y = 0;      w->h = work;     w->snap = left ? SNAP_LEFT : SNAP_RIGHT; }

    klog("wm: snapped %d %dx%d at %d,%d", w->snap, w->w, w->h, w->x, w->y);
    return 1;
}

/* --- painting ---------------------------------------------------------- */

/* Resize cursors. Windows changes the pointer to a double-headed arrow over a
 * draggable border; these are the four directions. A border grab (see
 * resize_edge_at / hover_resize_edge) picks one, so what the cursor shows is
 * exactly what a click there would do. '#' is the dark outline, 'W' the fill,
 * '.' transparent — same encoding as the arrow above. */
static const char *const cursor_ns[] = {
    ".....##.....",
    "....####....",
    "...######...",
    "....####....",
    ".....##.....",
    ".....##.....",
    ".....##.....",
    ".....##.....",
    ".....##.....",
    ".....##.....",
    "....####....",
    "...######...",
    "....####....",
    ".....##.....",
    "............",
    "............",
};

static const char *const cursor_ew[] = {
    "............",
    "............",
    "............",
    "............",
    "............",
    "..#......#..",
    ".#........#.",
    "############",
    ".#........#.",
    "..#......#..",
    "............",
    "............",
    "............",
    "............",
    "............",
    "............",
};

static const char *const cursor_nwse[] = {   /* "\" — top-left / bottom-right  */
    "##..........",
    ".##.........",
    "..##........",
    "...##.......",
    "....##......",
    ".....##.....",
    "......##....",
    ".......##...",
    "........##..",
    ".........##.",
    "..........##",
    "...........#",
    "............",
    "............",
    "............",
    "............",
};

static const char *const cursor_nesw[] = {   /* "/" — top-right / bottom-left  */
    "..........##",
    ".........##.",
    "........##..",
    ".......##...",
    "......##....",
    ".....##.....",
    "....##......",
    "...##.......",
    "..##........",
    ".##.........",
    "##..........",
    "#...........",
    "............",
    "............",
    "............",
    "............",
};

static void draw_cursor(int x, int y, int shape)
{
    const char *const *glyph = cursor_shape;
    if (shape == CUR_NS)        glyph = cursor_ns;
    else if (shape == CUR_EW)   glyph = cursor_ew;
    else if (shape == CUR_NWSE) glyph = cursor_nwse;
    else if (shape == CUR_NESW) glyph = cursor_nesw;

    for (int row = 0; row < CURSOR_H; row++) {
        const char *line = glyph[row];
        for (int col = 0; line[col] != '\0'; col++) {
            if (line[col] == '#')
                gfx_pixel(x + col, y + row, gfx_rgb(0x00, 0x00, 0x00));
            else if (line[col] == 'W')
                gfx_pixel(x + col, y + row, C_WHITE);
        }
    }
}

/* Map a border mask to one of the cursors. A single edge is an axis arrow; a
 * corner is the diagonal it drags along (top-left/bottom-right follow "\",
 * top-right/bottom-left follow "/"). RS_NONE (and any odd combination) is the
 * plain arrow, so the pointer only changes where a click really resizes. */
static int cursor_shape_id(int mask)
{
    switch (mask) {
    case RS_TOP:      case RS_BOTTOM:      case (RS_TOP | RS_BOTTOM):
        return CUR_NS;
    case RS_LEFT:     case RS_RIGHT:       case (RS_LEFT | RS_RIGHT):
        return CUR_EW;
    case (RS_TOP | RS_LEFT):   case (RS_BOTTOM | RS_RIGHT):
        return CUR_NWSE;
    case (RS_TOP | RS_RIGHT):  case (RS_BOTTOM | RS_LEFT):
        return CUR_NESW;
    default:
        return CUR_ARROW;
    }
}

/* One title-bar button, painted from its index. BTN_CLOSE lights the same red
 * the operating system people run shows on hover; the other two are neutral
 * chrome that only lifts a shade when the pointer is over them. */
static void draw_title_button(const struct window *w, int btn, int hover)
{
    int bx = w->x + w->w - 4 - BTN_W * (btn + 1);
    int by = w->y + BTN_DY;
    uint32_t bg;

    if (btn == BTN_CLOSE)
        bg = hover ? C_CLOSE : C_TITLE_HOT;
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

/* Is (mx,my) inside window w and within RESIZE_EDGE pixels of one of its
 * borders? Returns the edge bitmask (a corner sets two bits). The window-body
 * pass never sees RS_TOP (by the time it runs the title bar already owned that
 * row), and the title-bar pass only consults the top band above the buttons, so
 * a grab never clashes with minimise / maximise / close. */
static int resize_edge_at(const struct window *w, int mx, int my)
{
    if (mx < w->x || mx >= w->x + w->w || my < w->y || my >= w->y + w->h)
        return RS_NONE;
    int m = RS_NONE;
    if (mx < w->x + RESIZE_EDGE)         m |= RS_LEFT;
    if (mx >= w->x + w->w - RESIZE_EDGE) m |= RS_RIGHT;
    if (my < w->y + RESIZE_EDGE)         m |= RS_TOP;
    if (my >= w->y + w->h - RESIZE_EDGE) m |= RS_BOTTOM;
    return m;
}

/* Which border, if any, is the pointer hovering? Walk front to back and let the
 * frontmost *visible* window that contains the point decide — the same window a
 * click would act on, so the cursor promises exactly what the click delivers. A
 * maximised window fills the desktop and cannot be resized, so it (and only it)
 * yields the arrow even though it is under the pointer. The title bar offers the
 * top edge only in the thin strip above its buttons; the body offers any border. */
static int hover_resize_edge(int mx, int my)
{
    for (int i = WIN_TOTAL - 1; i >= 0; i--) {
        struct window *w = &wins[order[i]];
        if (!w->open || w->min)
            continue;
        if (mx < w->x || mx >= w->x + w->w || my < w->y || my >= w->y + w->h)
            continue;
        if (w->max)
            return RS_NONE;
        if (my < w->y + TITLE_H) {
            if (my < w->y + RESIZE_EDGE) {
                int m = RS_TOP;
                if (mx < w->x + RESIZE_EDGE)         m |= RS_LEFT;
                if (mx >= w->x + w->w - RESIZE_EDGE) m |= RS_RIGHT;
                return m;
            }
            return RS_NONE;
        }
        return resize_edge_at(w, mx, my);
    }
    return RS_NONE;
}

/* --- the wallpaper -----------------------------------------------------
 * Painted once into a RAM surface and copied from there on every frame.
 *
 * Both halves matter. The surface is what lets the wallpaper be a *picture*
 * rather than one flat gradient: a few million blended pixels at boot costs
 * nothing per frame afterwards, because a frame blits the damage box out of the
 * cache. And the fallback is what makes it safe: a machine whose heap cannot
 * give up width*height*4 bytes paints the gradient again, exactly as it did
 * before this existed, and the desktop does not care which one it got.
 *
 * The design is a bloom: a light source, petals laid across it as translucent
 * ellipses at two angles, and edges that fall away into dark water. It is drawn
 * from these numbers by these loops. It is not anybody's screenshot, which is
 * the only reason it can be in a release. */

static uint32_t *wall;
static int wall_w, wall_h;

/* Digit-by-digit square root in base 2. There is no libm in a freestanding
 * kernel and no FPU state saved for ring 0, and every curve in a wallpaper needs
 * a square root per row — thirty-two shifts, integers only, and exact enough for
 * a pixel. */
static unsigned long long isqrt64(unsigned long long v)
{
    unsigned long long bit = 1ULL << 62;
    unsigned long long r = 0;

    if (v == 0)
        return 0;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else              { r >>= 1; }
        bit >>= 2;
    }
    return r;
}

/* Division that rounds outward instead of toward zero. C's `/` truncates, which
 * is the wrong direction for the two ends of a span: the left root has to go down
 * and the right one up, or a shape loses the pixel whose centre the curve passed
 * through and the edge of every petal comes out one row ragged. */
static long long fdiv(long long num, long long den)
{
    long long q = num / den;                 /* C truncates toward zero */
    if ((num % den) != 0 && ((num < 0) != (den < 0)))
        q--;
    return q;
}

static long long cdiv(long long num, long long den)
{
    return -fdiv(-num, den);
}

static void petal(int cx, int cy, int a, int b, int rot45, uint32_t color, int alpha)
{
    if (a <= 0 || b <= 0)
        return;

    long long aa = (long long)a * a;
    long long bb = (long long)b * b;

    int reach = rot45 ? a + b : b;

    for (int dy = -reach; dy <= reach; dy++) {
        long long x0, x1;

        if (!rot45) {
            long long inner = bb - (long long)dy * dy;
            if (inner < 0)
                continue;
            long long hw = (long long)isqrt64((unsigned long long)((aa * inner) / bb));
            x0 = -hw; x1 = hw;
        } else {
            long long A = aa + bb;
            long long B = 2 * (long long)dy * (bb - aa);
            long long C = (long long)dy * dy * A - 2 * aa * bb;
            long long D = B * B - 4 * A * C;
            if (D < 0)
                continue;
            long long s = (long long)isqrt64((unsigned long long)D);
            /* Outward on both sides: the left root is floored and the right one
             * ceiled, so a row never loses the pixel the real curve passed
             * through the middle of. */
            x0 = fdiv(-B - s, 2 * A);
            x1 = cdiv(-B + s, 2 * A);
        }

        if (x1 < -reach - 1 || x0 > reach + 1)
            continue;
        gfx_blend_rect(cx + (int)x0, cy + dy, (int)(x1 - x0) + 1, 1, color, alpha);
    }
}

/* A glow: the same ellipse again and again, each one a little smaller and a
 * little brighter. Layering low alphas is how a soft light is made out of a
 * primitive that only knows flat rectangles, and at a step of six it reads as a
 * gradient while costing a fraction of a per-pixel distance field. */
static void glow(int cx, int cy, int rx, int ry, uint32_t color, int alpha)
{
    for (int r = rx; r > 0; r -= 6) {
        int rr = (int)((long long)r * ry / rx);
        petal(cx, cy, r, rr > 0 ? rr : 1, 0, color, alpha);
    }
}

static void wall_paint(int W, int H)
{
    wall = gfx_alloc_surface(W, H);
    if (wall == NULL) {
        klog("wm: wallpaper uncached (no surface), gradient only");
        return;
    }
    wall_w = W;
    wall_h = H;

    gfx_set_target(wall, W, H);

    gfx_gradient_v(0, 0, W, H, C_WALL_TOP, C_WALL_BOTTOM);

    /* The light comes up behind the bloom, off-centre and low, the way a lamp
     * behind glass never sits in the middle of the frame. */
    int cx = W * 61 / 100;
    int cy = H * 46 / 100;
    glow(cx, cy, W * 42 / 100, H * 42 / 100, gfx_rgb(0x2E, 0x8E, 0xE6), 5);
    glow(cx, cy, W * 18 / 100, H * 18 / 100, gfx_rgb(0x9F, 0xD8, 0xFF), 6);

    /* Six petals: two crossings at 45 degrees and one upright pair, each with a
     * lighter core drawn over its own body so the ribbons read as translucent
     * where they overlap instead of as a pile of flat shapes. */
    petal(cx, cy, W * 34 / 100, H * 11 / 100, 1, gfx_rgb(0x3C, 0xA8, 0xF0), 70);
    petal(cx, cy, W * 30 / 100, H * 9 / 100, 1, gfx_rgb(0x8E, 0xD4, 0xFF), 46);
    petal(cx, cy, W * 34 / 100, H * 11 / 100, 0, gfx_rgb(0x1F, 0x6F, 0xD0), 78);
    petal(cx, cy, W * 12 / 100, H * 30 / 100, 0, gfx_rgb(0x2B, 0x86, 0xDD), 66);
    petal(cx, cy, W * 9 / 100, H * 26 / 100, 0, gfx_rgb(0x7A, 0xCC, 0xFF), 40);
    petal(cx, cy, W * 6 / 100, H * 6 / 100, 1, gfx_rgb(0xD6, 0xF1, 0xFF), 58);

    /* The corners go dark. Two passes of a linear alpha, one from each side of
     * every edge, and the eye is left where the light is. */
    for (int x = 0; x < W; x++) {
        int d = x < W - x ? x : W - x;
        int a = 70 - d * 70 / (W / 2);
        if (a > 0) {
            gfx_blend_rect(x, 0, 1, H, C_BLACK, a);
        }
    }
    for (int y = 0; y < H; y++) {
        int d = y < H - y ? y : H - y;
        int a = 60 - d * 60 / (H / 2);
        if (a > 0) {
            gfx_blend_rect(0, y, W, 1, C_BLACK, a);
        }
    }

    gfx_set_target(NULL, 0, 0);
    klog("wm: wallpaper cached %dx%d", W, H);
}

/* What a frame costs for the wallpaper: one blit of the damage box out of the
 * cache, or the old gradient when there is no cache. */
static void draw_wallpaper(int W, int H)
{
    if (wall != NULL)
        gfx_blit_surface(wall, 0, 0, wall_w, wall_h, wall_w);
    else
        gfx_gradient_v(0, 0, W, H, C_WALL_TOP, C_WALL_BOTTOM);
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
    ty += 20;
    /* The claim this screen makes about itself, and the one a user cannot test
     * by clicking: a program started here is not a function call, it has a pid,
     * a parent and an exit code. Saying the shell is pid 1 says what the table
     * says — see proc.h, and /bin/whoami for the program that checks it. */
    gfx_text(tx, ty, "CHAQUE PROGRAMME LANCE ICI", C_MUTED, 1);
    ty += 12;
    gfx_text(tx, ty, "EST UN PROCESSUS. LE SHELL", C_MUTED, 1);
    ty += 12;
    gfx_text(tx, ty, "EST PID 1 : TU NE PEUX PAS", C_MUTED, 1);
    ty += 12;
    gfx_text(tx, ty, "LE TUER.", C_MUTED, 1);
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

static void draw_window(const struct window *w, int focus)
{
    int mx = mouse_x();
    int my = mouse_y();
    int front = (focus >= 0 && w == &wins[focus]);
    int console = (w->kind == WIN_TERM || w->kind == WIN_PROGRAM);
    uint32_t border = console ? gfx_rgb(0x2B, 0x36, 0x48)
                              : gfx_rgb(0xCE, 0xD5, 0xE0);

    /* Shadow only when there is room behind the window: a maximised one
     * has no behind, and the extra pixels would poke into the taskbar. It is a
     * blend, not a navy slab, so it reads as a soft drop over the wallpaper the
     * way an overlapping window does on the desktop people actually use. */
    if (!w->max)
        gfx_blend_rect(w->x + 5, w->y + 6, w->w, w->h, C_BLACK, A_SHADOW);

    /* Windows 11 corners: a rounded body with a one-pixel edge that follows the
     * round, and a title strip whose top corners match. A maximised window gets
     * r = 0, which every one of these primitives already treats as a plain fill,
     * so a full-screen window keeps its square edges without a single special
     * case in the drawing code. */
    int r = w->max ? 0 : WIN_RADIUS;
    gfx_round_rect(w->x, w->y, w->w, w->h, r, border);
    gfx_round_rect(w->x + 1, w->y + 1, w->w - 2, w->h - 2, r ? r - 1 : 0,
                   console ? C_CONSOLE : C_BODY);
    gfx_round_top(w->x + 1, w->y + 1, w->w - 2, TITLE_H - 1, r ? r - 1 : 0,
                  front ? C_TITLE_HOT : C_TITLE);
    gfx_fill(w->x + 1, w->y + r + 1, 3, TITLE_H - r - 2,
             front ? C_ACCENT : gfx_rgb(0x47, 0x55, 0x69));

    gfx_text(w->x + 16, w->y + 10, w->title, C_WHITE, 2);
    for (int btn = BTN_MIN; btn <= BTN_CLOSE; btn++)
        draw_title_button(w, btn,
                          title_button_at(w, mx, my) == btn && front);

    if (w->kind == WIN_STATUS)
        draw_status_content(w);
    else if (w->kind == WIN_TRASH)
        draw_trash_content(w);
    else if (w->kind == WIN_PROGRAM)
        shell_program_draw(w->slot, w->x + 14, w->y + TITLE_H + 12, w->w - 28);
    else
        shell_draw(w->x + 14, w->y + TITLE_H + 12, w->w - 28);
}

/* --- start menu -------------------------------------------------------- */

#define MENU_X        START_X
#define MENU_W        236
#define MENU_HEADER_H 30
#define MENU_ITEM_H   22
#define MENU_FOOTER_H 26
#define MENU_GAP      6          /* air between the panel and the taskbar */
#define MENU_MAX      32         /* rows the array can hold; menu_scan() also
                                  * caps that by what the screen has room for.
                                  * It has to stay above the number of programs
                                  * in /bin: tools/local-test.sh requires a row
                                  * for every USER_BINS entry and `hidden 0`, so
                                  * a menu that quietly drops a program is a
                                  * failing test rather than a silent one. On a
                                  * 1024x768 panel the screen itself fits 29 rows
                                  * of MENU_ITEM_H, and menu_scan() will never
                                  * draw more than the screen can reach — this
                                  * ceiling only bounds the array, with slots
                                  * spare so the next viewer still needs no
                                  * desktop change. */
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

    /* Acrylic, not a slab: the bar lets the cached wallpaper through at a low
     * alpha instead of hiding it behind flat charcoal, which is the single thing
     * that separates a taskbar that reads as this desktop's from one that reads
     * as a grey strip laid over it. */
    gfx_blend_rect(0, y, W, TASKBAR_H, C_TASKBAR, A_TASKBAR);
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
    for (int id = 0; id < WIN_TOTAL; id++) {
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

    /* The tray clock is the CMOS time, read fresh, not a string that was true
     * the day the desktop was written. When the chip does not answer the tray
     * shows dashes - a clock with nothing behind it is not allowed to invent
     * 12:00 and wear it as if it were the hour. */
    int hh, mm;
    rtc_now(&hh, &mm, 0);
    char clk[6];
    if (hh >= 0 && mm >= 0) {
        clk[0] = (char)('0' + hh / 10); clk[1] = (char)('0' + hh % 10);
        clk[2] = ':';
        clk[3] = (char)('0' + mm / 10); clk[4] = (char)('0' + mm % 10);
    } else {
        clk[0] = clk[1] = clk[3] = clk[4] = '-'; clk[2] = ':';
    }
    clk[5] = '\0';
    gfx_text(W - 56, y + 17, clk, C_WHITE, 1);
}

/* --- input ------------------------------------------------------------- */

static int task_button_at(int mx, int my, int H)
{
    if (my < H - TASKBAR_H || my >= H)
        return -1;
    int x = TASK_X;
    for (int id = 0; id < WIN_TOTAL; id++) {
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
    for (int i = 0; i < WIN_TOTAL; i++)
        if (wins[i].open && !wins[i].min)
            anything_shown = 1;

    for (int i = 0; i < WIN_TOTAL; i++)
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

        /* Copy the name before launching. shell_launch() is now asynchronous —
         * it places the process in the table and returns while it still runs —
         * so the panel is dismissed here rather than waiting on a program that
         * may not be finished for a while. The copy keeps this row's name safe
         * regardless: a repaint during the run can rebuild the menu, and the
         * launch must carry the exact row the user clicked, not a later one. */
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
    for (int i = WIN_TOTAL - 1; i >= 0; i--) {
        int id = order[i];
        struct window *w = &wins[id];
        if (!w->open || w->min)
            continue;
        if (mx < w->x || mx >= w->x + w->w ||
            my < w->y || my >= w->y + TITLE_H)
            continue;

        raise_window(id);
        /* The thin strip at the very top of the title bar resizes the window's
         * height (or a top corner, adding the horizontal bit). It sits above
         * BTN_DY, so it never overlaps the minimise / maximise / close buttons;
         * below it the title bar still drags, and the buttons still act. */
        if (!w->max && my < w->y + RESIZE_EDGE) {
            int m = RS_TOP;
            if (mx < w->x + RESIZE_EDGE)         m |= RS_LEFT;
            if (mx >= w->x + w->w - RESIZE_EDGE) m |= RS_RIGHT;
            resizing = 1;
            resize_win = id;
            resize_mask = m;
            resize_changed = 0;
            return;
        }
        switch (title_button_at(w, mx, my)) {
        case BTN_CLOSE:
            /* A program window is more than a rectangle: closing it frees the
             * text slot behind it (shell.c) so the six-window pool has room
             * again, and says which program's window went away. The process is
             * not touched — dismissing a window is not a signal. Fixed windows
             * just hide until an icon or the taskbar brings them back. */
            if (w->kind == WIN_PROGRAM) {
                serial_write("wm: program window closed '");
                serial_write(w->title);
                serial_write("' pid ");
                serial_write_dec((uint64_t)w->pid);
                serial_write("\n");
                shell_program_close(w->slot);
                w->slot = -1;
                w->pid = 0;
            }
            w->open = 0; w->min = 0; w->max = 0; w->snap = SNAP_NONE;
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
        /* Remember where to restore if this drag ends in a snap — but keep the
         * existing rect when the window is already snapped, so grabbing a tiled
         * title detaches it and a later snap still restores to the original
         * floating spot, not to the tiled size. */
        if (!w->snap) {
            w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
        }
        w->snap = SNAP_NONE;
        dragging = 1;
        drag_win = id;
        grab_dx = mx - w->x;
        grab_dy = my - w->y;
        return;
    }

    /* Window bodies: clicking one raises it. Focus without a drag, which
     * is what makes two overlapping windows usable at all. A press within
     * RESIZE_EDGE pixels of a border instead starts a resize - the grab raises
     * the window too, then the frame loop tracks the cursor. The title-bar
     * loop above already owns everything inside TITLE_H, so the left/right/
     * bottom bands that reach here never clash with a drag or a button. */
    for (int i = WIN_TOTAL - 1; i >= 0; i--) {
        int id = order[i];
        struct window *w = &wins[id];
        if (!w->open || w->min)
            continue;
        if (mx < w->x || mx >= w->x + w->w ||
            my < w->y || my >= w->y + w->h)
            continue;
        raise_window(id);
        int edge = w->max ? RS_NONE : resize_edge_at(w, mx, my);
        if (edge) {
            resizing = 1;
            resize_win = id;
            resize_mask = edge;
            resize_changed = 0;
        }
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

    /* The program-window pool starts empty. `slot = -1` marks an unused entry
     * (0 is a real surface id, so it cannot be the default of a zeroed array);
     * they are closed until wm_open_program() gives one a geometry, a name and a
     * pid. Being closed, they are skipped by every hit-test and paint loop. */
    for (int i = WIN_FIXED; i < WIN_TOTAL; i++) {
        wins[i].kind  = WIN_PROGRAM;
        wins[i].open  = 0;
        wins[i].slot  = -1;
        wins[i].pid   = 0;
        wins[i].title = "";
        wins[i].task  = "";
    }

    for (int i = 0; i < WIN_TOTAL; i++) {
        wins[i].rx = wins[i].x;  wins[i].ry = wins[i].y;
        wins[i].rw = wins[i].w;  wins[i].rh = wins[i].h;
        wins[i].snap = SNAP_NONE;
    }

    /* Paint the wallpaper once into its own surface, now that the screen size
     * is known. Every frame from here blits a rectangle out of that cache
     * instead of redrawing a few million blended pixels, which is the only
     * reason a desktop this decorated is also a desktop cheap enough to repaint
     * on every mouse packet. layout() runs a single time, so this costs the boot
     * one pass and the running system nothing extra per frame. */
    wall_paint(W, H);

    laid_out = 1;
}

/* A program window's body is a 62-column scrollback (see SHELL_COLS), so it
 * needs that much width plus margins; short enough to leave room to cascade. */
#define PROG_W 500
#define PROG_H 240

/* Open a window for a program the shell just launched: claim an unused entry in
 * the pool, cascade it so several are tellable apart, bind it to the shell's
 * text slot and the program's pid, and raise it. From here the compositor does
 * the rest — this window is drawn, dragged, minimised, maximised and closed
 * exactly like a fixed one, because it lives in the same arrays. It also
 * *outlives* the process: shell_program_pump() finalises it (exit code, the
 * "finished" line) when the program stops, and only its close button removes it. */
void wm_open_program(int slot, int pid)
{
    if (!gfx_ready() || slot < 0)
        return;

    int id = -1;
    for (int i = WIN_FIXED; i < WIN_TOTAL; i++)
        if (wins[i].slot == -1) { id = i; break; }
    if (id < 0)
        return;                    /* pool full; shell.c already logged the fallback */

    int W = gfx_width();
    int H = gfx_height();
    int n = id - WIN_FIXED;
    struct window *w = &wins[id];

    w->w = PROG_W;  w->h = PROG_H;
    if (w->w > W - 40) w->w = W - 40;
    w->x = 150 + n * 24;
    w->y = 56 + n * 22;
    clamp_to_desktop(w, W, H);
    w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
    w->title = shell_program_name(slot);
    w->task  = shell_program_name(slot);
    w->kind  = WIN_PROGRAM;
    w->slot  = slot;
    w->pid   = pid;
    w->open  = 1;
    w->min   = 0;
    w->max   = 0;
    w->snap  = SNAP_NONE;
    raise_window(id);
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

    /* Release closes whichever gesture was live. A resize logs its final
     * geometry once - and only if the size actually changed, so a stray grab
     * with no move is silent. A drag that ends on a screen edge snaps. */
    int snap_now = 0;
    if (!left && (dragging || resizing)) {
        if (resizing && resize_changed)
            klog("wm: window resized %dx%d",
                 wins[resize_win].w, wins[resize_win].h);
        if (dragging && !resizing)
            snap_now = try_snap(drag_win, mx, my, W, H);
        dragging = 0;
        resizing = 0;
        resize_mask = 0;
        resize_changed = 0;
    }

    if (dragging) {
        struct window *w = &wins[drag_win];
        w->x = mx - grab_dx;
        w->y = my - grab_dy;
        clamp_to_desktop(w, W, H);
    }

    /* Resize: the grabbed edges follow the cursor, the opposite edges stay put.
     * A left/top grab moves x/y and keeps the right/bottom fixed; right/bottom
     * keep x/y. Floors stop the window shrinking past usable, and the desktop
     * clamp keeps it on screen. A maximised window never reaches here. */
    if (resizing) {
        struct window *w = &wins[resize_win];
        int right = w->x + w->w;    /* fixed edge a left-drag preserves */
        int bottom = w->y + w->h;   /* fixed edge a top-drag preserves  */
        int nx = w->x, ny = w->y, nw = w->w, nh = w->h;
        if (resize_mask & RS_LEFT)   { nx = mx; nw = right - mx; }
        if (resize_mask & RS_RIGHT)  { nw = mx - w->x; }
        if (resize_mask & RS_TOP)    { ny = my; nh = bottom - my; }
        if (resize_mask & RS_BOTTOM) { nh = my - w->y; }
        /* floors: a left/top drag holds its fixed edge when it hits MIN */
        if (nw < MIN_W) { if (resize_mask & RS_LEFT) nx = right - MIN_W; nw = MIN_W; }
        if (nh < MIN_H) { if (resize_mask & RS_TOP)  ny = bottom - MIN_H; nh = MIN_H; }
        /* the grabbed top/left edge cannot pass the screen origin */
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (resize_mask & RS_LEFT)  nw = right - nx;   /* recompute vs fixed edge */
        if (resize_mask & RS_TOP)   nh = bottom - ny;
        /* clamp to the desktop, then re-assert the floor */
        if (nx + nw > W)                nw = W - nx;
        if (ny + nh > H - TASKBAR_H)    nh = H - TASKBAR_H - ny;
        if (nw < MIN_W) nw = MIN_W;
        if (nh < MIN_H) nh = MIN_H;
        if (nx != w->x || ny != w->y || nw != w->w || nh != w->h) {
            w->x = nx; w->y = ny; w->w = nw; w->h = nh;
            resize_changed = 1;
        }
    }

    if (press)
        handle_press(mx, my, W, H);

    /* Border under the pointer now that drag/resize/press may have moved a
     * window's edges. Say so only on a change, so hovering a border does not
     * spam the log, and keep the arrow while the start menu covers the click. */
    int cur_mask = menu_open ? RS_NONE : hover_resize_edge(mx, my);
    if (cur_mask != prev_cursor_mask) {
        klog("wm: cursor resize %d", cur_mask);
        prev_cursor_mask = cur_mask;
    }

    /* Decide what changed since the last flip. A structural change - the first
     * frame, a click, a drag, the menu opening or closing, or a machine with no
     * back buffer to update - repaints the whole desktop. Everything else is a
     * small set of damage rectangles: the band the cursor swept, any title strip
     * it hovered, the terminal body if its text moved, and each program window
     * whose own output changed. A running program used to force a full-screen
     * flip; now it dirties only its window's rect. */
    int full = !painted_once
            || press
            || dragging
            || resizing
            || snap_now
            || (menu_open != prev_menu_open)
            || !gfx_has_backbuffer();

    dmg_n = 0;
    dmg_overflow = 0;
    if (!full) {
        dmg_x0 = dmg_y0 = 1 << 30;
        dmg_x1 = dmg_y1 = 0;
        dmg_add(W, H, mx, my, CURSOR_W, CURSOR_H);
        if (prev_mx >= 0)
            dmg_add(W, H, prev_mx, prev_my, CURSOR_W, CURSOR_H);

        /* A title-bar button lights on hover, so when the pointer is in (or has
         * just left) a window's title strip, that strip is damaged too - the
         * hover highlight would otherwise paint over a stale button. */
        for (int i = 0; i < WIN_TOTAL; i++) {
            struct window *w = &wins[i];
            if (!w->open || w->min)
                continue;
            int over_new = mx >= w->x && mx < w->x + w->w &&
                           my >= w->y && my < w->y + TITLE_H;
            int over_prev = prev_mx >= w->x && prev_mx < w->x + w->w &&
                            prev_my >= w->y && prev_my < w->y + TITLE_H;
            if (over_new || over_prev)
                dmg_add(W, H, w->x, w->y, w->w, TITLE_H);
        }

        /* The terminal's text changed: its body is the damage. */
        if (shell_dirty())
            dmg_add(W, H, wins[WIN_TERM].x, wins[WIN_TERM].y,
                    wins[WIN_TERM].w, wins[WIN_TERM].h);

        /* Each program window whose own surface gained output since the last
         * paint. This is the whole point of the release: a running program used
         * to force a full-screen flip, and now it dirties only its window's rect
         * - the clipped recompose redraws the layer stack over that box, so a
         * window partly hidden behind another is still composited correctly. A
         * minimized window is skipped (nothing visible changed; its restore is a
         * click, which repaints full), and a maximized one damages the desktop. */
        for (int i = WIN_FIXED; i < WIN_TOTAL; i++) {
            struct window *w = &wins[i];
            if (w->open && !w->min && w->kind == WIN_PROGRAM &&
                shell_program_dirty_slot(w->slot))
                dmg_add(W, H, w->x, w->y, w->w, w->h);
        }

        gfx_set_clip(dmg_x0, dmg_y0, dmg_x1 - dmg_x0, dmg_y1 - dmg_y0);
    } else {
        gfx_reset_clip();
    }

    /* Recompose the whole layer stack, but clipped: the primitives only write
     * pixels inside the box above, so this is cheap for a cursor move and exact
     * for a full repaint, and it recomputes the scene *over* wherever the cursor
     * used to be, so no trail survives. */
    draw_wallpaper(W, H);
    for (int i = 0; i < (int)(sizeof icons / sizeof icons[0]); i++)
        draw_icon(icons[i].label, icons[i].y);

    int focus = focused();
    for (int i = 0; i < WIN_TOTAL; i++) {
        struct window *w = &wins[order[i]];
        if (w->open && !w->min)
            draw_window(w, focus);
    }

    draw_taskbar(W, H);
    if (menu_open)
        draw_menu(H);                  /* above the windows: it asked for them */
    draw_cursor(mx, my, cursor_shape_id(cur_mask));

    /* Push the recomputed region to the display in one pass, and say how big
     * that region was - the honest number the tests read. A structural frame
     * flips the whole screen; otherwise each damage rect is blitted, and a frame
     * that wanted more rects than fit pushes the whole union box instead (the
     * recompose already covered it, and bb outside the box is last frame's
     * unchanged content, so one big rect is correct - just blits more). */
    uint64_t area;
    int n;
    if (full) {
        area = gfx_flip();
        n = 1;
    } else if (dmg_overflow) {
        area = gfx_flip_rect(dmg_x0, dmg_y0, dmg_x1 - dmg_x0, dmg_y1 - dmg_y0);
        n = 1;
    } else {
        area = 0;
        n = dmg_n;
        for (int i = 0; i < dmg_n; i++)
            area += gfx_flip_rect(dmg[i].x, dmg[i].y, dmg[i].w, dmg[i].h);
    }
    gfx_reset_clip();

    serial_write("gfx: flip ");
    serial_write_dec((uint64_t)n);
    serial_write(" rects, ");
    serial_write_dec(area);
    serial_write(" px\n");

    prev_mx = mx;
    prev_my = my;
    prev_menu_open = menu_open;
    painted_once = 1;
}

