/* WindClone — command line: line editor, built-ins, and program launcher.
 *
 * Everything here runs from the idle loop, never from an IRQ: the keyboard
 * driver only queues bytes (see keyboard.c), so a `cat` that walks the
 * initrd or a `hello` that enters ring 3 cannot deadlock an interrupt.
 *
 * The scrollback is a fixed grid of characters, not a framebuffer: the
 * window can be repainted (dragged, overlapped) as often as it likes and
 * the text is still there. That is the same reason a real terminal keeps
 * cells and not pixels.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>

#include "shell.h"
#include "gfx.h"
#include "initrd.h"
#include "usermode.h"
#include "keyboard.h"
#include "serial.h"
#include "string.h"
#include "timer.h"

#define SHELL_ROWS 17
#define SHELL_COLS 62
#define LINE_H     11

static char rows[SHELL_ROWS][SHELL_COLS + 1];
static int  next_row;                 /* where the next line goes        */
static char input[SHELL_COLS + 1];
static int  input_len;
static int  dirty = 1;

/* --- grid helpers ----------------------------------------------------- */

static void grid_clear(void)
{
    for (int r = 0; r < SHELL_ROWS; r++)
        memset(rows[r], 0, sizeof rows[r]);
    next_row = 0;
}

static void scroll_up(void)
{
    for (int r = 1; r < SHELL_ROWS; r++)
        memcpy(rows[r - 1], rows[r], SHELL_COLS + 1);
    memset(rows[SHELL_ROWS - 1], 0, SHELL_COLS + 1);
    next_row = SHELL_ROWS - 1;
}

/* The 5x7 font has no lowercase: fold on the way into the grid so the
 * scrollback is already in the form the screen will show. */
static char upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static void put_line(const char *s)
{
    if (next_row >= SHELL_ROWS)
        scroll_up();

    int c = 0;
    for (; s[c] != '\0' && c < SHELL_COLS; c++)
        rows[next_row][c] = upper(s[c]);
    rows[next_row][c] = '\0';
    next_row++;
    dirty = 1;
}

/* "TEXT" + decimal, without dragging printf into the shell. */
static void put_line_num(const char *prefix, uint64_t value)
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
    put_line(buf);
}

/* --- commands --------------------------------------------------------- */

static void cmd_help(void)
{
    put_line("COMMANDES:");
    put_line("  HELP          CETTE LISTE");
    put_line("  LS            FICHIERS DE L'INITRD");
    put_line("  CAT <FICHIER> AFFICHE UN FICHIER");
    put_line("  HELLO         LANCE /BIN/HELLO EN RING 3");
    put_line("  ABOUT         VERSION ET UPTIME");
    put_line("  CLEAR         EFFACE L'ECRAN");
}

static void cmd_ls(void)
{
    unsigned n = initrd_count();
    if (n == 0) {
        put_line("INITRD VIDE OU ABSENT");
        return;
    }

    put_line_num("FICHIERS: ", n);

    for (unsigned i = 0; i < n; i++) {
        const struct initrd_file *f = initrd_at(i);
        char buf[SHELL_COLS + 1];
        int c = 0;

        buf[c++] = ' ';
        buf[c++] = '/';
        for (int k = 0; f->name[k] != '\0' && c < SHELL_COLS - 12; k++)
            buf[c++] = upper(f->name[k]);
        buf[c++] = ' ';
        buf[c++] = '(';

        char digits[21];
        int d = 0;
        uint64_t size = f->size;
        do {
            digits[d++] = (char)('0' + (size % 10));
            size /= 10;
        } while (size != 0 && d < 20);
        while (d > 0 && c < SHELL_COLS - 4)
            buf[c++] = digits[--d];

        buf[c++] = ' ';
        buf[c++] = 'B';
        buf[c++] = ')';
        buf[c] = '\0';
        put_line(buf);
    }
}

