/* WindClone — command line: line editor, built-ins, and program launcher.
 *
 * Everything here runs from the idle loop, never from an IRQ: the keyboard
 * driver only queues bytes (see keyboard.c), so a `cat` that walks the initrd
 * or a program that enters ring 3 cannot deadlock an interrupt.
 *
 * The scrollback is a fixed grid of characters, not a framebuffer: the
 * window can be repainted (dragged, overlapped) as often as it likes and
 * the text is still there. That is the same reason a real terminal keeps
 * cells and not pixels.
 *
 * Most commands are no longer here. `ls`, `cat`, `echo`, `hello` are ELF
 * programs in /bin, launched with their arguments and given this grid as
 * their stdout — which is the whole reason the sink below exists: a program
 * that can only talk to the serial port is a program nobody can use.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>

#include "shell.h"
#include "gfx.h"
#include "kmalloc.h"
#include "usermode.h"
#include "proc.h"
#include "vfs.h"
#include "keyboard.h"
#include "serial.h"
#include "string.h"
#include "timer.h"
#include "version.h"
#include "desktop.h"
#include "net.h"

#define SHELL_ROWS 17
#define SHELL_COLS 62
#define LINE_H     11
#define GLYPH_W    6              /* 5 columns of font + 1 of spacing (gfx.c) */

/* One scrollback grid, and the code that writes and paints it, lifted out of
 * the terminal so it can be shared. The terminal is one instance; every
 * launched program gets another (see `struct program` below). Text stays cells
 * rather than pixels for the reason a real terminal does: a window can be
 * dragged, overlapped and repainted as often as it likes and the scrollback is
 * still there.
 *
 * The colour escape: a program writes FS (0x1C) followed by one digit 0..7 and
 * every glyph after that is painted in palette[index] until it says otherwise.
 * It is in-band for the same reason the form feed is (see surf_write): a byte
 * that is not a glyph, already on the wire the program is writing, so a
 * highlighter needs no syscall and no new ABI. FS is what ASCII reserved for
 * exactly this — "file separator", a control byte no document text wants. */
#define COLOR_ESC    0x1C
#define COLOR_PALETTE 8

/* The pixel escape, and the same reasoning as FS above: a program that has
 * pictures to show — a decoded image, a chart — has no way to say so through a
 * text grid, and a syscall for it would be a new ABI to freeze forever. So the
 * request travels on the wire it already owns. A program writes RS (0x1E, "record
 * separator"), then 'R', then four hex digits — `WW HH` in pixels — and every
 * byte after that is one RGB24 pixel, left to right, top to bottom, until W*H
 * have arrived. The buffer belongs to the kernel and not to the program, so
 * a picture costs a window 40 KiB of RAM and not a fifth of the program's own
 * 256 KiB image, and the pixel bytes are read straight off the write the way the
 * text path already reads its glyphs — same context, same user pages. */
#define RASTER_ESC    0x1E
#define RASTER_MAX_W  128
#define RASTER_MAX_H  80

/* One row of the grid, and the colour each of its cells was painted in. The
 * attribute is a palette index and not an RGB: 17x62 cells per window, six
 * windows and one terminal, and a uint32 per cell would cost more RAM than the
 * text itself while a program can only ever name one of eight inks anyway. */
struct surface {
    char    rows[SHELL_ROWS][SHELL_COLS + 1];
    uint8_t attr[SHELL_ROWS][SHELL_COLS];
    int     next_row;                   /* where the next line goes        */
    char    pending[SHELL_COLS + 1];     /* the line still being assembled  */
    uint8_t pending_attr[SHELL_COLS];
    int     pending_len;
    int     fg;                          /* the pen the next glyph lands in */
    int     awaiting_attr;               /* 1 = the next byte is a digit    */
    int     saw_color;                   /* 1 = a complete FS+digit pair was
                                          * consumed since the last check. It is
                                          * the surface that knows a colour was
                                          * asked for, not the byte stream: a
                                          * lone FS is a program that has not
                                          * finished speaking, and logging the
                                          * first byte would claim a pen move
                                          * that has not happened. */
    int     dirty;                       /* this surface changed since paint */

    /* --- a decoded picture, owned by the kernel ----------------------
     * See RASTER_ESC above. These are NULL/zero for every surface that never
     * receives a pixel record — the terminal, and every text program — so the
     * whole feature costs a pointer and four ints until a program asks for it.
     * `raster` points at a kmalloc'd w*h array of 0xRRGGBB pixels. */
    uint32_t *raster;
    int      rw, rh;                     /* picture dimensions, in pixels      */
    int      rcnt;                       /* pixels stored so far               */
    int      rwant;                      /* w*h: the record is done at rcnt     */
    int      rstage;                     /* 0 none, 1 saw RS (next is 'R'),
                                          * 3 reading the 4 hex digits,
                                          * 4 collecting pixel bytes            */
    char     rhex[8];                    /* the WW HH header, half-parsed       */
    int      rhn;                        /* hex digits collected so far         */
    uint8_t  rpix[3];                    /* an RGB24 pixel being assembled      */
    int      rpixn;                      /* bytes of rpix filled so far         */
    int      saw_image;                  /* 1 = a whole picture just landed; the
                                          * console router logs it once, the same
                                          * way it does a completed colour escape*/
};

static struct surface term;           /* the shell's own window */

/* The eight inks a program may name. Index 0 is the green every cell starts in
 * — the same 0x22C55E this OS has drawn its text with since the first window —
 * so a program that never mentions a colour paints exactly the screen it
 * painted before this existed. The others are the roles a code viewer needs,
 * named for what they are used for rather than for their hue: a keyword, a
 * string, a number, a comment. Written as literals because gfx_rgb() is a
 * function and a table has to be a constant; the values are that function's
 * output for the (r,g,b) triples in the comments. */
static const uint32_t color_ink[COLOR_PALETTE] = {
    0x22C55E,   /* 0 default: the terminal's green   */
    0xF8FAFC,   /* 1 white:   plain text, emphasized */
    0x38BDF8,   /* 2 cyan:    keywords               */
    0xFDE68A,   /* 3 yellow:  strings                */
    0xF472B6,   /* 4 magenta: numbers                */
    0xEF4444,   /* 5 red:     a name that failed     */
    0xA78BFA,   /* 6 violet:  types                  */
    0x64748B,   /* 7 dim:     comments               */
};

static char input[SHELL_COLS + 1];
static int  input_len;

/* --- surface helpers -------------------------------------------------- */

static void surf_clear(struct surface *s)
{
    /* A fresh screen drops any picture with it: a form feed means "this is the
     * whole frame now", and a leftover raster would keep painting under a
     * program that has moved on to words. The buffer is the kernel's, so this is
     * the one place it comes back. */
    if (s->raster != NULL) {
        kfree(s->raster);
        s->raster = NULL;
    }
    s->rw = s->rh = s->rcnt = s->rwant = 0;
    s->rstage = s->rhn = s->rpixn = 0;
    s->saw_image = 0;

    for (int r = 0; r < SHELL_ROWS; r++) {
        memset(s->rows[r], 0, sizeof s->rows[r]);
        memset(s->attr[r], 0, sizeof s->attr[r]);
    }
    s->next_row = 0;
    s->pending_len = 0;
    /* The pen is deliberately not reset: a form feed says "this is a screen",
     * not "start over in green". A program that paints a frame sets its own
     * colours row by row, and a pen that survives the clear is the simpler
     * contract to write a frame against. */
    s->dirty = 1;
}

static void surf_scroll(struct surface *s)
{
    for (int r = 1; r < SHELL_ROWS; r++) {
        memcpy(s->rows[r - 1], s->rows[r], SHELL_COLS + 1);
        memcpy(s->attr[r - 1], s->attr[r], SHELL_COLS);
    }
    memset(s->rows[SHELL_ROWS - 1], 0, SHELL_COLS + 1);
    memset(s->attr[SHELL_ROWS - 1], 0, SHELL_COLS);
    s->next_row = SHELL_ROWS - 1;
}

/* The 5x7 font has no lowercase: fold on the way into the grid so the
 * scrollback is already in the form the screen will show. */
static char upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

/* Commit one finished row. `attrs` is the colour of each cell, or NULL for the
 * default: every kernel-written line (banner, echo, help, the prompt) passes
 * NULL and lands in green, which is why this can be an optional argument rather
 * than a second writer to keep in sync. */
static void surf_row(struct surface *s, const char *str, const uint8_t *attrs)
{
    if (s->next_row >= SHELL_ROWS)
        surf_scroll(s);

    int c = 0;
    for (; str[c] != '\0' && c < SHELL_COLS; c++) {
        s->rows[s->next_row][c] = upper(str[c]);
        s->attr[s->next_row][c] = attrs ? attrs[c] : 0;
    }
    s->rows[s->next_row][c] = '\0';
    s->next_row++;
    s->dirty = 1;
}

static void surf_put_line(struct surface *s, const char *str)
{
    surf_row(s, str, NULL);
}