static void cmd_cat(const char *name)
{
    const struct initrd_file *f = initrd_find(name);
    if (f == NULL) {
        put_line("FICHIER INTROUVABLE");
        return;
    }

    /* Stream the file into the grid one '\n' at a time: text files only,
     * and long lines are cut rather than wrapped (wrapping needs a width,
     * the grid is meant to be dumb). */
    char line[SHELL_COLS + 1];
    int c = 0;
    for (uint64_t i = 0; i < f->size; i++) {
        char ch = (char)f->data[i];
        if (ch == '\r')
            continue;
        if (ch == '\n' || c >= SHELL_COLS) {
            line[c] = '\0';
            put_line(line);
            c = 0;
            if (ch != '\n')
                line[c++] = ch;
            continue;
        }
        line[c++] = ch;
    }
    if (c > 0) {
        line[c] = '\0';
        put_line(line);
    }
}

static void cmd_hello(void)
{
    /* This line is the CI contract for typing: to reach it, the keyboard
     * must have decoded h-e-l-l-o in order through the real IRQ path. */
    serial_write("shell: running 'hello' from the prompt\n");
    put_line("EXECUTION DE /BIN/HELLO EN RING 3...");
    int code = -1;
    if (usermode_run("/bin/hello", &code) == 0) {
        put_line_num("TERMINE, CODE DE SORTIE: ", (uint64_t)(code < 0 ? 0 : (unsigned)code));
        serial_write("shell: hello ran and returned\n");
    } else {
        put_line("IMPOSSIBLE DE LANCER /BIN/HELLO");
    }
}

static void cmd_about(void)
{
    put_line("WINDCLONE 0.0.4 - X86_64");
    put_line("KERNEL: C, ASM, SANS LIBC");
    put_line("RING 3 + INITRD + FRAMEBUFFER: OK");
    put_line_num("UPTIME: ", timer_ticks() / 100);
    put_line("SHELL: CE QUE TU TAPES EST LU PAR L'IRQ1");
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

    if (input_len == 0) {
        /* bare Enter: nothing to do */
    } else if (strcmp(input, "help") == 0) {
        cmd_help();
    } else if (strcmp(input, "ls") == 0) {
        cmd_ls();
    } else if (strncmp(input, "cat ", 4) == 0) {
        cmd_cat(input + 4);
    } else if (strcmp(input, "hello") == 0) {
        cmd_hello();
    } else if (strcmp(input, "about") == 0) {
        cmd_about();
    } else if (strcmp(input, "clear") == 0) {
        grid_clear();
    } else {
        put_line("COMMANDE INCONNUE. TAPE HELP.");
    }

    input_len = 0;
    input[0] = '\0';
    dirty = 1;
}

/* --- public ------------------------------------------------------------ */

void shell_init(void)
{
    grid_clear();
    put_line("WINDCLONE - INVITE DE COMMANDES");
    put_line("TAPE HELP POUR LA LISTE DES COMMANDES.");
    put_line("");
    dirty = 1;
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
        dirty = 1;
        return;
    }
    if (c >= 32 && c < 127 && input_len < SHELL_COLS - 1) {
        input[input_len++] = (char)c;
        input[input_len] = '\0';
        dirty = 1;
    }
}

void shell_draw(int x, int y, int width)
{
    (void)width;

    for (int r = 0; r < SHELL_ROWS; r++)
        if (rows[r][0] != '\0')
            gfx_text(x, y + r * LINE_H, rows[r], gfx_rgb(0x22, 0xC5, 0x5E), 1);

    /* Prompt + the line being typed, with a block cursor. */
    int py = y + SHELL_ROWS * LINE_H + 2;
    gfx_text(x, py, ">", gfx_rgb(0xEF, 0x44, 0x44), 1);
    gfx_text(x + 12, py, input, gfx_rgb(0xF8, 0xFA, 0xFC), 1);
    int cx = x + 12 + gfx_text_width(input, 1) + 2;
    gfx_fill(cx, py, 6, 8, gfx_rgb(0x22, 0xC5, 0x5E));
}

int shell_dirty(void)
{
    return dirty;
}

void shell_clear_dirty(void)
{
    dirty = 0;
}