/* "TEXT" + decimal, without dragging printf in. */
static void surf_put_line_num(struct surface *s, const char *prefix,
                              uint64_t value)
{
    char buf[SHELL_COLS + 1];
    int n = 0;

    for (; prefix[n] != '\0' && n < SHELL_COLS - 20; n++)
        buf[n] = prefix[n];

    char digits[21];
    int d = 0;
    do {
        digits[d++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0 && d < 20);
    while (d > 0 && n < SHELL_COLS)
        buf[n++] = digits[--d];

    buf[n] = '\0';
    surf_put_line(s, buf);
}

/* "EXIT -9" into a program's surface: signed, because a killed process is a
 * whole reason a window exists and a minus sign must survive to the screen. */
static void surf_put_exit(struct surface *s, int code)
{
    char buf[24];
    int n = 0;
    for (const char *p = "EXIT "; *p != '\0'; p++)
        buf[n++] = *p;

    unsigned u;
    if (code < 0) {
        buf[n++] = '-';
        u = (unsigned)(-code);
    } else {
        u = (unsigned)code;
    }
    char dig[12];
    int d = 0;
    do {
        dig[d++] = (char)('0' + (u % 10));
        u /= 10;
    } while (u != 0 && d < 12);
    while (d > 0)
        buf[n++] = dig[--d];

    buf[n] = '\0';
    surf_put_line(s, buf);
}

/* Paint a surface's rows at (x,y). Shared by the terminal and every program
 * window; neither knows the other's cells.
 *
 * Text is drawn in runs of equal attribute rather than one glyph at a time:
 * gfx_text() is a loop over the font and a call costs what a call costs, so
 * painting "int main" as two runs (cyan, then green) is cheaper than painting
 * it as six glyphs of two inks, and it is exactly what the row means. The cell
 * columns map to pixels through GLYPH_W because the font is a fixed 5+1 grid —
 * the same reason a terminal can put the caret at a character. */
static void surf_draw(const struct surface *s, int x, int y)
{
    /* A completed picture goes up first, as a plain copy of the kernel's RGB
     * buffer into the window body; any text the program wrote lands under it.
     * Only a *whole* record blits — a half-received one is left as text so a
     * torn frame never shows a band of garbage. */
    int yoff = 0;
    if (s->raster != NULL && s->rw > 0 && s->rh > 0 && s->rcnt >= s->rwant) {
        gfx_blit_surface(s->raster, x, y, s->rw, s->rh, s->rw);
        yoff = s->rh + 4;
    }

    for (int r = 0; r < SHELL_ROWS; r++) {
        if (s->rows[r][0] == '\0')
            continue;

        int n = 0;
        while (s->rows[r][n] != '\0' && n < SHELL_COLS)
            n++;

        int c = 0;
        while (c < n) {
            uint8_t a = s->attr[r][c];
            int start = c;
            while (c < n && s->attr[r][c] == a)
                c++;

            char run[SHELL_COLS + 1];
            int len = c - start;
            for (int i = 0; i < len; i++)
                run[i] = s->rows[r][start + i];
            run[len] = '\0';

            gfx_text(x + start * GLYPH_W, y + yoff + r * LINE_H, run, color_ink[a], 1);
        }
    }
}

/* The terminal's own line writer: every banner, echo, help and prompt line in
 * this file still calls these, they just route into the terminal surface. */
static void put_line(const char *str)
{
    surf_put_line(&term, str);
}

static void put_line_num(const char *prefix, uint64_t value)
{
    surf_put_line_num(&term, prefix, value);
}


/* --- commands --------------------------------------------------------- */

static void cmd_help(void)
{
    put_line("COMMANDES:");
    put_line("  HELP           CETTE LISTE");
    put_line("  ABOUT          VERSION ET UPTIME");
    put_line("  CLEAR          EFFACE L'ECRAN");
    put_line("  BAT <NOM>      EXECUTE /TMP/<NOM>.BAT");
    put_line("  SET [N=V]      DEFINIT OU LISTE UNE VARIABLE DE SESSION");
    put_line("  TYPE <CHEMIN>  AFFICHE UN FICHIER DU STOCKAGE OU DE L'ARCHIVE");
    put_line("  FETCH <CHEMIN> NAVIGUE : GET <CHEMIN> -> /NET/FETCHED");
    put_line("  DIR            LISTE LE STOCKAGE PUIS L'ARCHIVE");
    put_line("  DEL <CHEMIN>   SUPPRIME UN FICHIER ECRIVABLE");
    put_line("  REN <A> <B>    RENOMME UN FICHIER ECRIVABLE");
    put_line("  MOVE <A> <B>   DEPLACE (ECRASE LA DESTINATION, COMME CMD)");
    put_line("  - PROGRAMMES DANS /BIN -");
    put_line("  LS             LISTE L'INITRD");
    put_line("  CAT <FICHIER>  AFFICHE UN FICHIER");
    put_line("  ECHO <MOTS>    RENVOIE SES ARGUMENTS");
    put_line("  HELLO          UN PROGRAMME QUI PARLE EN RING 3");
    put_line("  MOTD           LIT /ETC/MOTD VIA LE VFS");
    put_line("  WHOAMI         MON PID - ON NE TUE PAS PID 1");
    put_line("  REAPER         ENFANT, KILL, WAIT - DANS CET ORDRE");
    put_line("  SAVE           ECRIT OU LIT UN FICHIER DANS /TMP");
    put_line("  COPY <A> <B>   DUPLIE UN FICHIER OCTET PAR OCTET");
    put_line("  FC <A> <B>     COMPARE DEUX FICHIERS (COMME CMD)");
    put_line("  FIND /I /N <CH> <F>  CHERCHE UN TEXTE (COMME CMD)");
    put_line("  NOTE <NOM>     EDITE /TMP/<NOM> (OU <NOM>.TXT)");
    put_line("  VIEW <FICHIER> AFFICHE UN FICHIER EN COULEUR");
    put_line("  PKG            MONTRE LA PAGE RECUPEREE /NET/FETCHED");
    put_line("  BROWSER        BARRE D'ADRESSE + LA PAGE RECUPEREE");
    put_line("  UNZIP <A> <D>  DECOMPRESSE UNE ARCHIVE STOCKEE DANS <D>");
    put_line("  IMGVIEW <F>    AFFICHE UN BITMAP 24 BITS DANS LA FENETRE");
    put_line("CE QUE TU LANCES ICI EST UN PROCESSUS: PID REEL,");
    /* The shell's pid is printed as the number the table holds, not as a "1"
     * written into this file: the claim on the screen is then whatever the
     * kernel actually did, and a boot that handed pid 1 to somebody else
     * shows up as a different number instead of a lie. */
    put_line_num("PARENT = LE SHELL, PID ", (uint64_t)proc_shell_pid());
}

/* --- a program's own window ------------------------------------------
 * A launched program gets a surface of its own, bound to the pid the shell was
 * handed. The console router below sends a *running* program's write(1) bytes
 * into that surface instead of the shared terminal, so the program truly owns
 * its window: the compositor paints it, the user drags / minimises / closes it,
 * and it stays on screen after the process is reaped — showing the exit code the
 * process left and its final output — until the user closes it. A program's own
 * children (a reaper's bell) were never launched by a click or a word, so their
 * output still lands in the terminal: that is the honest edge of "a launched
 * program gets a window", not "every process does".
 *
 * A bound pid is collision-proof because pids are never reused (proc.c's
 * next_pid only climbs), so a window can key itself to one forever. */
#define SHELL_PROG_MAX 6

struct program {
    int      used;           /* slot holds a window (live or finished)   */
    int      live;           /* 1 while its process still owns a CPU     */
    int      finished;       /* 1 once the process stopped               */
    int      pid;
    int      exit_code;
    uint64_t bytes;          /* output routed to this window (the claim) */
    int      routed_logged;  /* emit "wm: routed output" only once       */
    int      screen_logged;  /* emit "wm: screen cleared" only once. A program
                              * that repaints says it every keystroke, and a log
                              * line per frame would bury the one fact worth
                              * knowing: that \f reaches a window at all.        */
    int      color_logged;   /* emit "wm: colour" only once, on the same rule:
                              * the fact worth testing is that FS+digit was
                              * *parsed* by the kernel, not that a program wrote
                              * a byte — the serial port already knows about the
                              * byte, and CI would grep that just as easily.     */
    int      image_logged;   /* emit "wm: image ... pid" only once, same rule:
                              * a program that repaints a picture every frame
                              * would otherwise bury the log in one line per
                              * frame, and the fact worth testing is that a whole
                              * RGB24 record was received and can be blitted.    */
    char     name[PROC_NAME_MAX];
    struct surface s;
};

static struct program prog[SHELL_PROG_MAX];

static struct program *program_for_pid(int pid)
{
    if (pid <= 0)
        return NULL;
    for (int i = 0; i < SHELL_PROG_MAX; i++)
        if (prog[i].used && prog[i].live && prog[i].pid == pid)
            return &prog[i];
    return NULL;
}

/* A hex digit as its value, or -1. The WW HH header is four of them. */
static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Feed one byte to the picture engine (see RASTER_ESC). Returns 1 when the byte
 * belonged to a record and the caller must not treat it as text, 0 to fall
 * through to the row parser. The whole point is that once a record is open its
 * bytes are RGB24 and nothing else: a 0x0A in the middle of a pixel is a green
 * value, not a newline, so this branch runs before every other rule in
 * surf_write. */
static int raster_feed(struct surface *s, char c)
{
    if (s->rstage == 4) {
        /* Three bytes make one pixel; a partial pixel waits for the next write,
         * which is why rpix is on the surface and not on the stack. */
        s->rpix[s->rpixn++] = (uint8_t)c;
        if (s->rpixn == 3) {
            s->rpixn = 0;
            if (s->raster != NULL && s->rcnt < s->rwant)
                s->raster[s->rcnt++] =
                    gfx_rgb(s->rpix[0], s->rpix[1], s->rpix[2]);
            if (s->rcnt >= s->rwant) {
                s->rstage = 0;
                s->saw_image = 1;
                s->dirty = 1;
            }
        }
        return 1;
    }
    if (s->rstage == 3) {
        int v = hexval(c);
        if (v < 0) {                       /* a broken header: drop the record */
            s->rstage = 0;
            s->rhn = 0;
            return 1;
        }
        s->rhex[s->rhn++] = (char)v;
        if (s->rhn == 4) {
            int w = (unsigned char)s->rhex[0] * 16 + (unsigned char)s->rhex[1];
            int h = (unsigned char)s->rhex[2] * 16 + (unsigned char)s->rhex[3];
            s->rhn = 0;
            if (w < 1 || h < 1 || w > RASTER_MAX_W || h > RASTER_MAX_H) {
                s->rstage = 0;             /* empty or too big: refuse */
                return 1;
            }
            uint32_t *buf = kmalloc((size_t)w * h * sizeof(uint32_t));
            if (buf == NULL) {
                s->rstage = 0;             /* no room: the picture just doesn't come */
                return 1;
            }
            s->raster = buf;
            s->rw = w;
            s->rh = h;
            s->rcnt = 0;
            s->rwant = w * h;
            s->rpixn = 0;
            s->rstage = 4;
        }
        return 1;
    }
    if (s->rstage == 1) {
        s->rstage = (c == 'R') ? 3 : 0;    /* only RS 'R' opens a record */
        return 1;                          /* RS and its follower are no glyphs */
    }
    if (c == RASTER_ESC) {
        s->rstage = 1;
        return 1;
    }
    return 0;
}

/* Assemble a chunk of output into a surface's rows. Bytes arrive in whatever
 * sizes the program chose — half a line, a whole file, forty at a time — so a
 * pending row fills until a newline or a full line forces it out. */
static void surf_write(struct surface *s, const char *data, uint64_t len)
{
    for (uint64_t i = 0; i < len; i++) {
        char c = data[i];
        if (raster_feed(s, c))
            continue;
        if (c == '\r')
            continue;
        if (s->awaiting_attr) {
            /* The byte after FS names the pen. Anything else is not a colour
             * request, so it is dropped rather than painted: a stray FS must
             * not eat a glyph, and a digit out of range must not index past the
             * palette. `awaiting_attr` is a flag and not a two-byte window
             * because writes arrive in whatever sizes the program chose — FS at
             * the end of one buffer and its digit at the start of the next is
             * the normal case, not a corner. */
            s->awaiting_attr = 0;
            if (c >= '0' && c < '0' + COLOR_PALETTE) {
                s->fg = c - '0';
                s->saw_color = 1;
            }
            continue;
        }
        if (c == COLOR_ESC) {
            s->awaiting_attr = 1;
            continue;
        }
        if (c == '\f') {
            /* Form feed means "this is a screen, not another line". A program
             * that draws a whole frame — an editor, a viewer, a status panel —
             * would otherwise have to watch its own output scroll off a
             * scrollback it never asked for, one repaint at a time. Clearing on
             * \f is the smallest thing that turns an append-only surface into a
             * canvas, and it needs no syscall because the byte is already on
             * the wire the program is writing to. */
            surf_clear(s);
            continue;
        }
        if (c == '\n' || s->pending_len >= SHELL_COLS) {
            if (s->pending_len > 0) {
                s->pending[s->pending_len] = '\0';
                surf_row(s, s->pending, s->pending_attr);
                s->pending_len = 0;
            }
            if (c != '\n')
                s->pending[s->pending_len++] = c;   /* wrap: keep the byte */
            continue;
        }
        s->pending_attr[s->pending_len] = (uint8_t)s->fg;
        s->pending[s->pending_len++] = c;
    }
    s->dirty = 1;
}

/* The one console sink the kernel calls for a program's write(1)/write(2). It
 * decides by *whose* bytes these are: proc_current_pid() is that program while
 * it owns the CPU, so they go to its window; when the writer is the shell, a
 * boot demo, or an un-launched child, they go to the terminal instead. The
 * serial port has already received every byte before this is ever reached (see
 * vfs.c), so routing changes only what a human sees, never what CI greps. */
static void console_router(const char *data, uint64_t len)
{
    struct program *pr = program_for_pid(proc_current_pid());
    if (pr != NULL) {
        if (!pr->routed_logged) {
            pr->routed_logged = 1;
            serial_write("wm: routed output pid ");
            serial_write_dec((uint64_t)pr->pid);
            serial_write("\n");
        }
        pr->bytes += len;
        if (!pr->screen_logged) {
            for (uint64_t i = 0; i < len; i++) {
                if (data[i] != '\f')
                    continue;
                pr->screen_logged = 1;
                serial_write("wm: screen cleared pid ");
                serial_write_dec((uint64_t)pr->pid);
                serial_write("\n");
                break;
            }
        }
        surf_write(&pr->s, data, len);
        /* Read from the surface and not from `data`: this line is the kernel
         * saying it moved a pen, and only the parser knows whether the byte it
         * was given was the start of a request or the whole of one. */
        if (pr->s.saw_color && !pr->color_logged) {
            pr->s.saw_color = 0;
            pr->color_logged = 1;
            serial_write("wm: colour pid ");
            serial_write_dec((uint64_t)pr->pid);
            serial_write("\n");
        }
        /* The same once-per-window rule, for a picture that arrived whole: the
         * kernel saying the RGB24 record closed and can be blitted, keyed to the
         * dimensions it parsed rather than the ones the program claims, so the
         * line is a fact about what landed in the buffer. */
        if (pr->s.saw_image && !pr->image_logged) {
            pr->s.saw_image = 0;
            pr->image_logged = 1;
            serial_write("wm: image ");
            serial_write_dec((uint64_t)pr->s.rw);
            serial_write("x");
            serial_write_dec((uint64_t)pr->s.rh);
            serial_write(" pid ");
            serial_write_dec((uint64_t)pr->pid);
            serial_write("\n");
        }
        return;
    }
    surf_write(&term, data, len);
}

/* Claim a surface for a just-launched program. Returns its slot (which becomes
 * the window's content id) or -1 when all six hold windows nobody has closed
 * yet — in which case the launch still runs, its output just falls back to the
 * terminal rather than being silently dropped. */
int shell_program_open(const char *name, int pid)
{
    for (int i = 0; i < SHELL_PROG_MAX; i++) {
        struct program *pr = &prog[i];
        if (pr->used)
            continue;
        pr->used = 1;
        pr->live = 1;
        pr->finished = 0;
        pr->pid = pid;
        pr->exit_code = 0;
        pr->bytes = 0;
        pr->routed_logged = 0;
        pr->screen_logged = 0;
        pr->color_logged = 0;
        pr->image_logged = 0;
        int n = 0;
        for (; name[n] != '\0' && n + 1 < PROC_NAME_MAX; n++)
            pr->name[n] = name[n];
        pr->name[n] = '\0';
        surf_clear(&pr->s);
        /* The pen is reset here and not in surf_clear: a slot can be recycled
         * from a closed highlighter to a plain program, and a fresh window must
         * start in the default ink even if its tenant never names a colour. A
         * live program's own \f keeps the pen, because that clear is one frame
         * of a running painting, not the birth of a window. */
        pr->s.fg = 0;
        pr->s.awaiting_attr = 0;
        pr->s.saw_color = 0;
        pr->s.dirty = 0;          /* opening a window is not new output */
        return i;
    }
    return -1;
}

/* Once per desktop frame: a window bound to a pid that has stopped owning a CPU
 * is finished. Read the exit code by that never-reused pid, say so on the serial
 * line the tests grep, and leave the window open showing "EXIT n" under its
 * output. Closing it stays the user's decision, not the kernel's. */
void shell_program_pump(void)
{
    for (int i = 0; i < SHELL_PROG_MAX; i++) {
        struct program *pr = &prog[i];
        if (!pr->used || !pr->live)
            continue;
        if (proc_pid_live(pr->pid))
            continue;

        pr->live = 0;
        pr->finished = 1;
        int code = 0;
        if (proc_exit_status(pr->pid, &code) == 0)
            pr->exit_code = code;

        serial_write("wm: program window pid ");
        serial_write_dec((uint64_t)pr->pid);
        serial_write(" finished code ");
        serial_write_signed(pr->exit_code);
        serial_write(", ");
        serial_write_dec(pr->bytes);
        serial_write(" bytes\n");

        surf_put_exit(&pr->s, pr->exit_code);
    }
}

const char *shell_program_name(int slot)
{
    if (slot < 0 || slot >= SHELL_PROG_MAX || !prog[slot].used)
        return "";
    return prog[slot].name;
}

int shell_program_live(int slot)
{
    if (slot < 0 || slot >= SHELL_PROG_MAX || !prog[slot].used)
        return 0;
    return prog[slot].live;
}

int shell_program_exit_code(int slot)
{
    if (slot < 0 || slot >= SHELL_PROG_MAX || !prog[slot].used)
        return 0;
    return prog[slot].exit_code;
}

/* Any program surface changed since it was last painted — the reason a running
 * program's window repaints on its own, exactly as the terminal does. */
int shell_program_dirty_any(void)
{
    for (int i = 0; i < SHELL_PROG_MAX; i++)
        if (prog[i].used && prog[i].s.dirty)
            return 1;
    return 0;
}

/* Whether *this* program's surface changed since it was last painted. The
 * compositor uses it to damage only the window whose text moved, rather than
 * repainting the whole desktop because some program somewhere produced output.
 * 0 for a free or out-of-range slot, so a window bound to a stale slot is safe. */
int shell_program_dirty_slot(int slot)
{
    if (slot < 0 || slot >= SHELL_PROG_MAX || !prog[slot].used)
        return 0;
    return prog[slot].s.dirty;
}

void shell_program_clear_dirty_all(void)
{
    for (int i = 0; i < SHELL_PROG_MAX; i++)
        prog[i].s.dirty = 0;
}

void shell_program_draw(int slot, int x, int y, int width)
{
    (void)width;
    if (slot < 0 || slot >= SHELL_PROG_MAX || !prog[slot].used)
        return;
    surf_draw(&prog[slot].s, x, y);
}

/* The user closed the window. Free the slot so its six- window pool has room
 * again; the process it belonged to is already gone or already on its way, and
 * nothing here kills it — a window being dismissed is not a signal. */
void shell_program_close(int slot)
{
    if (slot < 0 || slot >= SHELL_PROG_MAX)
        return;
    prog[slot].used = 0;
    prog[slot].live = 0;
    prog[slot].finished = 0;
    prog[slot].pid = 0;
    prog[slot].exit_code = 0;
    prog[slot].bytes = 0;
    prog[slot].routed_logged = 0;
    prog[slot].name[0] = '\0';
    surf_clear(&prog[slot].s);
    prog[slot].s.dirty = 0;
}


/* --- running a program ----------------------------------------------- */
#define PROGRAM_PATH_MAX 64

/* The one path from a word to a running program. Both entry points — the
 * keyboard prompt and the desktop's start menu — come through here, so a
 * program started by a mouse click gets the same /bin lookup, the same
 * terminal as its stdout and the same exit code as one typed by hand. `verb`
 * and `origin` are the only difference, and they exist because a log that
 * cannot tell the two apart cannot prove which one a test drove.
 *
 * Everything that is not a built-in is looked for in /bin and started as a
 * *child of the shell's own process slot* (see proc.h), with the rest of the
 * line as argv and this grid as its console. It used to go through
 * usermode_run(), which has no process table: a program could not spawn, wait
 * or kill, because there was no entry for it to be anybody's parent or child.
 * Now `reaper` typed at the prompt does what the boot demo does — a real bell,
 * killed by a real kill, reaped by a real wait — from the keyboard or the
 * mouse. That is the difference between "the shell has commands" and "there is
 * an OS": the kernel never hears about a program's name beyond the path it
 * opened, and the program itself is a process like any other. */
static int launch(int argc, const char *const *argv,
                  const char *verb, const char *origin)
{
    char path[PROGRAM_PATH_MAX];
    int n = 0;

    for (const char *p = "/bin/"; *p != '\0'; p++)
        path[n++] = *p;
    for (int i = 0; argv[0][i] != '\0' && n < PROGRAM_PATH_MAX - 1; i++)
        path[n++] = argv[0][i];
    path[n] = '\0';

    if (!vfs_exists(path)) {
        put_line("COMMANDE INCONNUE. TAPE HELP.");
        serial_write("shell: no program ");
        serial_write(path);
        serial_write("\n");
        return -1;
    }

    /* The word the CI contract greps for: reaching this line with "running"
     * means the keyboard decoded the command in order through the real IRQ
     * path; with "launched" it means a mouse click reached the same code. */
    serial_write("shell: ");
    serial_write(verb);
    serial_write(" '");
    serial_write(argv[0]);
    serial_write("' ");
    serial_write(origin);
    serial_write("\n");

    int pid = 0;
    if (proc_spawn_async(path, argc, argv, &pid) == -1) {
        put_line("IMPOSSIBLE DE LANCER CE PROGRAMME");
        serial_write("shell: failed to run ");
        serial_write(path);
        serial_write("\n");
        return -1;
    }

    /* Honest about what just happened, and about what did not: this placed the
     * program in the table as the shell's child and returned while it is still
     * about to work. The old synchronous launch claimed a completion the instant
     * the program ended and froze the desktop until then; now launch() makes no
     * such claim. The exit code arrives later, on a visit that runs the process
     * to its end, through proc_reap_live()'s "ran and returned" / "exited with"
     * lines. And the program no longer shares the terminal's grid: it is given a
     * window of its own (a surface the console router streams its output into,
     * painted and driven by the desktop), so the compositor keeps painting
     * between slices *and* the program's text belongs to something the user can
     * move, overlap and — after it finishes — close. */
    int slot = shell_program_open(argv[0], pid);
    if (slot >= 0) {
        wm_open_program(slot, pid);
        serial_write("wm: program window opened '");
        serial_write(argv[0]);
        serial_write("' pid ");
        serial_write_dec((uint64_t)pid);
        serial_write("\n");
    } else {
        /* All six windows are still open on finished programs. The launch is not
         * refused for that — the process runs and its output falls back to the
         * terminal rather than vanishing, and the log says which happened. */
        serial_write("wm: no window room, '");
        serial_write(argv[0]);
        serial_write("' output to terminal\n");
    }

    serial_write("shell: ");
    serial_write(argv[0]);
    serial_write(" started, pid ");
    serial_write_dec((uint64_t)pid);
    serial_write(" (running)\n");
    return 0;
}

static void cmd_program(int argc, const char *const *argv, const char *origin)
{
    /* `origin` is the only difference between a keystroke and a script line,
     * and it is not cosmetic: a log that cannot tell the two apart cannot
     * prove which one a test drove. */
    launch(argc, argv, "running", origin);
}

/* The desktop's start menu calls this with one of the names it read out of
 * /bin. No arguments: a menu row is a program, not a command line. */
int shell_launch(const char *name)
{
    if (name == NULL || name[0] == '\0')
        return -1;

    /* The prompt echoes its own line in execute(); the menu has to write the
     * equivalent, or a program's output appears on screen with nothing saying
     * which program produced it. */
    char echo[SHELL_COLS + 1];
    int n = 0;
    for (const char *p = "DEMARRER : "; *p != '\0' && n < SHELL_COLS - 1; p++)
        echo[n++] = *p;
    for (int i = 0; name[i] != '\0' && n < SHELL_COLS - 1; i++)
        echo[n++] = name[i];
    echo[n] = '\0';
    put_line(echo);

    const char *argv[1];
    argv[0] = name;
    return launch(1, argv, "launched", "from the start menu");
}

static void cmd_about(void)
{
    put_line("WINDCLONE " WC_VERSION " - X86_64");
    put_line("KERNEL: C, ASM, SANS LIBC");
    put_line("RING 3 + INITRD + FRAMEBUFFER: OK");
    put_line_num("UPTIME: ", timer_ticks() / 100);
    put_line("SHELL: CE QUE TU TAPES EST LU PAR L'IRQ1");
}

static void cmd_bat(int argc, const char *const *argv);
static int ci_equal(const char *a, const char *b);

/* `fetch /second` — the address bar's other half, and the shell's only built-in
 * that does not finish while the call is running. The page cannot be read back
 * here: a received frame exists only once the emulator's network gets the CPU,
 * and that happens in `hlt` in the idle loop, so this function can only hand the
 * request to net.c and say out loud what it asked for. The transfer, the store
 * and the digest come later, from the same machine that fetched the boot page.
 *
 * Three refusals, each a line rather than a silent drop: no name, a name the
 * client will not accept (nothing resolved yet, or a navigation already under
 * way), and a path with no leading '/'. The last one is not pedantry — the GET
 * line is built from this string byte for byte, so "second" would ask a real
 * server for a document it does not have and read a 404 back as if the network
 * had failed. */
static void cmd_fetch(int argc, const char *const *argv)
{
    if (argc < 2) {
        put_line("USAGE: FETCH <CHEMIN>  PAR /SECOND");
        return;
    }
    if (argv[1][0] != '/') {
        put_line("FETCH: LE CHEMIN COMMENCE PAR /");
        return;
    }
    if (!net_resolved()) {
        put_line("FETCH: PAS DE PASSERELLE RESOLUE - RESEAU OCCUPE");
        return;
    }
    if (!net_start_fetch(argv[1], "net/fetched")) {
        put_line("FETCH: NAVIGATION DEJA EN COURS");
        return;
    }
    put_line("FETCH: REQUETE ENREGISTREE - LE RESEAU VA NAVIGUER");
    serial_write("shell: fetch ");
    serial_write(argv[1]);
    serial_write(" - navigating over tcp\n");
}

/* --- cmd.exe's file verbs: DIR, DEL, REN ---------------------------------
 * `ls` and `copy` already live in /bin, and they are what a *program* can do.
 * These three are built-ins because they are the shell's own verbs — the ones
 * a person at a prompt expects the command line itself to answer, exactly as
 * cmd.exe keeps DIR/DEL/REN internal rather than shipping three .exe's. They
 * reach the file layer through the VFS and nothing else: the archive is
 * read-only, so DEL and REN refuse an initrd name with a line that says why,
 * and only a file in the writable store can actually move or vanish.
 *
 * Each refusal is its own sentence rather than one "error" string: the
 * difference between "the archive is read-only", "no such file" and "the new
 * name is taken" is the difference a person needs to fix what they typed. */

/* Count every entry the file layer offers, store first then archive, the way
 * a caller loop does. The numbers go out on the serial port, so a headless test
 * can watch one appear after a copy and vanish after a del without parsing the
 * French on the screen. */
static void count_files(unsigned *files, unsigned long *bytes)
{
    *files = 0;
    *bytes = 0;
    for (unsigned long i = 0; ; i++) {
        struct wc_dirent de;
        if (vfs_dirent(i, &de) != 0)
            break;
        if (de.namelen == 0)
            continue;
        (*files)++;
        *bytes += (unsigned long)de.size;
    }
}

/* Append a decimal to a screen buffer. There is no printf in the kernel, and a
 * DIR that shows a size needs one; this is the smallest honest version. The cap
 * is checked rather than assumed: a uint64 printed in full is twenty digits,
 * and a formatter that overflowed the row it was filling would be a stack
 * smash hiding behind a cosmetic command. */
static int dec_into(char *out, int n, unsigned long v, int cap)
{
    char d[24];
    int m = 0;

    if (v == 0)
        d[m++] = '0';
    while (v > 0 && m < (int)sizeof d) {
        d[m++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (m > 0 && n + 1 <= cap)
        out[n++] = d[--m];
    return n;
}

static void cmd_dir(void)
{
    unsigned files;
    unsigned long bytes;

    count_files(&files, &bytes);
    put_line("FICHIERS - STOCKAGE TEMPORAIRE PUIS ARCHIVE:");

    for (unsigned long i = 0; ; i++) {
        struct wc_dirent de;
        if (vfs_dirent(i, &de) != 0)
            break;
        if (de.namelen == 0)
            continue;

        char row[SHELL_COLS + 1];
        int n = 0;
        row[n++] = ' ';
        row[n++] = ' ';
        for (unsigned k = 0; k < de.namelen && n < SHELL_COLS - 12; k++)
            row[n++] = upper(de.name[k]);
        for (; n < SHELL_COLS - 12; n++)
            row[n] = ' ';
        n = dec_into(row, n, (unsigned long)de.size, SHELL_COLS);
        row[n] = '\0';
        put_line(row);
    }

    {
        char row[SHELL_COLS + 1];
        int n = 0;
        const char *pre = "TOTAL: ";
        for (; *pre != '\0' && n < SHELL_COLS - 1; pre++)
            row[n++] = *pre;
        n = dec_into(row, n, files, SHELL_COLS);
        const char *mid = " FICHIERS, ";
        for (; *mid != '\0' && n < SHELL_COLS - 1; mid++)
            row[n++] = *mid;
        n = dec_into(row, n, bytes, SHELL_COLS);
        row[n] = '\0';
        put_line(row);
    }

    serial_write("shell: dir shows ");
    serial_write_dec((uint64_t)files);
    serial_write(" entries, ");
    serial_write_dec((uint64_t)bytes);
    serial_write(" bytes in the file layer\n");
}

static void cmd_del(int argc, const char *const *argv)
{
    if (argc < 2) {
        put_line("USAGE: DEL <CHEMIN>");
        return;
    }

    int64_t r = vfs_remove(argv[1]);
    if (r == 0) {
        unsigned files;
        unsigned long bytes;
        count_files(&files, &bytes);
        put_line("FICHIER SUPPRIME DU STOCKAGE ECRIVABLE");
        serial_write("shell: deleted ");
        serial_write(argv[1]);
        serial_write(" - ");
        serial_write_dec((uint64_t)files);
        serial_write(" entries left in the file layer\n");
        return;
    }

    put_line("SUPPRESSION REFUSEE");
    serial_write("shell: del refused ");
    serial_write(argv[1]);
    if (r == WC_EPERM)
        serial_write(" - the archive is read-only\n");
    else if (r == WC_ENOENT)
        serial_write(" - no such file\n");
    else if (r == WC_EBUSY)
        serial_write(" - a descriptor still holds it\n");
    else
        serial_write(" - not a name the store can hold\n");
}

static void cmd_ren(int argc, const char *const *argv)
{
    if (argc < 3) {
        put_line("USAGE: REN <ANCIEN> <NOUVEAU>");
        return;
    }

    int64_t r = vfs_rename(argv[1], argv[2]);
    if (r == 0) {
        put_line("FICHIER RENOME DANS LE STOCKAGE ECRIVABLE");
        serial_write("shell: renamed ");
        serial_write(argv[1]);
        serial_write(" -> ");
        serial_write(argv[2]);
        serial_write("\n");
        return;
    }

    put_line("RENOMMAGE REFUSE");
    serial_write("shell: ren refused ");
    serial_write(argv[1]);
    if (r == WC_EPERM)
        serial_write(" - the archive is read-only\n");
    else if (r == WC_ENOENT)
        serial_write(" - no such file\n");
    else if (r == WC_EEXIST)
        serial_write(" -> the new name is already taken\n");
    else if (r == WC_EBUSY)
        serial_write(" - a descriptor still holds it\n");
    else
        serial_write(" - not a name the store can hold\n");
}


/* cmd's `move` — the file verb that is NOT just a rename. `ren` refuses when the
 * destination name is already taken (WC_EEXIST); Windows `move` overwrites it,
 * dropping the file that was in the way and landing the source in its place. So
 * the whole difference is what happens on EEXIST: clear the destination, then
 * move onto the freed name. And the invariant a refused operation must keep is
 * the one cmd_ren already promises — if the move does not happen, the source is
 * exactly where it was. That is why the rename is attempted whole first and the
 * destination is only deleted once that attempt has proved it occupied: a plain
 * ENOENT or a read-only archive source never reaches the delete at all.
 *
 * cmd scripts pass `/Y` to suppress the overwrite confirmation prompt. There is
 * no interactive prompt on this shell, so the overwrite is always the behaviour
 * and `/Y` is accepted and simply does nothing extra — the switch a real script
 * leans on is parsed so the line still resolves to the right source and target,
 * and cmd reads a switch in any case, so `/y` and `/Y` are the same word here. */
static void cmd_move(int argc, const char *const *argv)
{
    int a = 1;
    if (argc >= 4 && ci_equal(argv[1], "/y"))
        a = 2;                        /* skip cmd's no-confirm switch */
    if (argc - a < 2) {
        put_line("USAGE: MOVE <ANCIEN> <NOUVEAU>");
        return;
    }
    const char *from = argv[a];
    const char *to   = argv[a + 1];

    int64_t r = vfs_rename(from, to);
    int replaced = 0;
    if (r == WC_EEXIST) {
        int64_t d = vfs_remove(to);   /* cmd's move replaces the file in the way */
        if (d == 0) {
            r = vfs_rename(from, to);
            replaced = 1;
        } else {
            r = d;                    /* report why the way would not clear */
        }
    }

    if (r == 0) {
        put_line("FICHIER DEPLACE DANS LE STOCKAGE ECRIVABLE");
        serial_write("shell: moved ");
        serial_write(from);
        serial_write(" -> ");
        serial_write(to);
        serial_write(replaced ? " (replaced existing)\n" : "\n");
        return;
    }

    put_line("DEPLACEMENT REFUSE");
    serial_write("shell: move refused ");
    serial_write(from);
    if (r == WC_EPERM)
        serial_write(" - the archive is read-only\n");
    else if (r == WC_ENOENT)
        serial_write(" - no such file\n");
    else if (r == WC_EBUSY)
        serial_write(" - source or destination is held open\n");
    else
        serial_write(" - not a name the store can hold\n");
}


/* One command line, wherever it came from. `line` is written in place — spaces
 * become NULs so argv[] points into it and there is no second copy to keep in
 * sync — and the buffer belongs to the caller, which is what lets a typed line
 * live in `input` and a script line live in `bat_line` while both go through
 * this one dispatcher. `origin` names which one it was. */
static void run_line(char *line, int len, const char *origin)
{
    const char *argv[USER_ARG_MAX];
    int argc = 0;
    int i = 0;

    while (i < len && argc < USER_ARG_MAX) {
        while (i < len && line[i] == ' ')
            line[i++] = '\0';
        if (i >= len)
            break;
        argv[argc++] = &line[i];
        while (i < len && line[i] != ' ')
            i++;
    }

    /* The cap can fill while more words are still pending. Each argument is
     * only NUL-terminated when the next iteration turns its following space
     * into '\0', so the last kept argument would otherwise run on through the
     * un-null'd spaces and swallow the rest of the line as one folded string.
     * `i` sits on the delimiter we stopped at; cut the argument there. */
    if (argc == USER_ARG_MAX && i < len)
        line[i] = '\0';

    if (argc == 0)
        return;                       /* nothing but spaces */
    if (strcmp(argv[0], "help") == 0)
        cmd_help();
    else if (strcmp(argv[0], "about") == 0)
        cmd_about();
    else if (strcmp(argv[0], "clear") == 0)
        surf_clear(&term);
    else if (strcmp(argv[0], "bat") == 0)
        cmd_bat(argc, argv);
    else if (strcmp(argv[0], "fetch") == 0)
        cmd_fetch(argc, argv);
    else if (strcmp(argv[0], "dir") == 0)
        cmd_dir();
    else if (strcmp(argv[0], "del") == 0)
        cmd_del(argc, argv);
    else if (strcmp(argv[0], "ren") == 0)
        cmd_ren(argc, argv);
    else if (strcmp(argv[0], "move") == 0)
        cmd_move(argc, argv);
    else
        cmd_program(argc, argv, origin);
}

/* --- batch files: a script read out of the writable store -----------------
 * cmd.exe's oldest idea, and the reason v0.1.18 mattered: bytes a *program*
 * wrote are now something the shell can read back and obey. `bat demo` opens
 * /tmp/demo.bat, splits it on newlines and runs every line through the same
 * dispatcher a typed line goes through, so a script gets the built-ins and the
 * /bin programs exactly the way a person at the prompt does.
 *
 * Three limits, each one a refusal with a line in the log rather than a crash:
 *   - BAT_MAX_LINES commands, because a file that runs forever is not a script;
 *   - no `bat` inside a `bat`, so the depth is two and a self-referring file
 *     cannot eat the kernel stack it is executing on;
 *   - BAT_LINE_MAX bytes per line, and a longer line is truncated rather than
 *     skipped — a half command may fail, which is an answer, where quietly
 *     dropping it is a lie about what ran.
 * The whole file is read into a kernel static before the first line runs, so a
 * script cannot change underneath itself half way through (and on this store,
 * nothing else can change it either — there is no scheduler preemption of a
 * syscall that simple, but the rule is worth having anyway). */
#define BAT_NAME_MAX   12         /* same rule /bin/save uses: a word, not a path */
#define BAT_PATH_MAX   32         /* "/tmp/" + name + ".bat" + NUL, with room */
#define BAT_LINE_MAX   64         /* one command, SHELL_COLS minus the prompt */
#define BAT_MAX        512        /* the whole script, in one static */
#define BAT_MAX_LINES  16

static char bat_text[BAT_MAX];
static char bat_line[BAT_LINE_MAX];
static char bat_work[BAT_LINE_MAX];   /* after '@' and after %VAR% expansion */

/* Compose "/tmp/<name>.bat" into `out`. Returns the length, or 0 when the name
 * is not a word — letters, digits or underscore — because nothing that reaches
 * the VFS from here may look like a traversal. The store is flat and the
 * archive is searched first anyway, so no name here could have escaped; the
 * filter exists so a typo says "bad name" instead of making a file nobody
 * meant to make. */
static int bat_path(const char *name, char *out, int cap)
{
    const char *p = "/tmp/";
    int n = 0;

    if (name == NULL || name[0] == '\0' || cap < BAT_PATH_MAX)
        return 0;
    for (; *p != '\0'; p++)
        out[n++] = *p;
    for (int i = 0; name[i] != '\0'; i++) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_';
        if (!ok || i >= BAT_NAME_MAX || n > cap - 6)
            return 0;
        out[n++] = c;
    }
    for (p = ".bat"; *p != '\0'; p++)
        out[n++] = *p;
    out[n] = '\0';
    return n;
}

/* --- batch built-ins: ECHO ON/OFF, SET, %VAR% -----------------------------
 * The half of cmd.exe a real .bat leans on and v0.1.36 did not have. Nearly
 * every Windows script opens with `@echo off` and carries its values in
 * `SET NAME=value` referenced as `%NAME%`; without those three, a script copied
 * from Windows echoed its own commands and ran a literal "%GREET%" where it
 * meant "hello". This adds exactly those, and nothing else: no `if`, no `for`,
 * no redirection, no labels — each is its own rung and its own release.
 *
 * They live here, in the batch reader, not in run_line. A line typed at the
 * prompt is dispatched the way it always was (an `echo` there still runs the
 * /bin/echo program); the ON/OFF/SET/% behaviour is what a *script* gets. That
 * keeps v0.1.19's prompt path untouched and states the scope honestly. */
#define BAT_ENV_MAX   8                 /* variables held for the session      */
#define BAT_ENV_NAME  16
#define BAT_ENV_VAL   48                /* a value with spaces in it still fits */

static char env_name[BAT_ENV_MAX][BAT_ENV_NAME];
static char env_val[BAT_ENV_MAX][BAT_ENV_VAL];
static int  env_count;
static int  bat_echo_on;                /* the per-script echo state           */

/* Fold a whole word to upper case into dst, NUL-terminated, bounded by cap.
 * cmd variables are case-insensitive, so names are canonicalised on the way in
 * and on the way to lookup; `hello world` is preserved in the value because
 * only names are folded. */
static void upper_word(char *dst, int cap, const char *src, int srclen)
{
    int n = 0;
    for (int i = 0; i < srclen && n < cap - 1; i++)
        dst[n++] = upper(src[i]);
    dst[n] = '\0';
}

/* Look up a folded name; returns the value or NULL. */
static const char *env_get(const char *folded)
{
    for (int i = 0; i < env_count; i++)
        if (strcmp(env_name[i], folded) == 0)
            return env_val[i];
    return NULL;
}

/* Define NAME=value, overwriting an existing name rather than shadowing it, the
 * way SET does. Truncates honestly to the caps. Returns 1 when stored. */
static int env_set(const char *name, int nlen, const char *val)
{
    char folded[BAT_ENV_NAME];
    upper_word(folded, sizeof folded, name, nlen);
    if (folded[0] == '\0')
        return 0;

    int slot = -1;
    for (int i = 0; i < env_count; i++)
        if (strcmp(env_name[i], folded) == 0) { slot = i; break; }
    if (slot < 0) {
        if (env_count >= BAT_ENV_MAX)
            return 0;                   /* environment full: refuse, do not drop */
        slot = env_count++;
    }

    int n = 0;
    for (; folded[n] != '\0'; n++)
        env_name[slot][n] = folded[n];
    env_name[slot][n] = '\0';

    int m = 0;
    for (int i = 0; val[i] != '\0' && m < BAT_ENV_VAL - 1; i++)
        env_val[slot][m++] = val[i];
    env_val[slot][m] = '\0';
    return 1;
}

/* Copy `in` to `out`, replacing every %NAME% with the value of the (folded)
 * name. An undefined name expands to nothing, which is exactly what cmd.exe
 * does and what makes the difference between "the name was never set" and "the
 * value was empty" show up on the transcript rather than hide. A '%' with no
 * closing '%' is copied literally, so a stray percent does not eat the line.
 * cmd's caret escape applies here too: `^` consumes itself and emits the next
 * character literally, so a script can write a real percent with `^%`. */
static int bat_expand(const char *in, char *out, int cap)
{
    int n = 0;
    for (int i = 0; in[i] != '\0'; ) {
        if (in[i] == '^' && in[i + 1] != '\0') {
            if (n < cap - 1)
                out[n++] = in[i + 1];   /* caret consumed, next char literal  */
            i += 2;
            continue;
        }
        if (in[i] == '%') {
            int j = i + 1, start = j;
            while (in[j] != '\0' && in[j] != '%')
                j++;
            if (in[j] == '%' && j > start) {
                char folded[BAT_ENV_NAME];
                upper_word(folded, sizeof folded, in + start, j - start);
                const char *val = env_get(folded);
                if (val != NULL)
                    for (int k = 0; val[k] != '\0' && n < cap - 1; k++)
                        out[n++] = val[k];
                i = j + 1;
                continue;
            }
        }
        if (n < cap - 1)
            out[n++] = in[i];
        i++;
    }
    out[n] = '\0';
    return n;
}

/* Prompt-time %NAME% expansion, the sibling of bat_expand a person typing at
 * the keyboard deserves. One deliberate difference keeps it safe: a DEFINED
 * name folds to upper case and substitutes its value, but an UNDEFINED name is
 * left LITERAL (`%NOPE%` survives unchanged), where the batch reader's
 * bat_expand drops it to nothing. That choice is what makes this usable on the
 * prompt without corrupting a typed line that merely happens to carry a
 * percent - the `save script a ... echo %x%` that builds a script must write
 * `%x%` to the file, not an empty hole, because x is defined inside the script,
 * not yet at the prompt. A lone or unclosed '%' is copied verbatim, exactly as
 * in the batch reader. cmd's caret escape lands here too: `^` consumes itself
 * and emits the next character literally, so `^%` is a percent that cannot
 * start a reference - `echo ^%pz%` prints `%pz%` even when pz is set. Returns
 * the length written. */
static int prompt_expand(const char *in, char *out, int cap)
{
    int n = 0;
    for (int i = 0; in[i] != '\0'; ) {
        if (in[i] == '^' && in[i + 1] != '\0') {
            if (n < cap - 1)
                out[n++] = in[i + 1];   /* caret consumed, next char literal  */
            i += 2;
            continue;
        }
        if (in[i] == '%') {
            int j = i + 1, start = j;
            while (in[j] != '\0' && in[j] != '%')
                j++;
            if (in[j] == '%' && j > start) {
                char folded[BAT_ENV_NAME];
                upper_word(folded, sizeof folded, in + start, j - start);
                const char *val = env_get(folded);
                if (val != NULL) {           /* defined: substitute the value  */
                    for (int k = 0; val[k] != '\0' && n < cap - 1; k++)
                        out[n++] = val[k];
                    i = j + 1;
                    continue;
                }
                /* undefined: keep the literal %NAME% for the whole run  */
                for (int k = i; k <= j && n < cap - 1; k++)
                    out[n++] = in[k];
                i = j + 1;
                continue;
            }
        }
        if (n < cap - 1)
            out[n++] = in[i];
        i++;
    }
    out[n] = '\0';
    return n;
}

/* Is the line the word `kw` (case-insensitive) followed by a space or end? So
 * "set" and "SET X=1" match, "setup" does not. */
static int is_word(const char *line, const char *kw)
{
    int i = 0;
    for (; kw[i] != '\0'; i++)
        if (upper(line[i]) != upper(kw[i]))
            return 0;
    return line[i] == '\0' || line[i] == ' ';
}

static int ci_equal(const char *a, const char *b)
{
    for (int i = 0; ; i++) {
        if (upper(a[i]) != upper(b[i]))
            return 0;
        if (a[i] == '\0')
            return 1;
    }
}

/* SET NAME=VALUE and bare SET (list the environment). `work` is the whole
 * line, so a value with spaces stays one value; the shell tokeniser would
 * have cut it. `who` names the origin in the transcript ("batch" or
 * "prompt") so the two paths are distinguishable byte for byte in the log.
 * Returns 1 when the line was a SET (handled), 0 to let the caller keep
 * going. Names fold to upper case the way cmd folds them; the value is
 * stored verbatim. BAT_ENV_MAX caps the environment honestly: when full,
 * the line says so instead of silently dropping it. */
static int do_set(const char *work, const char *who)
{
    if (!is_word(work, "set"))
        return 0;
    const char *arg = work + 3;
    while (*arg == ' ')
        arg++;
    if (*arg == '\0') {             /* bare SET: list the environment        */
        serial_write("shell: ");
        serial_write(who);
        serial_write(" env ");
        serial_write_dec((uint64_t)env_count);
        serial_write(" variables\n");
        for (int i = 0; i < env_count; i++) {
            put_line(env_name[i]);
            serial_write("shell: ");
            serial_write(who);
            serial_write(" env ");
            serial_write(env_name[i]);
            serial_write("=");
            serial_write(env_val[i]);
            serial_write("\n");
        }
        return 1;
    }
    const char *eq = NULL;
    for (const char *q = arg; *q != '\0'; q++)
        if (*q == '=') { eq = q; break; }
    if (eq == NULL || eq == arg) {  /* SET with no name or no '=': refuse     */
        put_line("SET ATTEND NOM=VALEUR");
        serial_write("shell: ");
        serial_write(who);
        serial_write(" set refused - want NAME=VALUE\n");
        return 1;
    }
    if (env_set(arg, (int)(eq - arg), eq + 1)) {
        char folded[BAT_ENV_NAME];
        upper_word(folded, sizeof folded, arg, (int)(eq - arg));
        serial_write("shell: ");
        serial_write(who);
        serial_write(" set ");
        serial_write(folded);
        serial_write("=");
        serial_write(eq + 1);
        serial_write("\n");
    } else {
        put_line("SET IMPOSSIBLE: ENVIRONNEMENT PLEIN OU NOM VIDE");
        serial_write("shell: ");
        serial_write(who);
        serial_write(" set refused - environment full or empty name\n");
    }
    return 1;
}

/* TYPE <path> — cmd's file viewer, kept short and in-kernel so the bytes
 * land on the serial transcript a test can read byte for byte. The name is
 * passed straight to vfs_open, which searches the read-only archive first
 * and the writable store second (the same order `type notes.txt` resolves
 * in on a real cmd: current directory then PATH). A missing file refuses;
 * a directory or an unreadable archive member is a "no such file" too.
 * The body is dumped raw; a file not ending in \n gets a trailing newline
 * so the next transcript line starts clean. TYPE shares the batch reader's
 * BAT_MAX buffer because both are bounded by the same 512-byte store cap,
 * and a larger file is honestly truncated rather than looped. */
static int do_type(const char *line)
{
    if (!is_word(line, "type"))
        return 0;
    const char *arg = line + 4;
    while (*arg == ' ')
        arg++;
    if (*arg == '\0') {
        put_line("TYPE ATTEND UN CHEMIN");
        serial_write("shell: type refused - want a path\n");
        return 1;
    }
    int64_t fd = vfs_open(arg);
    if (fd < 0) {
        put_line("TYPE INTROUVABLE");
        serial_write("shell: type no such file ");
        serial_write(arg);
        serial_write("\n");
        return 1;
    }
    int have = 0;
    while (have < BAT_MAX - 1) {
        int64_t got = vfs_read((int)fd, bat_text + have,
                               (uint64_t)(BAT_MAX - 1 - have));
        if (got <= 0)
            break;
        have += (int)got;
    }
    vfs_close((int)fd);

    serial_write("shell: type ");
    serial_write(arg);
    serial_write(" (");
    serial_write_dec((uint64_t)have);
    serial_write(" bytes)\n");
    serial_write_len(bat_text, (size_t)have);
    if (have > 0 && bat_text[have - 1] != '\n')
        serial_write("\n");
    return 1;
}

/* Handle a batch line that is one of the three built-ins. `work` is the line
 * after '@' and after %expansion%. Returns 1 when the line was a built-in (and
 * so must not be dispatched to a program), 0 to let it run normally. Only
 * ECHO-with-no-argument / ECHO ON / ECHO OFF and SET are intercepted; a plain
 * `echo some text` still falls through to /bin/echo, which is what a person
 * copying a working script and typing it both expect. */
static int batch_builtin(const char *work)
{
    if (is_word(work, "echo")) {
        const char *arg = work + 4;
        while (*arg == ' ')
            arg++;
        if (*arg == '\0') {
            put_line(bat_echo_on ? "L'ECHO EST ACTIVE" : "L'ECHO EST INACTIF");
            serial_write("shell: batch echo state ");
            serial_write(bat_echo_on ? "on" : "off");
            serial_write("\n");
            return 1;
        }
        if (ci_equal(arg, "on")) {
            bat_echo_on = 1;
            serial_write("shell: batch echo on\n");
            return 1;
        }
        if (ci_equal(arg, "off")) {
            bat_echo_on = 0;
            serial_write("shell: batch echo off\n");
            return 1;
        }
        return 0;                       /* ECHO <text>: a message, run normally */
    }

    if (do_set(work, "batch"))
        return 1;

    if (is_word(work, "rem")) {         /* cmd's comment word, alongside ';'      */
        serial_write("shell: batch remark skipped\n");
        return 1;
    }

    return 0;
}

static void cmd_bat(int argc, const char *const *argv)
{
    if (argc < 2) {
        put_line("USAGE: BAT <NOM>  EXECUTE /TMP/<NOM>.BAT");
        return;
    }

    char path[BAT_PATH_MAX];
    if (bat_path(argv[1], path, sizeof path) == 0) {
        put_line("NOM DE SCRIPT INVALIDE: DES LETTRES, CHIFFRES OU _.");
        serial_write("shell: bad batch name ");
        serial_write(argv[1]);
        serial_write("\n");
        return;
    }

    int64_t fd = vfs_open(path);
    if (fd < 0) {
        put_line("FICHIER DE SCRIPT INTROUVABLE.");
        serial_write("shell: no batch file ");
        serial_write(path);
        serial_write("\n");
        return;
    }

    int have = 0;
    while (have < BAT_MAX - 1) {
        int64_t got = vfs_read((int)fd, bat_text + have,
                               (uint64_t)(BAT_MAX - 1 - have));
        if (got <= 0)
            break;
        have += (int)got;
    }
    vfs_close((int)fd);
    bat_text[have] = '\0';

    serial_write("shell: batch file ");
    serial_write(path);
    serial_write(" (");
    serial_write_dec((uint64_t)have);
    serial_write(" bytes)\n");

    int ran = 0;
    int lines = 0;
    char *p = bat_text;
    bat_echo_on = 1;                 /* each script starts echoing, like cmd */

    while (*p != '\0') {
        int l = 0;
        while (*p != '\0' && *p != '\n') {
            if (*p != '\r' && l < BAT_LINE_MAX - 1)
                bat_line[l++] = *p;
            p++;
        }
        if (*p == '\n')
            p++;
        bat_line[l] = '\0';

        if (l == 0)
            continue;                 /* a blank line is not a command */

        if (lines >= BAT_MAX_LINES) {
            serial_write("shell: batch stopped at ");
            serial_write_dec((uint64_t)BAT_MAX_LINES);
            serial_write(" lines, more in ");
            serial_write(path);
            serial_write("\n");
            put_line("SCRIPT TROP LONG: LIMITE ATTEINTE.");
            break;
        }
        lines++;

        /* ';' is cmd.exe's comment character, and the line says so on the serial
         * port rather than vanishing: a transcript with holes in it is a
         * transcript you cannot trust. */
        if (bat_line[0] == ';') {
            serial_write("shell: batch comment skipped\n");
            continue;
        }

        /* '@' suppresses the echo of this one line; strip it before anything
         * else so the built-in test and the program both see the real command. */
        const char *body = bat_line;
        int suppress = 0;
        if (bat_line[0] == '@') {
            suppress = 1;
            body++;
        }

        /* Expand %NAME% now, so both what a person sees echoed and what the
         * dispatcher runs are the resolved line - cmd echoes the expanded
         * command too. The serial line below always carries this resolved form,
         * whether or not the screen echo is on, so the transcript can prove an
         * expansion the (possibly off) screen echo would hide. */
        int wl = bat_expand(body, bat_work, sizeof bat_work);

        serial_write("shell: batch exec '");
        serial_write(bat_work);
        serial_write("'\n");

        if (bat_echo_on && !suppress) {
            char echo[BAT_LINE_MAX + 3];
            int e = 0;
            echo[e++] = '>';
            echo[e++] = ' ';
            for (int k = 0; bat_work[k] != '\0' && e < BAT_LINE_MAX; k++)
                echo[e++] = bat_work[k];
            echo[e] = '\0';
            put_line(echo);
        }

        /* The three batch built-ins (ECHO ON/OFF, SET, REM) never reach a
         * program; everything else falls through to run_line as before. */
        if (batch_builtin(bat_work))
            continue;

        /* No nesting. Checked on the resolved line, before run_line() turns its
         * spaces into NULs, so this is a prefix test and not a token one. */
        if (is_word(bat_work, "bat")) {
            serial_write("shell: batch refused nested script\n");
            put_line("PAS DE SCRIPT DANS UN SCRIPT.");
            continue;
        }

        /* Spaces become NULs inside bat_work here, which is fine: this buffer
         * is rewritten from bat_text on the next iteration. */
        run_line(bat_work, wl, "from a batch file");
        ran++;
    }

    serial_write("shell: batch ran ");
    serial_write_dec((uint64_t)ran);
    serial_write(" commands from ");
    serial_write(path);
    serial_write("\n");
}

static void execute(void)
{
    input[input_len] = '\0';

    /* Echo the prompt line into the scrollback first, so history reads
     * like a transcript instead of a series of answers. */
    char echo[SHELL_COLS + 3];
    int n = 0;
    echo[n++] = '>';
    echo[n++] = ' ';
    for (int i = 0; i < input_len && n < SHELL_COLS; i++)
        echo[n++] = input[i];
    echo[n] = '\0';
    put_line(echo);

    if (input_len != 0) {
        /* Expand %VAR% on the typed line first, so what the built-ins see and
         * what run_line dispatches is the resolved form - cmd expands a typed
         * `echo %x%` before it ever reaches ECHO. Only DEFINED names change; an
         * undefined one stays literal so a line that merely carries a percent
         * (like the `save ... echo %x%` that builds a script) is untouched. The
         * `shell: prompt exec '<resolved>'` row is written ONLY when something
         * actually expanded, so a plain command adds no noise to the transcript
         * the other scenarios read. `resolved` is a local copy: run_line will
         * NUL its spaces in place, and the caller's `input` stays pristine for
         * the scrollback echo that already happened above. */
        char resolved[SHELL_COLS * 2 + 1];
        int rl = prompt_expand(input, resolved, sizeof resolved);
        const char *line = resolved;
        if (strcmp(resolved, input) != 0) {
            serial_write("shell: prompt exec '");
            serial_write(resolved);
            serial_write("'\n");
        }
        /* The two cmd built-ins a person reaches for at the keyboard, checked
         * on the resolved line before the tokeniser cuts values in half: `set`
         * defines or lists a session variable, `type` prints a file. Neither
         * is a /bin program, so they live on the prompt path only — a typed
         * `echo` is still /bin/echo, and the batch reader keeps its own copy
         * of `set` via do_set(work, "batch"). The origin tag on anything that
         * still runs ("from the prompt") is what keeps a typed launch
         * distinguishable from a scripted one in the transcript. */
        if (!do_set(line, "prompt") && !do_type(line))
            run_line(resolved, rl, "from the prompt");
    }

    input_len = 0;
    input[0] = '\0';
    term.dirty = 1;
}

/* --- public ------------------------------------------------------------ */

void shell_init(void)
{
    surf_clear(&term);

    /* The shell takes its place in the process table before anything can be
     * typed, and the claim it makes is the one the rest of this file depends
     * on: a slot with a pid means the programs started here have a parent, so
     * they may spawn, wait and kill instead of being refused. It happens here,
     * in the shell's own bootstrap, rather than from kmain, because the slot
     * belongs to this subsystem and nothing else needs it. */
    proc_shell_start();

    /* The one console sink the kernel calls for a running program's write(1)/
     * write(2). It is a router, not a writer: a launched program's bytes go to
     * that program's own window, and everyone else's (the shell's own lines, the
     * boot demos, an un-launched child) go to this terminal. The serial port has
     * already received every byte before the router is ever reached — that is
     * what CI greps — so this changes only what a human at the screen sees. */
    vfs_set_console_sink(console_router);

    put_line("WINDCLONE - INVITE DE COMMANDES");
    put_line("TAPE HELP POUR LA LISTE DES COMMANDES.");
    put_line("");
    term.dirty = 1;
}

void shell_key(int c)
{
    if (c == '\n') {
        execute();
        return;
    }
    if (c == '\b') {
        if (input_len > 0)
            input[--input_len] = '\0';
        term.dirty = 1;
        return;
    }
    if (c >= 32 && c < 127 && input_len < SHELL_COLS - 1) {
        input[input_len++] = (char)c;
        input[input_len] = '\0';
        term.dirty = 1;
    }
}

void shell_draw(int x, int y, int width)
{
    (void)width;

    surf_draw(&term, x, y);

    /* Prompt + the line being typed, with a block cursor. Only the terminal
     * has these; a program's window is pure scrollback (shell_program_draw). */
    int py = y + SHELL_ROWS * LINE_H + 2;
    gfx_text(x, py, ">", gfx_rgb(0xEF, 0x44, 0x44), 1);
    gfx_text(x + 12, py, input, gfx_rgb(0xF8, 0xFA, 0xFC), 1);
    int cx = x + 12 + gfx_text_width(input, 1) + 2;
    gfx_fill(cx, py, 6, 8, gfx_rgb(0x22, 0xC5, 0x5E));
}

int shell_dirty(void)
{
    return term.dirty;
}

void shell_clear_dirty(void)
{
    term.dirty = 0;
}
